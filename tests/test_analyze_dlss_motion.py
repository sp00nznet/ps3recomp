import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOL_PATH = pathlib.Path(__file__).resolve().parent.parent / "tools" / "analyze_dlss_motion.py"
_spec = importlib.util.spec_from_file_location("analyze_dlss_motion", TOOL_PATH)
adm = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(adm)


def sr(frame, mode="quality", inp="1280x720", out="2560x1440", prefix="[vk] "):
    return (
        f"{prefix}DLSS SR {mode} frame={frame} input={inp} output={out} "
        f"completedEvaluations={max(frame - 1, 0)} reset=0 nonfinite=0 HUD=guest-composite"
    )


def motion(
    counter_frame,
    width,
    height,
    eligible,
    matched,
    fallback,
    reset=0,
    disoccluded=0,
    background=None,
    ready=1,
    valid=None,
    available=None,
    percents=True,
    prefix="[vk] ",
):
    if background is None:
        background = width * height - eligible - reset
    consistent = (
        matched + fallback == eligible
        and disoccluded <= eligible
        and eligible + background + reset == width * height
        and width > 0
        and height > 0
    )
    if valid is None:
        valid = int(ready == 1 and consistent)
    if available is None:
        available = int(valid == 1 and eligible > 0)
    text = (
        f"{prefix}DLSS motion coverage countersReady={ready} counterFrame={counter_frame} "
        f"valid={valid} available={available} eligible={eligible} matched={matched} "
        f"cameraFallback={fallback} background={background} resetExcluded={reset} "
        f"disoccluded={disoccluded}"
    )
    if percents:
        mp = 100.0 * matched / eligible if eligible else 0.0
        fp = 100.0 * fallback / eligible if eligible else 0.0
        text += f" matchedPercent={mp:.3f} fallbackPercent={fp:.3f}"
    return text


def codes(result):
    return [w["code"] for w in result["warnings"]]


def normal_720():
    # 1280*720 = 921600 = 600000 eligible + 320000 background + 1600 reset
    return motion(100, 1280, 720, 600000, 450000, 150000, reset=1600, disoccluded=5000)


def normal_ultrawide():
    # 3440*1440 = 4953600 = 4000000 eligible + 953600 background
    return motion(200, 3440, 1440, 4000000, 3900000, 100000)


class NormalPartitionTests(unittest.TestCase):
    def test_1280x720_partition(self):
        result = adm.analyze_lines([sr(101), normal_720()])
        self.assertEqual(result["schema_version"], 1)
        self.assertEqual(result["scope"], "sampled_logged_frames")
        self.assertEqual(result["warnings"], [])
        self.assertEqual(result["sr_records"], 1)
        self.assertEqual(result["motion_records"], 1)
        self.assertEqual(len(result["samples"]), 1)
        sample = result["samples"][0]
        self.assertEqual(sample["counter_frame"], 100)
        self.assertEqual(sample["sr_frame"], 101)
        self.assertEqual((sample["input_width"], sample["input_height"]), (1280, 720))
        self.assertEqual(sample["eligible"], 600000)
        self.assertEqual(sample["background"], 320000)
        self.assertEqual(sample["reset_excluded"], 1600)
        self.assertEqual(sample["disoccluded"], 5000)
        self.assertTrue(sample["coverage_available"])
        self.assertEqual(result["aggregate"]["fallback_percent"], 25.0)
        self.assertEqual(result["aggregate"]["matched_percent"], 75.0)
        last = result["last_sr"]
        self.assertEqual(last["mode"], "quality")
        self.assertEqual(last["frame"], 101)
        self.assertEqual((last["output_width"], last["output_height"]), (2560, 1440))
        self.assertEqual(last["completed_evaluations"], 100)
        self.assertEqual(last["reset"], 0)
        self.assertEqual(last["nonfinite"], 0)
        self.assertFalse(result["limitations"]["presentation_verified"])
        self.assertFalse(result["limitations"]["visual_quality_verified"])
        self.assertFalse(result["limitations"]["full_run_coverage_inferred"])

    def test_3440x1440_and_sample_weighted_aggregate(self):
        lines = [
            sr(101),
            normal_720(),
            sr(201, mode="dlaa", inp="3440x1440", out="3440x1440"),
            normal_ultrawide(),
        ]
        result = adm.analyze_lines(lines)
        self.assertEqual(len(result["samples"]), 2)
        self.assertEqual(result["segments"], 2)
        self.assertEqual(result["aggregate"]["eligible_pixels"], 4600000)
        self.assertEqual(result["aggregate"]["camera_fallback_pixels"], 250000)
        self.assertAlmostEqual(result["aggregate"]["fallback_percent"], 5.435, places=3)
        self.assertEqual(codes(result), ["sr_segment_started"])

    def test_background_and_reset_only(self):
        line = motion(100, 1280, 720, 0, 0, 0, reset=21600)
        result = adm.analyze_lines([sr(101), line])
        self.assertEqual(result["warnings"], [])
        self.assertEqual(len(result["samples"]), 1)
        self.assertFalse(result["samples"][0]["coverage_available"])
        self.assertEqual(result["aggregate"]["available_sample_count"], 0)
        self.assertIsNone(result["aggregate"]["fallback_percent"])
        self.assertIsNone(result["aggregate"]["matched_percent"])

    def test_percent_fields_do_not_drive_aggregation(self):
        line = normal_720().replace("fallbackPercent=25.000", "fallbackPercent=99.000")
        result = adm.analyze_lines([sr(101), line])
        self.assertIn("percent_mismatch", codes(result))
        self.assertEqual(result["aggregate"]["fallback_percent"], 25.0)

    def test_output_is_deterministic_json(self):
        lines = [sr(101), normal_720(), "garbage", normal_720()]
        first = adm.dumps(adm.analyze_lines(lines))
        second = adm.dumps(adm.analyze_lines(lines))
        self.assertEqual(first, second)
        self.assertNotIn("NaN", first)
        self.assertNotIn("Infinity", first)


class LegacyTests(unittest.TestCase):
    OLD = (
        "I/render: DLSS SR quality frame=101 input=1707x960 output=2560x1440 "
        "completedEvaluations=44 matchedMotionPixels=500 cameraFallbackPixels=200 "
        "reset=0 nonfinite=0 worldReplacement=1 HUD=guest-composite"
    )
    NEW = (
        "I/render: DLSS motion coverage countersReady=1 counterFrame=100 valid=1 "
        "available=1 eligible=700 matched=500 cameraFallback=200 background=300 "
        "resetExcluded=0 disoccluded=20 matchedPercent=71.429 fallbackPercent=28.571"
    )

    def test_legacy_only_has_no_coverage(self):
        result = adm.analyze_lines([self.OLD])
        self.assertEqual(result["sr_records"], 1)
        self.assertEqual(result["motion_records"], 0)
        self.assertEqual(result["samples"], [])
        self.assertIsNone(result["aggregate"]["fallback_percent"])
        self.assertIn("legacy_coverage_unavailable", codes(result))
        self.assertEqual(result["legacy"]["sr_records_with_legacy_fields"], 1)
        self.assertFalse(result["legacy"]["comparable_to_motion_coverage"])
        self.assertEqual(result["last_sr"]["input_width"], 1707)

    def test_contract_example_partition_is_recomputed_not_trusted(self):
        # 700+300+0 != 1707*960, so the claimed valid=1 must not be trusted.
        result = adm.analyze_lines([self.OLD, self.NEW])
        self.assertEqual(result["motion_records"], 1)
        self.assertEqual(result["samples"], [])
        self.assertIn("counter_invariant_violation", codes(result))
        self.assertIn("inconsistent_flags", codes(result))


class MalformedTests(unittest.TestCase):
    def test_oversized_numbers_are_rejected_without_exception(self):
        for bad in ("9" * 5000, str(1 << 64)):
            result = adm.analyze_lines([sr(101), normal_720().replace("eligible=600000", "eligible=" + bad)])
            self.assertEqual(result["samples"], [])
            self.assertIn("malformed_motion_record", codes(result))
        result = adm.analyze_lines([sr(101, inp=("9" * 5000) + "x720")])
        self.assertEqual(result["sr_records"], 0)

    def test_malformed_sr_invalidates_counter_association(self):
        result = adm.analyze_lines([sr(101), sr(102).replace("input=1280x720", "input=broken"), normal_720()])
        self.assertEqual(result["last_sr"]["frame"], 101)
        self.assertEqual(result["samples"], [])
        self.assertIn("unknown_dimensions", codes(result))

    def test_infinite_decimal_diagnostic_is_rejected(self):
        line = normal_720().replace("fallbackPercent=25.000", "fallbackPercent=" + "9" * 1000)
        result = adm.analyze_lines([sr(101), line])
        self.assertIn("malformed_percent", codes(result))
        self.assertEqual(result["aggregate"]["fallback_percent"], 25.0)

    def test_malformed_motion_records(self):
        base = normal_720()
        lines = [
            sr(101),
            base.replace("eligible=600000", "eligible=-5"),
            base.replace("eligible=600000", "eligible=1.5"),
            base.replace(" eligible=600000", ""),
            base.replace("countersReady=1", "countersReady=2"),
            base + " matched=1",
        ]
        result = adm.analyze_lines(lines)
        self.assertEqual(result["malformed_records"], 5)
        self.assertEqual(result["motion_records"], 0)
        self.assertEqual(codes(result).count("malformed_motion_record"), 5)
        self.assertEqual(result["samples"], [])

    def test_malformed_sr_does_not_replace_last_sr(self):
        lines = [
            sr(101),
            sr(102).replace("input=1280x720", "input=1280by720"),
            sr(103).replace("nonfinite=0", "nonfinite=nan"),
            "DLSS SR frame=5 input=1x1 output=1x1",
        ]
        result = adm.analyze_lines(lines)
        self.assertEqual(result["sr_records"], 1)
        self.assertEqual(result["malformed_records"], 3)
        self.assertEqual(result["last_sr"]["frame"], 101)

    def test_nan_percent_is_warning_only(self):
        line = normal_720().replace("fallbackPercent=25.000", "fallbackPercent=nan")
        result = adm.analyze_lines([sr(101), line])
        self.assertIn("malformed_percent", codes(result))
        self.assertEqual(len(result["samples"]), 1)
        adm.dumps(result)

    def test_non_string_and_bytes_lines_do_not_crash(self):
        result = adm.analyze_lines([None, 42, sr(101).encode("utf-8"), normal_720()])
        self.assertEqual(result["sr_records"], 1)
        self.assertEqual(len(result["samples"]), 1)

    def test_empty_input(self):
        result = adm.analyze_lines([])
        self.assertEqual(result["sr_records"], 0)
        self.assertEqual(result["motion_records"], 0)
        self.assertIsNone(result["last_sr"])
        self.assertFalse(adm.has_usable_records(result))

    def test_unrelated_lines_ignored(self):
        lines = [
            "Vulkan device created",
            "DLSS SR initialized for quality",
            "DLSS SR",
            "DLSS motion coverage disabled",
            "frame=10 input=1x1",
            "",
        ]
        result = adm.analyze_lines(lines)
        self.assertEqual(result["sr_records"], 0)
        self.assertEqual(result["motion_records"], 0)
        self.assertEqual(result["malformed_records"], 0)
        self.assertEqual(result["warnings"], [])

    def test_motion_without_sr_does_not_fabricate_dimensions(self):
        result = adm.analyze_lines([normal_720()])
        self.assertEqual(result["motion_records"], 1)
        self.assertEqual(result["samples"], [])
        self.assertIn("unknown_dimensions", codes(result))
        self.assertTrue(adm.has_usable_records(result))


class InconsistencyTests(unittest.TestCase):
    def test_claimed_valid_with_bad_sum(self):
        line = motion(100, 1280, 720, 600000, 450000, 140000, background=321600, valid=1, available=1)
        result = adm.analyze_lines([sr(101), line])
        self.assertIn("counter_invariant_violation", codes(result))
        self.assertIn("inconsistent_flags", codes(result))
        self.assertEqual(result["samples"], [])

    def test_partition_mismatch(self):
        line = motion(100, 1280, 720, 600000, 450000, 150000, background=1, valid=1, available=1)
        result = adm.analyze_lines([sr(101), line])
        self.assertIn("counter_invariant_violation", codes(result))
        self.assertEqual(result["samples"], [])

    def test_disocclusion_exceeds_eligible(self):
        line = motion(100, 1280, 720, 600000, 450000, 150000, disoccluded=600001, valid=1, available=1)
        result = adm.analyze_lines([sr(101), line])
        self.assertIn("counter_invariant_violation", codes(result))
        self.assertEqual(result["samples"], [])

    def test_reported_invalid_but_consistent(self):
        line = motion(100, 1280, 720, 600000, 450000, 150000, valid=0, available=0)
        result = adm.analyze_lines([sr(101), line])
        self.assertIn("inconsistent_flags", codes(result))
        self.assertEqual(result["samples"], [])

    def test_available_without_eligible(self):
        line = motion(100, 1280, 720, 0, 0, 0, valid=1, available=1)
        result = adm.analyze_lines([sr(101), line])
        self.assertIn("inconsistent_flags", codes(result))
        self.assertEqual(result["samples"], [])

    def test_not_ready(self):
        line = motion(100, 1280, 720, 0, 0, 0, ready=0, valid=1, available=0)
        result = adm.analyze_lines([sr(101), line])
        self.assertIn("counters_not_ready", codes(result))
        self.assertIn("inconsistent_flags", codes(result))
        self.assertEqual(result["samples"], [])

    def test_counter_frame_not_earlier(self):
        result = adm.analyze_lines([sr(100), normal_720()])
        self.assertIn("counter_frame_not_earlier", codes(result))
        self.assertEqual(result["samples"], [])

    def test_zero_input_dimensions(self):
        result = adm.analyze_lines([sr(101, inp="0x0"), motion(100, 1, 1, 0, 0, 0, background=0, valid=1, available=0)])
        self.assertIn("unknown_dimensions", codes(result))
        self.assertEqual(result["samples"], [])


class DuplicateAndSegmentTests(unittest.TestCase):
    def test_invalid_conflicting_duplicate_excludes_valid_sample(self):
        bad = normal_720().replace("eligible=600000", "eligible=600001")
        for lines in ([sr(101), normal_720(), bad], [sr(101), bad, normal_720()]):
            result = adm.analyze_lines(lines)
            self.assertEqual(result["samples"], [])
            self.assertIn("duplicate_counter_frame", codes(result))

    def test_explicitly_labeled_new_sr_is_not_legacy(self):
        current = LegacyTests.OLD.replace("completedEvaluations=44", "completedEvaluations=44 motionCounterFrame=100")
        result = adm.analyze_lines([current])
        self.assertNotIn("legacy_coverage_unavailable", codes(result))

    def test_identical_duplicate_deduplicated(self):
        result = adm.analyze_lines([sr(101), normal_720(), sr(102), normal_720()])
        self.assertEqual(len(result["samples"]), 1)
        self.assertEqual(result["deduplicated_records"], 1)
        self.assertIn("duplicate_counter_frame", codes(result))
        self.assertEqual(result["aggregate"]["eligible_pixels"], 600000)

    def test_conflicting_duplicate_excluded(self):
        other = motion(100, 1280, 720, 600000, 300000, 300000, reset=1600)
        result = adm.analyze_lines([sr(101), normal_720(), other, normal_720()])
        self.assertEqual(result["samples"], [])
        self.assertEqual(codes(result).count("duplicate_counter_frame"), 2)
        self.assertIsNone(result["aggregate"]["fallback_percent"])

    def test_restart_with_reused_frame_ids_not_deduplicated(self):
        line = motion(450, 1280, 720, 600000, 450000, 150000, reset=1600)
        result = adm.analyze_lines([sr(500), line, sr(460), line])
        self.assertEqual(result["segments"], 2)
        self.assertEqual(len(result["samples"]), 2)
        self.assertEqual([s["segment"] for s in result["samples"]], [1, 2])
        self.assertNotIn("duplicate_counter_frame", codes(result))
        self.assertIn("sr_segment_started", codes(result))

    def test_mode_change_starts_segment(self):
        line = motion(100, 1280, 720, 600000, 450000, 150000, reset=1600)
        result = adm.analyze_lines([sr(101), line, sr(102, mode="balanced"), line])
        self.assertEqual(result["segments"], 2)
        self.assertEqual(len(result["samples"]), 2)
        self.assertEqual(result["last_sr"]["mode"], "balanced")

    def test_all_known_modes_accepted(self):
        for mode in ("dlaa", "quality", "balanced", "performance", "ultra-performance"):
            result = adm.analyze_lines([sr(5, mode=mode)])
            self.assertEqual(result["last_sr"]["mode"], mode)
            self.assertNotIn("unknown_sr_mode", codes(result))


class CliTests(unittest.TestCase):
    def run_cli(self, path):
        proc = subprocess.run(
            [sys.executable, str(TOOL_PATH), str(path)],
            capture_output=True,
            text=True,
            timeout=60,
        )
        return proc.returncode, json.loads(proc.stdout)

    def write(self, directory, name, text):
        path = pathlib.Path(directory) / name
        path.write_text(text, encoding="utf-8")
        return path

    def test_cli_success(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self.write(tmp, "log.txt", "\r\n".join(["noise", sr(101), normal_720()]) + "\r\n")
            code, data = self.run_cli(path)
        self.assertEqual(code, 0)
        self.assertEqual(data["schema_version"], 1)
        self.assertEqual(len(data["samples"]), 1)
        self.assertEqual(data["aggregate"]["fallback_percent"], 25.0)

    def test_cli_missing_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            code, data = self.run_cli(os.path.join(tmp, "missing.log"))
        self.assertEqual(code, 2)
        self.assertEqual(data["error"]["type"], "io_error")

    def test_cli_directory_is_io_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            code, data = self.run_cli(tmp)
        self.assertEqual(code, 2)
        self.assertEqual(data["error"]["type"], "io_error")

    def test_cli_empty_and_unrelated(self):
        with tempfile.TemporaryDirectory() as tmp:
            empty = self.write(tmp, "empty.log", "")
            unrelated = self.write(tmp, "other.log", "hello\nDLSS SR initialized\n")
            code_empty, data_empty = self.run_cli(empty)
            code_other, data_other = self.run_cli(unrelated)
        self.assertEqual(code_empty, 2)
        self.assertEqual(data_empty["error"]["type"], "no_dlss_records")
        self.assertEqual(code_other, 2)
        self.assertEqual(data_other["sr_records"], 0)

    def test_cli_requires_one_argument(self):
        proc = subprocess.run(
            [sys.executable, str(TOOL_PATH)], capture_output=True, text=True, timeout=60
        )
        self.assertEqual(proc.returncode, 2)


if __name__ == "__main__":
    unittest.main()
