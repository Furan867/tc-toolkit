"""run.py 的编排单测：不启动游戏、不编译、不采样。"""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import os
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from datetime import datetime
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("hotspot_run", HERE.parent / "run.py")
assert SPEC and SPEC.loader
run = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(run)


def setUpModule() -> None:
    """默认日志目录换成临时目录：调 main() 的用例不该往仓库 work/tmp 里写日志。"""
    temp = tempfile.TemporaryDirectory()
    unittest.addModuleCleanup(temp.cleanup)
    patcher = mock.patch.object(run, "LOG_DIR", Path(temp.name))
    patcher.start()
    unittest.addModuleCleanup(patcher.stop)


class ParseArgsTest(unittest.TestCase):
    def test_defaults(self):
        opts = run.parse_args(["--src", "x.si"])
        self.assertEqual(opts.window, 10)
        self.assertIsNone(opts.pid)
        self.assertFalse(opts.clean)

    def test_overrides(self):
        opts = run.parse_args(
            ["--src", "x.si", "--window", "3", "--pid", "1", "--clean"]
        )
        self.assertEqual(opts.window, 3)
        self.assertEqual(opts.pid, 1)
        self.assertTrue(opts.clean)

    def test_pause_on_exit_is_opt_in(self):
        # 管理员终端直接跑不带它就不该停：等回车只能来自显式要求或提权重启那次自动加
        self.assertFalse(run.parse_args(["--src", "x.si"]).pause_on_exit)
        self.assertTrue(
            run.parse_args(["--src", "x.si", "--pause-on-exit"]).pause_on_exit
        )


class GameRunningTest(unittest.TestCase):
    def test_zero_is_not_running(self):
        with mock.patch.object(run, "_run", return_value="sim_speed        0"):
            self.assertFalse(run._game_running())

    def test_nonzero_is_running(self):
        with mock.patch.object(
            run, "_run", return_value="sim_speed    45.4 MHz"
        ):
            self.assertTrue(run._game_running())


class PreflightTest(unittest.TestCase):
    def test_missing_config(self):
        with tempfile.TemporaryDirectory() as tmp:
            opts = run.parse_args(
                ["--src", "x.si", "--config", str(Path(tmp) / "no.toml")]
            )
            with self.assertRaises(run.StageError) as caught:
                run.preflight(opts)
            self.assertEqual(caught.exception.stage, "preflight")
            self.assertIn("config", str(caught.exception))


class SampleStageTest(unittest.TestCase):
    def test_script_and_stats(self):
        opts = run.parse_args(
            ["--src", "x.si", "--window", "5", "--pid", "42"]
        )
        output = "窗口 5 s：样本 100（bucket0 90 / bucket1 5 / bucket2 5）"
        captured = []

        def fake_run(command, *, stage, input_text=None, timeout=120):
            text = input_text or ""
            captured.append(text)
            save_line = next(
                line for line in text.splitlines() if line.startswith("save ")
            )
            run_dir = Path(save_line.split(" ", 1)[1])
            run_dir.mkdir(parents=True)
            (run_dir / "run.toml").write_text("")
            (run_dir / "samples.tsv").write_text("")
            return output

        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(
            run, "_run", side_effect=fake_run
        ):
            stats = run.sample_stage(
                opts, Path("cfg.toml"), Path(tmp) / "blob.bin", Path(tmp) / "run"
            )
            text = captured[0]
            self.assertIn("open", text)
            self.assertIn("--pid 42", text)
            self.assertIn("window 5", text)
            self.assertIn("save", text)
            self.assertIn("quit", text)
            self.assertEqual(stats["samples"], 100)
            self.assertEqual(stats["bucket0"], 90)

    def test_zero_bucket0_fails(self):
        opts = run.parse_args(["--src", "x.si"])
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(
            run, "_run", return_value="样本 10（bucket0 0 / bucket1 5 / bucket2 5）"
        ):
            with self.assertRaises(run.StageError) as caught:
                run.sample_stage(
                    opts, Path("cfg.toml"), Path(tmp) / "b", Path(tmp) / "r"
                )
            self.assertEqual(caught.exception.stage, "sample")


class MainTest(unittest.TestCase):
    def setUp(self):
        # etw 提权会真弹 UAC 并真起一个进程：这些用例只验编排，把提权整体替掉。
        patcher = mock.patch.object(run, "maybe_relaunch", return_value=None)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_preflight_failure_returns_one(self):
        with mock.patch.object(
            run,
            "preflight",
            side_effect=run.StageError("preflight", "缺 config"),
        ):
            self.assertEqual(run.main(["--src", "x.si"]), 1)

    def test_compile_failure_stops_before_sample(self):
        with mock.patch.object(
            run, "preflight", return_value=(Path("cfg"), Path("x.si"))
        ), mock.patch.object(
            run,
            "compile_stage",
            side_effect=run.StageError("compile", "boom"),
        ), mock.patch.object(run, "sample_stage") as sample, mock.patch.object(
            run, "stop_stage"
        ) as stop:
            self.assertEqual(run.main(["--src", "x.si"]), 1)
            sample.assert_not_called()
            stop.assert_called_once()

    def test_success_stops_sim(self):
        stats = {"samples": 10, "bucket0": 10, "bucket1": 0, "bucket2": 0}
        with mock.patch.object(
            run, "preflight", return_value=(Path("cfg"), Path("x.si"))
        ), mock.patch.object(
            run, "compile_stage", return_value={"blob": Path("blob.bin")}
        ), mock.patch.object(
            run, "sidecar_stage", return_value=Path("sidecar.json")
        ), mock.patch.object(
            run, "sample_stage", return_value=stats
        ), mock.patch.object(
            run, "report_stage", return_value=Path("report.html")
        ), mock.patch.object(run, "stop_stage") as stop:
            self.assertEqual(run.main(["--src", "x.si"]), 0)
            stop.assert_called_once()

    def test_keep_running_skips_stop(self):
        stats = {"samples": 10, "bucket0": 10, "bucket1": 0, "bucket2": 0}
        with mock.patch.object(
            run, "preflight", return_value=(Path("cfg"), Path("x.si"))
        ), mock.patch.object(
            run, "compile_stage", return_value={"blob": Path("blob.bin")}
        ), mock.patch.object(
            run, "sidecar_stage", return_value=Path("sidecar.json")
        ), mock.patch.object(
            run, "sample_stage", return_value=stats
        ), mock.patch.object(
            run, "report_stage", return_value=Path("report.html")
        ), mock.patch.object(run, "stop_stage") as stop:
            self.assertEqual(
                run.main(["--src", "x.si", "--keep-running"]), 0
            )
            stop.assert_not_called()


RUNNING = "PID 4242   模块基址 0x140000000\nsim_speed    45.400 MHz   (45400000 Hz)"
STOPPED = "PID 4242   模块基址 0x140000000\nsim_speed        0        ← 仿真没在跑（哨兵值）"
GAME_GONE = "Turing Complete.exe 没在运行\n"


def _script(states: list[str]):
    """`_run` 替身：read_freq 按 states 依次吐输出（最后一个重复用），其余命令都成功。"""
    commands: list[list[str]] = []
    pending = list(states)

    def fake(command, *, stage, input_text=None, timeout=120):
        parts = [str(part) for part in command]
        commands.append(parts)
        if any(part.endswith("read_freq.py") for part in parts):
            return pending.pop(0) if len(pending) > 1 else pending[0]
        return ""

    return commands, fake


def _simctl_calls(commands: list[list[str]]) -> list[list[str]]:
    return [parts for parts in commands if any(part.endswith("simctl.py") for part in parts)]


class _MainHarness(unittest.TestCase):
    """把工具链与四个阶段都替掉，只留 preflight 与自动启停这段真逻辑。"""

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.tmp = Path(temp.name)
        self.config = self.tmp / "config.toml"
        self.config.write_text("", encoding="utf-8")
        self.src = self.tmp / "circuit.si"
        self.src.write_text("", encoding="utf-8")
        for name in ("SIMPLEXC", "CONSUMER"):
            fake_tool = self.tmp / f"{name.lower()}.exe"
            fake_tool.write_text("", encoding="utf-8")
            patcher = mock.patch.object(run, name, fake_tool)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.blob = self.tmp / "blob.bin"
        self.blob.write_bytes(b"artifact")
        self.run_dir = self.tmp / "run"
        patcher = mock.patch.object(run, "maybe_relaunch", return_value=None)
        patcher.start()
        self.addCleanup(patcher.stop)

    def argv(self, *extra: str) -> list[str]:
        return ["--config", str(self.config), "--out", str(self.run_dir), *extra]

    def run_main(self, argv: list[str]):
        stats = {"samples": 100, "bucket0": 90, "bucket1": 5, "bucket2": 5}
        with mock.patch.object(
            run, "compile_stage", return_value={"blob": self.blob}
        ) as compile_, mock.patch.object(
            run, "sidecar_stage", return_value=self.tmp / "sidecar.json"
        ), mock.patch.object(
            run, "sample_stage", return_value=stats
        ) as sample, mock.patch.object(
            run, "report_stage", return_value=self.tmp / "report.html"
        ):
            code = run.main(argv)
        return code, compile_, sample


class AutoModeTest(_MainHarness):
    def _product(self, num: str = "042"):
        src = self.tmp / f"hook_src_{num}.si"
        src.write_text("", encoding="utf-8")
        out = self.tmp / f"hook_out_{num}.bin"
        out.write_text("", encoding="utf-8")
        return run.tc_common.HookProduct(
            num=num, src=src, out=out, mtime=1.0, stdlib=False, impl="ours"
        )

    def test_no_src_uses_latest_hook_product(self):
        product = self._product()
        commands, fake = _script([RUNNING])
        with mock.patch.object(
            run.tc_common, "latest_hook_product", return_value=product
        ) as latest, mock.patch.object(run, "_run", side_effect=fake):
            # 不给 --out：这条同时验无参模式的 run 目录命名
            code, compile_, sample = self.run_main(["--config", str(self.config)])

        self.assertEqual(code, 0)
        latest.assert_called_once_with()
        compile_.assert_called_once_with(mock.ANY, mock.ANY, product.src)
        self.assertTrue(sample.call_args.args[3].name.endswith("_hook042_t10"))
        self.assertEqual(_simctl_calls(commands), [])

    def test_no_src_without_hook_product_fails(self):
        with mock.patch.object(
            run.tc_common, "latest_hook_product", return_value=None
        ):
            code, compile_, _ = self.run_main(self.argv())

        self.assertEqual(code, 1)
        compile_.assert_not_called()

    def test_starts_and_stops_sim_when_stopped(self):
        commands, fake = _script([STOPPED, RUNNING])
        with mock.patch.object(run, "_run", side_effect=fake):
            code, _, _ = self.run_main(self.argv("--src", str(self.src)))

        self.assertEqual(code, 0)
        self.assertEqual(
            [parts[-1] for parts in _simctl_calls(commands)], ["run", "stop"]
        )

    def test_leaves_running_sim_alone(self):
        commands, fake = _script([RUNNING])
        with mock.patch.object(run, "_run", side_effect=fake):
            code, _, _ = self.run_main(self.argv("--src", str(self.src)))

        self.assertEqual(code, 0)
        self.assertEqual(_simctl_calls(commands), [])

    def test_no_sim_control_leaves_sim_alone(self):
        commands, fake = _script([RUNNING])
        with mock.patch.object(run, "_run", side_effect=fake):
            code, _, _ = self.run_main(
                self.argv("--src", str(self.src), "--no-sim-control")
            )

        self.assertEqual(code, 0)
        self.assertEqual(_simctl_calls(commands), [])

    def test_no_sim_control_with_stopped_sim_fails(self):
        commands, fake = _script([STOPPED])
        with mock.patch.object(run, "_run", side_effect=fake):
            code, _, sample = self.run_main(
                self.argv("--src", str(self.src), "--no-sim-control")
            )

        self.assertEqual(code, 1)
        self.assertEqual(_simctl_calls(commands), [])
        sample.assert_not_called()

    def test_game_not_running_fails(self):
        _, fake = _script([GAME_GONE])
        with mock.patch.object(run, "_run", side_effect=fake):
            code, compile_, _ = self.run_main(self.argv("--src", str(self.src)))

        self.assertEqual(code, 1)
        compile_.assert_not_called()


class ArtifactModeTest(_MainHarness):
    def test_artifact_alone_fails(self):
        with mock.patch.object(run, "_run", side_effect=_script([RUNNING])[1]):
            code, compile_, _ = self.run_main(self.argv("--artifact", str(self.blob)))

        self.assertEqual(code, 1)
        compile_.assert_not_called()

    def test_sidecar_sha_mismatch_fails(self):
        sidecar = self.tmp / "sidecar.json"
        sidecar.write_text(
            json.dumps({"artifact": {"sha256": "0" * 64}}), encoding="utf-8"
        )
        with mock.patch.object(run, "_run", side_effect=_script([RUNNING])[1]):
            code, compile_, _ = self.run_main(
                self.argv("--artifact", str(self.blob), "--sidecar", str(sidecar))
            )

        self.assertEqual(code, 1)
        compile_.assert_not_called()


class SidecarCacheTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.tmp = Path(temp.name)
        self.cache = self.tmp / "cache"
        self.blob = self.tmp / "blob.bin"
        self.blob.write_bytes(b"artifact")
        self.key = hashlib.sha256(b"artifact").hexdigest()
        self.paths = {
            "blob": self.blob,
            "obj": self.tmp / "module.obj",
            "elements": self.tmp / "el.tsv",
            "offsets": self.tmp / "blob.bin.offsets",
            "stubs": self.tmp / "blob.bin.stubs",
        }
        self._write_offsets(0)

    def _write_offsets(self, line_offset: int) -> None:
        self.paths["offsets"].write_text(json.dumps({"line_offset": line_offset}),
                                        encoding="utf-8")

    def _write_sidecar(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps({"artifact": {"len": 8, "sha256": self.key}}),
            encoding="utf-8",
        )

    def test_cache_hit_skips_make_sidecar(self):
        self._write_sidecar(self.cache / f"{self.key}.lo0.fv1" / "sidecar.json")

        with mock.patch.object(run, "_run") as run_mock:
            sidecar = run.sidecar_stage(self.paths, cache_dir=self.cache)

        run_mock.assert_not_called()
        self.assertEqual(sidecar, self.tmp / "sidecar.json")
        self.assertEqual(
            json.loads(sidecar.read_text(encoding="utf-8"))["artifact"]["sha256"],
            self.key,
        )

    def test_cache_key_follows_line_offset(self):
        # 只按 blob sha 命中会把旧口径（拼接后行号）的 sidecar 当成新的
        self._write_sidecar(self.cache / f"{self.key}.lo0.fv1" / "sidecar.json")
        self._write_offsets(1503)

        with mock.patch.object(run, "_run", side_effect=self._fake_make_sidecar) as run_mock:
            sidecar = run.sidecar_stage(self.paths, cache_dir=self.cache)

        run_mock.assert_called_once()
        self.assertTrue((self.cache / f"{self.key}.lo1503.fv1" / "sidecar.json").is_file())
        self.assertTrue(sidecar.is_file())

    def _fake_make_sidecar(self, command, *, stage, input_text=None, timeout=120):
        self._write_sidecar(Path(str(command[command.index("--out") + 1])))
        return ""

    def test_cache_miss_runs_and_writes_cache(self):
        with mock.patch.object(run, "_run", side_effect=self._fake_make_sidecar) as run_mock:
            sidecar = run.sidecar_stage(self.paths, cache_dir=self.cache)

        self.assertEqual(run_mock.call_args.kwargs["stage"], "sidecar")
        self.assertTrue(sidecar.is_file())
        self.assertTrue((self.cache / f"{self.key}.lo0.fv1" / "sidecar.json").is_file())


class CleanTargetsTest(unittest.TestCase):
    """不 patch 路径：验 clean 默认指向的位置，且工作输入 work/hook 不在里面。"""

    def test_targets_are_cache_work_tmp_and_runs(self):
        self.assertEqual(
            run.SIDECAR_CACHE, run.ROOT / "hotspot/workspace/cache/sidecar"
        )
        self.assertEqual(run.WORK_TMP, run.ROOT / "work/tmp/hotspot_run")
        self.assertEqual(run.RUNS_DIR, run.ROOT / "hotspot/workspace/runs")
        hook = run.ROOT / "work/hook"
        for path in (run.SIDECAR_CACHE, run.WORK_TMP, run.RUNS_DIR):
            self.assertNotIn(hook, path.parents)
            self.assertNotEqual(path, hook)


class CleanTest(unittest.TestCase):
    """clean 子命令：拿临时目录假装缓存/中间产物/run 目录，验删哪几个、报什么数。"""

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.tmp = Path(temp.name)
        self.cache = self.tmp / "cache/sidecar"
        self.work_tmp = self.tmp / "work/tmp/hotspot_run"
        self.runs = self.tmp / "runs"
        for name, value in (
            ("SIDECAR_CACHE", self.cache),
            ("WORK_TMP", self.work_tmp),
            ("RUNS_DIR", self.runs),
        ):
            patcher = mock.patch.object(run, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def _populate(self) -> None:
        """缓存两个 key、中间产物一份、runs 里一份报表；合计 4 个目录 / 3 个文件 / 13 字节。"""
        for name in ("key1", "key2"):
            (self.cache / name).mkdir(parents=True)
            (self.cache / name / "sidecar.json").write_bytes(b"1234")
        self.work_tmp.mkdir(parents=True)
        (self.work_tmp / "blob.bin").write_bytes(b"12345")
        (self.runs / "2026_run").mkdir(parents=True)
        (self.runs / "2026_run/report.html").write_bytes(b"<html>")

    def _main(self, argv: list[str]) -> tuple[int, str]:
        out = io.StringIO()
        with redirect_stdout(out):
            code = run.main(argv)
        return code, out.getvalue()

    def test_clean_is_positional_subcommand(self):
        self.assertEqual(run.parse_args(["clean"]).command, "clean")
        self.assertIsNone(run.parse_args(["--src", "x.si"]).command)
        self.assertIsNone(run.parse_args([]).command)

    def test_default_cleans_cache_and_work_tmp_but_not_runs(self):
        self._populate()
        with mock.patch.object(
            run, "preflight", side_effect=AssertionError("clean 不该跑 preflight")
        ):
            code, out = self._main(["clean"])

        self.assertEqual(code, 0)
        self.assertFalse(self.cache.exists())
        self.assertFalse(self.work_tmp.exists())
        self.assertTrue((self.runs / "2026_run/report.html").is_file())
        self.assertIn("4 个目录", out)
        self.assertIn("3 个文件", out)
        self.assertIn("13 字节", out)

    def test_dry_run_keeps_everything(self):
        self._populate()
        with mock.patch.object(
            run, "preflight", side_effect=AssertionError("clean 不该跑 preflight")
        ):
            code, out = self._main(["clean", "--dry-run"])

        self.assertEqual(code, 0)
        self.assertTrue((self.cache / "key1/sidecar.json").is_file())
        self.assertTrue((self.work_tmp / "blob.bin").is_file())
        self.assertIn("[dry-run]", out)
        self.assertIn("4 个目录", out)
        self.assertIn("3 个文件", out)
        self.assertIn("13 字节", out)
        self.assertNotIn("已删", out)

    def test_runs_flag_cleans_runs(self):
        self._populate()
        with mock.patch.object(
            run, "preflight", side_effect=AssertionError("clean 不该跑 preflight")
        ):
            code, _ = self._main(["clean", "--runs"])

        self.assertEqual(code, 0)
        self.assertFalse(self.cache.exists())
        self.assertFalse(self.work_tmp.exists())
        self.assertFalse(self.runs.exists())

    def test_missing_cache_reports_and_returns_zero(self):
        code, out = self._main(["clean"])

        self.assertEqual(code, 0)
        self.assertIn("没有缓存", out)
        self.assertNotIn("已删", out)


class SamplerBackendTest(unittest.TestCase):
    """提权判据要读 config.toml 的 sampler.backend；读不到就跟 consumer 一样当 etw。"""

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.tmp = Path(temp.name)

    def _write(self, text: str) -> Path:
        path = self.tmp / "config.toml"
        path.write_text(text, encoding="utf-8")
        return path

    def test_missing_file_falls_back_to_etw(self):
        self.assertEqual(run.sampler_backend(self.tmp / "no.toml"), "etw")

    def test_reads_sampler_section(self):
        path = self._write(
            '[game]\nexe_name = "Turing Complete.exe"\n'
            '[sampler]\nbackend = "suspend"      # 免管理员\n'
        )
        self.assertEqual(run.sampler_backend(path), "suspend")

    def test_ignores_backend_in_other_sections(self):
        path = self._write('[etw]\nbackend = "suspend"\n[sampler]\ninterval_ms = 1\n')
        self.assertEqual(run.sampler_backend(path), "etw")


class ElevationTest(unittest.TestCase):
    """maybe_relaunch：etw + 非管理员才弹 UAC；suspend / 已是管理员一概不弹。"""

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.tmp = Path(temp.name)

    def _config(self, backend: str | None) -> Path:
        path = self.tmp / "config.toml"
        if backend is not None:
            path.write_text(f'[sampler]\nbackend = "{backend}"\n', encoding="utf-8")
        return path

    def _relaunch(
        self,
        backend: str | None,
        *,
        elevated: bool,
        shell_result: int = 33,
        arguments: list[str] | None = None,
        log_file: Path | None = None,
    ) -> tuple[int | None, list[tuple[Path, str, Path]], str]:
        """跑一次 maybe_relaunch：(退出码, ShellExecuteW 的调用, stderr)。"""
        calls: list[tuple[Path, str, Path]] = []

        def fake_shell(target, parameters, directory):
            calls.append((target, parameters, directory))
            return shell_result

        stderr = io.StringIO()
        with mock.patch.object(run, "is_elevated", return_value=elevated), mock.patch.object(
            run, "_shell_execute_runas", side_effect=fake_shell
        ), redirect_stderr(stderr), redirect_stdout(io.StringIO()):
            code = run.maybe_relaunch(
                self._config(backend),
                (
                    ["--window", "3", "--out", "my dir/run"]
                    if arguments is None
                    else arguments
                ),
                log_file=log_file,
            )
        return code, calls, stderr.getvalue()

    def test_etw_without_admin_relaunches_with_args_and_cwd(self):
        code, calls, _ = self._relaunch("etw", elevated=False)

        self.assertEqual(code, 0)  # 已交给提权进程，原进程正常退出
        self.assertEqual(len(calls), 1)
        target, parameters, directory = calls[0]
        self.assertEqual(target, Path(run.sys.executable))
        self.assertEqual(directory, Path.cwd())
        # 参数要原样带上（合空格的路径要被引号包住，不然会被拆成两段）
        self.assertIn(str(run.RUN_SCRIPT), parameters)
        self.assertIn("--window 3", parameters)
        self.assertIn('"my dir/run"', parameters)

    def test_suspend_without_admin_never_relaunches(self):
        code, calls, _ = self._relaunch("suspend", elevated=False)

        self.assertIsNone(code)
        self.assertEqual(calls, [])

    def test_relaunch_arguments_carry_pause_on_exit(self):
        # 提权重启换来一个新控制台，它跟着进程一起关：参数得带上让新进程停一下
        _, calls, _ = self._relaunch("etw", elevated=False)

        self.assertIn(run.PAUSE_ON_EXIT, calls[0][1].split())

    def test_relaunch_does_not_duplicate_pause_on_exit(self):
        _, calls, _ = self._relaunch(
            "etw", elevated=False, arguments=["--window", "3", run.PAUSE_ON_EXIT]
        )

        self.assertEqual(calls[0][1].split().count(run.PAUSE_ON_EXIT), 1)

    def test_relaunch_carries_log_file(self):
        # 子进程要从原进程已经建好的那份日志接着写，不能自己再新建一份
        log_file = self.tmp / "run.log"
        _, calls, _ = self._relaunch("etw", elevated=False, log_file=log_file)

        self.assertIn(run.LOG_FILE_OPTION, calls[0][1].split())
        self.assertIn(str(log_file), calls[0][1])

    def test_relaunch_does_not_duplicate_log_file(self):
        log_file = self.tmp / "run.log"
        for existing in (
            [run.LOG_FILE_OPTION, "old.log"],
            [f"{run.LOG_FILE_OPTION}=old.log"],
        ):
            _, calls, _ = self._relaunch(
                "etw", elevated=False, arguments=existing, log_file=log_file
            )
            self.assertEqual(calls[0][1].split().count(run.LOG_FILE_OPTION), 1)
            self.assertNotIn("old.log", calls[0][1])

    def test_admin_never_relaunches(self):
        # consumer 已经是管理员：再提一次会多一个进程、还会把管道换成新控制台
        for backend in ("etw", "suspend"):
            code, calls, _ = self._relaunch(backend, elevated=True)
            self.assertIsNone(code)
            self.assertEqual(calls, [])

    def test_missing_config_means_etw(self):
        code, calls, _ = self._relaunch(None, elevated=False)

        self.assertEqual(code, 0)
        self.assertEqual(len(calls), 1)

    def test_cancelled_reports_and_returns_nonzero(self):
        for shell_result in (0, run.SE_ERR_ACCESSDENIED, run.ERROR_CANCELLED):
            code, _, stderr = self._relaunch(
                "etw", elevated=False, shell_result=shell_result
            )
            self.assertEqual(code, 1)
            self.assertIn("已取消提权", stderr)

    def test_other_failure_reports_the_code(self):
        code, _, stderr = self._relaunch("etw", elevated=False, shell_result=31)

        self.assertEqual(code, 1)
        self.assertIn("31", stderr)

    def test_main_stops_before_preflight_when_relaunching(self):
        with mock.patch.object(run, "maybe_relaunch", return_value=0), mock.patch.object(
            run, "preflight", side_effect=AssertionError("提权后不该接着跑")
        ):
            self.assertEqual(run.main(["--src", "x.si"]), 0)


class _FakeStdin(io.StringIO):
    """假 stdin：`tty` 决定 isatty()，其余按 StringIO（能看读了多少）。"""

    def __init__(self, text: str, tty: bool) -> None:
        super().__init__(text)
        self._tty = tty

    def isatty(self) -> bool:
        return self._tty


class PauseOnExitTest(unittest.TestCase):
    """提权后的进程活在一个随退出就关掉的新控制台里：要停一下，但管道调用不许停。"""

    def test_tty_waits_for_one_enter(self):
        stdin = _FakeStdin("\n剩下的\n", tty=True)
        out = io.StringIO()
        with mock.patch("sys.stdin", stdin), redirect_stdout(out):
            run.pause_before_exit(run.parse_args(["--pause-on-exit"]))

        self.assertIn("按回车退出", out.getvalue())
        # 只吃一行：后面的内容还在（吃光会把调用方剩下的输入弄没）
        self.assertEqual(stdin.readline(), "剩下的\n")

    def test_non_tty_does_not_wait(self):
        stdin = _FakeStdin("\n", tty=False)
        out = io.StringIO()
        with mock.patch("sys.stdin", stdin), redirect_stdout(out):
            run.pause_before_exit(run.parse_args(["--pause-on-exit"]))

        self.assertNotIn("按回车退出", out.getvalue())
        self.assertEqual(stdin.tell(), 0)  # 一个字节都没读

    def test_without_flag_never_waits(self):
        stdin = _FakeStdin("\n", tty=True)
        out = io.StringIO()
        with mock.patch("sys.stdin", stdin), redirect_stdout(out):
            run.pause_before_exit(run.parse_args([]))

        self.assertNotIn("按回车退出", out.getvalue())
        self.assertEqual(stdin.tell(), 0)

    def test_main_waits_after_failure_when_flagged(self):
        out = io.StringIO()
        error = io.StringIO()
        with (
            mock.patch.object(run, "maybe_relaunch", return_value=None),
            mock.patch.object(
                run, "preflight", side_effect=run.StageError("preflight", "缺 config")
            ),
            mock.patch("sys.stdin", _FakeStdin("\n", tty=True)),
            redirect_stdout(out),
            redirect_stderr(error),
        ):
            code = run.main(["--src", "x.si", "--pause-on-exit"])

        self.assertEqual(code, 1)
        self.assertIn("按回车退出", out.getvalue())
        self.assertNotIn("按回车退出", error.getvalue())

    def test_main_does_not_wait_when_handing_over_to_elevated_process(self):
        # 原进程只是交活：该停的是新控制台里那个，这里停了等于白占一个窗口
        stdin = _FakeStdin("\n", tty=True)
        out = io.StringIO()
        with mock.patch.object(run, "maybe_relaunch", return_value=0), mock.patch.object(
            run, "preflight", side_effect=AssertionError("提权后不该接着跑")
        ), mock.patch("sys.stdin", stdin), redirect_stdout(out):
            self.assertEqual(run.main(["--src", "x.si"]), 0)

        self.assertNotIn("按回车退出", out.getvalue())
        self.assertEqual(stdin.tell(), 0)


class RunLogTest(unittest.TestCase):
    """每次子进程调用都要留下阶段、命令、输入、退出码、stdout、stderr。"""

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.tmp = Path(temp.name)
        self.log_path = self.tmp / "run.log"
        patcher = mock.patch.object(run, "_LOG", run.RunLog(self.log_path))
        patcher.start()
        self.addCleanup(patcher.stop)

    def log_text(self) -> str:
        return self.log_path.read_text(encoding="utf-8")

    def test_run_records_command_input_exit_code_and_outputs(self):
        done = subprocess.CompletedProcess([], 0, "样本 100（bucket0 90）", "")
        script = "open blob.bin\nwindow 5\nsave run\nquit\n"
        with mock.patch.object(run.subprocess, "run", return_value=done):
            output = run._run(
                ["hotspot-consumer.exe", "--config", "config.toml"],
                stage="sample",
                input_text=script,
            )

        self.assertEqual(output, "样本 100（bucket0 90）")
        text = self.log_text()
        self.assertIn(
            "[sample] 调用：hotspot-consumer.exe --config config.toml", text
        )
        self.assertIn("window 5", text)
        self.assertIn("退出码 0", text)
        self.assertIn("样本 100（bucket0 90）", text)
        self.assertIn("[sample] stderr", text)

    def test_failure_records_exit_code_and_stderr(self):
        done = subprocess.CompletedProcess([], 3, "", "第一行\n第二行错误")
        with mock.patch.object(
            run.subprocess, "run", return_value=done
        ), self.assertRaises(run.StageError) as caught:
            run._run(["tools/x.py"], stage="compile")

        self.assertEqual(caught.exception.stage, "compile")
        text = self.log_text()
        self.assertIn("退出码 3", text)
        self.assertIn("第二行错误", text)

    def test_timeout_is_recorded(self):
        with mock.patch.object(
            run.subprocess, "run", side_effect=subprocess.TimeoutExpired(["x"], 7)
        ), self.assertRaises(run.StageError):
            run._run(["tools/x.py"], stage="report", timeout=7)

        self.assertIn("超时（7 s）", self.log_text())

    def test_start_records_argv_cwd_pid_and_backend(self):
        config = self.tmp / "config.toml"
        config.write_text('[sampler]\nbackend = "suspend"\n', encoding="utf-8")
        opts = run.parse_args(
            ["--src", "x.si", "--config", str(config), run.LOG_FILE_OPTION,
             str(self.log_path)]
        )
        path = run.open_run_log(opts, ["--src", "x.si"])

        self.assertEqual(path, self.log_path)
        text = self.log_text()
        self.assertIn("argv：", text)
        self.assertIn(self.tmp.name, text)  # argv 里带着原样的 --log-file 路径
        self.assertIn(f"cwd：{Path.cwd()}", text)
        self.assertIn(f"pid：{os.getpid()}", text)
        self.assertIn("backend：suspend", text)

    def test_stage_error_records_stage_command_and_error(self):
        error = run.StageError(
            "compile", "退出码 2", ["simplexc_cli.exe", "--src", "x.si"], "错误尾部"
        )
        with mock.patch.object(
            run, "maybe_relaunch", return_value=None
        ), mock.patch.object(run, "preflight", side_effect=error):
            code = run.main([
                "--src", "x.si", run.LOG_FILE_OPTION, str(self.log_path)
            ])

        self.assertEqual(code, 1)
        text = self.log_text()
        self.assertIn("[compile] 失败：退出码 2", text)
        self.assertIn("[compile] 命令：simplexc_cli.exe --src x.si", text)
        self.assertIn("错误尾部", text)
        self.assertIn("退出码 1", text)  # 最终结果也在日志里

    def test_success_records_final_result(self):
        stats = {"samples": 10, "bucket0": 10, "bucket1": 0, "bucket2": 0}
        with (
            mock.patch.object(run, "maybe_relaunch", return_value=None),
            mock.patch.object(
                run, "preflight", return_value=(Path("cfg"), Path("x.si"))
            ),
            mock.patch.object(
                run, "compile_stage", return_value={"blob": Path("blob.bin")}
            ),
            mock.patch.object(
                run, "sidecar_stage", return_value=Path("sidecar.json")
            ),
            mock.patch.object(run, "sample_stage", return_value=stats),
            mock.patch.object(
                run, "report_stage", return_value=Path("report.html")
            ),
            mock.patch.object(run, "stop_stage"),
        ):
            code = run.main([
                "--src", "x.si", run.LOG_FILE_OPTION, str(self.log_path)
            ])

        self.assertEqual(code, 0)
        text = self.log_text()
        self.assertIn("完成：", text)
        self.assertIn("样本 10", text)
        self.assertIn("退出码 0", text)

    def test_write_failure_does_not_affect_the_run(self):
        # 日志路径指向一个目录：写必然失败，但分析流程与退出码不能跟着变
        directory = self.tmp / "as-dir"
        directory.mkdir()
        run.RunLog(directory).write("写不进去也不抛")
        with mock.patch.object(
            run, "maybe_relaunch", return_value=None
        ), mock.patch.object(
            run, "preflight", side_effect=run.StageError("preflight", "缺 config")
        ):
            code = run.main([
                "--src", "x.si", run.LOG_FILE_OPTION, str(directory)
            ])

        self.assertEqual(code, 1)


class _FixedNow:
    """固定时间：默认日志名带时间戳，测试不能靠「刚好没跨秒」。"""

    @staticmethod
    def now() -> datetime:
        return datetime(2030, 1, 2, 3, 4, 5)


class LogPathTest(unittest.TestCase):
    """默认路径 `work/tmp/run_<时间戳>.log`；`--log-file` 是内部参数，给什么用什么。"""

    def test_log_file_defaults_to_none(self):
        self.assertIsNone(run.parse_args(["--src", "x.si"]).log_file)
        self.assertEqual(
            run.parse_args([run.LOG_FILE_OPTION, "a.log"]).log_file, "a.log"
        )

    def test_default_path_is_work_tmp_run_timestamp(self):
        with mock.patch.object(run, "datetime", _FixedNow):
            path = run._log_path(run.parse_args([]))

        self.assertEqual(path, run.LOG_DIR / "run_2030-01-02_03-04-05.log")

    def test_explicit_path_wins_and_resolves_against_root(self):
        with mock.patch.object(run, "datetime", _FixedNow):
            path = run._log_path(
                run.parse_args([run.LOG_FILE_OPTION, "work/x.log"])
            )

        self.assertEqual(path, run.ROOT / "work/x.log")

    def test_second_run_in_the_same_second_gets_its_own_file(self):
        # 同名就追加会把两次运行糊成一个文件，谁也读不出来
        with mock.patch.object(run, "datetime", _FixedNow):
            first = run._log_path(run.parse_args([]))
            first.parent.mkdir(parents=True, exist_ok=True)
            first.write_text("", encoding="utf-8")
            second = run._log_path(run.parse_args([]))

        self.assertEqual(first.name, "run_2030-01-02_03-04-05.log")
        self.assertEqual(second.name, "run_2030-01-02_03-04-05-2.log")


class RelaunchLogTest(unittest.TestCase):
    """提权重启：原进程与提权子进程写同一份日志，子进程不再新建。"""

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.tmp = Path(temp.name)
        self.log_path = self.tmp / "run.log"

    def test_child_receives_the_file_parent_opened(self):
        config = self.tmp / "config.toml"
        config.write_text('[sampler]\nbackend = "etw"\n', encoding="utf-8")
        calls: list[str] = []

        def fake_shell(target, parameters, directory):
            calls.append(parameters)
            return 33

        with (
            mock.patch.object(run, "is_elevated", return_value=False),
            mock.patch.object(run, "_shell_execute_runas", side_effect=fake_shell),
            redirect_stdout(io.StringIO()),
            redirect_stderr(io.StringIO()),
        ):
            code = run.main([
                "--config", str(config), run.LOG_FILE_OPTION, str(self.log_path)
            ])

        self.assertEqual(code, 0)  # 已交给提权进程
        self.assertEqual(len(calls), 1)
        self.assertIn(run.LOG_FILE_OPTION, calls[0])
        self.assertIn(str(self.log_path), calls[0])
        # 原进程那半段在同一份文件里，子进程接着追加
        text = self.log_path.read_text(encoding="utf-8")
        self.assertIn("backend：etw", text)
        self.assertIn("已交给提权进程", text)


if __name__ == "__main__":
    unittest.main()
