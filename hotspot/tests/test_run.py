"""run.py 的编排单测：不启动游戏、不编译、不采样。"""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("hotspot_run", HERE.parent / "run.py")
assert SPEC and SPEC.loader
run = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(run)


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


if __name__ == "__main__":
    unittest.main()
