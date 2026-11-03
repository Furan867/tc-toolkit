"""run.py 的编排单测：不启动游戏、不编译、不采样。"""
from __future__ import annotations

import importlib.util
import tempfile
import unittest
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


if __name__ == "__main__":
    unittest.main()
