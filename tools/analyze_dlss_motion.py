#!/usr/bin/env python3
"""Analyze sampled DLSS motion-coverage telemetry from Vulkan renderer logs.

The renderer logs two kinds of relevant lines (with arbitrary prefixes):

  DLSS SR <mode> frame=N input=WxH output=WxH completedEvaluations=N reset=N
      nonfinite=N [legacy matchedMotionPixels=/cameraFallbackPixels=/...]
  DLSS motion coverage countersReady=0|1 counterFrame=N valid=0|1
      available=0|1 eligible=N matched=N cameraFallback=N background=N
      resetExcluded=N disoccluded=N [matchedPercent=X fallbackPercent=X]

Each motion-coverage line describes ONE earlier completed frame (counterFrame)
and is logged roughly once per second. This tool only summarizes those logged
samples. It never infers full-run coverage, FPS, presentation success or
visual quality. Frame-time analysis is owned by the existing harness.

Usage: python tools/analyze_dlss_motion.py LOG_PATH
Exit status: 0 when at least one DLSS record parsed, 2 when no DLSS records
were found or the log could not be read.
"""

import json
import math
import re
import sys
from typing import Iterable

SCHEMA_VERSION = 1
SCOPE = "sampled_logged_frames"
KNOWN_MODES = frozenset(
    {"dlaa", "quality", "balanced", "performance", "ultra-performance"}
)
MAX_WARNINGS = 200
# Percent fields are logged with three decimals; allow rounding slack.
PERCENT_TOLERANCE = 0.0015

_SR_MARKER = re.compile(r"\bDLSS SR\b(?P<rest>.*)$")
_MOTION_MARKER = re.compile(r"\bDLSS motion coverage\b(?P<rest>.*)$")
_UINT = re.compile(r"[0-9]+")
_EXTENT = re.compile(r"([0-9]+)x([0-9]+)")
_PERCENT = re.compile(r"[0-9]+(?:\.[0-9]+)?")

_SR_INT_FIELDS = ("frame", "completedEvaluations", "reset", "nonfinite")
_SR_EXTENT_FIELDS = ("input", "output")
_LEGACY_FIELDS = ("matchedMotionPixels", "cameraFallbackPixels", "worldReplacement")
_MOTION_FLAG_FIELDS = ("countersReady", "valid", "available")
_MOTION_COUNT_FIELDS = (
    "counterFrame",
    "eligible",
    "matched",
    "cameraFallback",
    "background",
    "resetExcluded",
    "disoccluded",
)
_PERCENT_FIELDS = ("matchedPercent", "fallbackPercent")

LIMITATIONS = {
    "full_run_coverage_inferred": False,
    "fps_inferred": False,
    "presentation_verified": False,
    "visual_quality_verified": False,
    "note": (
        "Each motion sample describes one earlier completed frame logged about "
        "once per second; aggregates are sample-weighted over logged samples only."
    ),
}

LEGACY_NOTE = (
    "Legacy SR cameraFallbackPixels mixes sky/background and reset pixels; it is "
    "not object-motion fallback coverage and is never aggregated."
)


def _parse_uint(value):
    # Native counters are at most uint64. Bound before int conversion so a
    # damaged log cannot trigger Python's oversized-integer exception.
    if value is None or len(value) > 20 or not _UINT.fullmatch(value):
        return None
    number = int(value)
    return number if number <= (1 << 64) - 1 else None


def _parse_extent(value):
    if value is None:
        return None
    match = _EXTENT.fullmatch(value)
    if not match:
        return None
    width, height = (_parse_uint(match.group(i)) for i in (1, 2))
    if width is None or height is None or max(width, height) > (1 << 32) - 1:
        return None
    return width, height


def _parse_percent(value):
    if value is None or not _PERCENT.fullmatch(value):
        return None
    number = float(value)
    return number if math.isfinite(number) else None


def _parse_fields(tokens):
    """Return (fields, error) for key=value tokens; other tokens are ignored."""
    fields = {}
    for token in tokens:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        if key in fields:
            return None, "duplicate field " + key
        fields[key] = value
    return fields, None


class _Analyzer:
    def __init__(self):
        self.warnings = []
        self.warnings_dropped = 0
        self.sr_records = 0
        self.motion_records = 0
        self.malformed_records = 0
        self.segment = 0
        self.last_sr = None
        self.sr_context = None
        self.samples = {}
        self.seen_counts = {}
        self.conflicted = set()
        self.deduplicated_records = 0
        self.conflicting_records = 0
        self.legacy_records = 0
        self.unknown_modes = set()

    def warn(self, code, line_no, detail):
        if len(self.warnings) >= MAX_WARNINGS:
            self.warnings_dropped += 1
            return
        self.warnings.append({"code": code, "line": line_no, "detail": detail})

    def feed(self, line_no, line):
        if isinstance(line, bytes):
            line = line.decode("utf-8", errors="replace")
        elif not isinstance(line, str):
            return
        line = line.rstrip("\r\n")
        match = _MOTION_MARKER.search(line)
        if match:
            self._motion(line_no, match.group("rest").split())
            return
        match = _SR_MARKER.search(line)
        if match:
            self._sr(line_no, match.group("rest").split())

    def _malformed(self, code, line_no, detail):
        self.malformed_records += 1
        self.warn(code, line_no, detail)

    def _sr(self, line_no, tokens):
        # Only per-frame SR lines carry frame=; other "DLSS SR" lines are unrelated.
        if not any(token.startswith("frame=") for token in tokens):
            return
        self.sr_context = None
        if not tokens or "=" in tokens[0]:
            self._malformed("malformed_sr_record", line_no, "missing mode")
            return
        mode = tokens[0]
        fields, error = _parse_fields(tokens[1:])
        if fields is None:
            self._malformed("malformed_sr_record", line_no, error)
            return
        missing = [k for k in _SR_INT_FIELDS + _SR_EXTENT_FIELDS if k not in fields]
        if missing:
            self._malformed(
                "malformed_sr_record", line_no, "missing " + ",".join(missing)
            )
            return
        values = {}
        for key in _SR_INT_FIELDS:
            values[key] = _parse_uint(fields[key])
            if values[key] is None:
                self._malformed(
                    "malformed_sr_record", line_no, "invalid integer " + key
                )
                return
        for key in _SR_EXTENT_FIELDS:
            values[key] = _parse_extent(fields[key])
            if values[key] is None:
                self._malformed("malformed_sr_record", line_no, "invalid extent " + key)
                return

        if mode not in KNOWN_MODES and mode not in self.unknown_modes:
            self.unknown_modes.add(mode)
            self.warn("unknown_sr_mode", line_no, "mode " + mode)

        record = {
            "mode": mode,
            "frame": values["frame"],
            "input_width": values["input"][0],
            "input_height": values["input"][1],
            "output_width": values["output"][0],
            "output_height": values["output"][1],
            "completed_evaluations": values["completedEvaluations"],
            "reset": values["reset"],
            "nonfinite": values["nonfinite"],
        }

        previous = self.last_sr
        if previous is None:
            self.segment = 1
        else:
            reasons = []
            if record["frame"] < previous["frame"]:
                reasons.append("frame_regression")
            if record["mode"] != previous["mode"]:
                reasons.append("mode_change")
            if (
                record["input_width"],
                record["input_height"],
                record["output_width"],
                record["output_height"],
            ) != (
                previous["input_width"],
                previous["input_height"],
                previous["output_width"],
                previous["output_height"],
            ):
                reasons.append("extent_change")
            if reasons:
                self.segment += 1
                self.warn("sr_segment_started", line_no, ",".join(reasons))
        record["segment"] = self.segment
        self.last_sr = record
        self.sr_context = record
        self.sr_records += 1
        # New renderer labels these delayed counts explicitly and no longer
        # includes sky/reset pixels in its cameraFallbackPixels field.
        if any(key in fields for key in _LEGACY_FIELDS) and "motionCounterFrame" not in fields:
            self.legacy_records += 1

    def _motion(self, line_no, tokens):
        if not any(
            token.startswith("countersReady=") or token.startswith("counterFrame=")
            for token in tokens
        ):
            return
        fields, error = _parse_fields(tokens)
        if fields is None:
            self._malformed("malformed_motion_record", line_no, error)
            return
        required = _MOTION_FLAG_FIELDS + _MOTION_COUNT_FIELDS
        missing = [k for k in required if k not in fields]
        if missing:
            self._malformed(
                "malformed_motion_record", line_no, "missing " + ",".join(missing)
            )
            return
        c = {}
        for key in required:
            c[key] = _parse_uint(fields[key])
            if c[key] is None:
                self._malformed(
                    "malformed_motion_record", line_no, "invalid integer " + key
                )
                return
        for key in _MOTION_FLAG_FIELDS:
            if c[key] not in (0, 1):
                self._malformed(
                    "malformed_motion_record", line_no, "invalid flag " + key
                )
                return
        self.motion_records += 1

        percents = {}
        for key in _PERCENT_FIELDS:
            if key in fields:
                percents[key] = _parse_percent(fields[key])
                if percents[key] is None:
                    self.warn("malformed_percent", line_no, "invalid " + key)

        frame = c["counterFrame"]
        if c["countersReady"] != 1:
            self.warn("counters_not_ready", line_no, "counterFrame=%d" % frame)
            if c["valid"] or c["available"]:
                self.warn(
                    "inconsistent_flags",
                    line_no,
                    "valid/available reported without countersReady",
                )
            return

        eligible = c["eligible"]
        problems = []
        sums_match = c["matched"] + c["cameraFallback"] == eligible
        if not sums_match:
            problems.append("matched+cameraFallback!=eligible")
        if c["disoccluded"] > eligible:
            problems.append("disoccluded>eligible")

        sr = self.sr_context
        dims_known = False
        if sr is None:
            self.warn(
                "unknown_dimensions",
                line_no,
                "no preceding SR record for counterFrame=%d" % frame,
            )
        elif sr["input_width"] == 0 or sr["input_height"] == 0:
            self.warn(
                "unknown_dimensions", line_no, "zero input extent in preceding SR"
            )
        else:
            dims_known = True
            total = sr["input_width"] * sr["input_height"]
            if eligible + c["background"] + c["resetExcluded"] != total:
                problems.append("eligible+background+resetExcluded!=input pixels")
        for problem in problems:
            self.warn(
                "counter_invariant_violation",
                line_no,
                "counterFrame=%d %s" % (frame, problem),
            )

        computed_valid = dims_known and not problems
        computed_available = computed_valid and eligible > 0
        flags_ok = c["valid"] == int(computed_valid) and c["available"] == int(
            computed_available
        )
        if not flags_ok and (dims_known or c["valid"] == 0 or problems):
            self.warn(
                "inconsistent_flags",
                line_no,
                "counterFrame=%d reported valid=%d available=%d computed valid=%d available=%d"
                % (
                    frame,
                    c["valid"],
                    c["available"],
                    int(computed_valid),
                    int(computed_available),
                ),
            )

        if sums_match:
            expected = {
                "matchedPercent": 100.0 * c["matched"] / eligible if eligible else 0.0,
                "fallbackPercent": (
                    100.0 * c["cameraFallback"] / eligible if eligible else 0.0
                ),
            }
            for key in _PERCENT_FIELDS:
                value = percents.get(key)
                if value is not None and abs(value - expected[key]) > PERCENT_TOLERANCE:
                    self.warn(
                        "percent_mismatch",
                        line_no,
                        "%s reported %s recomputed %.3f" % (key, fields[key], expected[key]),
                    )

        if not dims_known:
            return
        if frame >= sr["frame"]:
            self.warn(
                "counter_frame_not_earlier",
                line_no,
                "counterFrame=%d associated SR frame=%d" % (frame, sr["frame"]),
            )
            return
        key = (sr["segment"], frame, sr["input_width"], sr["input_height"])
        counts = tuple(c[k] for k in _MOTION_COUNT_FIELDS)
        prior_counts = self.seen_counts.get(key)
        if prior_counts is not None and prior_counts != counts and key not in self.conflicted:
            self.samples.pop(key, None)
            self.conflicted.add(key)
            self.conflicting_records += 2
            self.warn("duplicate_counter_frame", line_no,
                      "counterFrame=%d conflicting duplicate excluded" % frame)
            return
        self.seen_counts[key] = counts
        if key in self.conflicted:
            self.conflicting_records += 1
            self.warn(
                "duplicate_counter_frame",
                line_no,
                "counterFrame=%d conflicting duplicate excluded" % frame,
            )
            return
        if not computed_valid or not flags_ok:
            return
        existing = self.samples.get(key)
        if existing is not None:
            if existing["_counts"] == counts:
                self.deduplicated_records += 1
                self.warn(
                    "duplicate_counter_frame",
                    line_no,
                    "counterFrame=%d identical duplicate deduplicated" % frame,
                )
            else:
                del self.samples[key]
                self.conflicted.add(key)
                self.conflicting_records += 2
                self.warn(
                    "duplicate_counter_frame",
                    line_no,
                    "counterFrame=%d conflicting duplicate excluded" % frame,
                )
            return
        self.samples[key] = {
            "_counts": counts,
            "segment": sr["segment"],
            "counter_frame": frame,
            "sr_frame": sr["frame"],
            "line": line_no,
            "input_width": sr["input_width"],
            "input_height": sr["input_height"],
            "eligible": eligible,
            "matched": c["matched"],
            "camera_fallback": c["cameraFallback"],
            "background": c["background"],
            "reset_excluded": c["resetExcluded"],
            "disoccluded": c["disoccluded"],
            "coverage_available": computed_available,
        }

    def result(self):
        if self.legacy_records:
            self.warn(
                "legacy_coverage_unavailable",
                None,
                "%d SR record(s) carry legacy coverage fields; %s"
                % (self.legacy_records, LEGACY_NOTE),
            )
        if self.sr_records and not self.motion_records:
            self.warn(
                "no_motion_coverage_records",
                None,
                "no DLSS motion coverage records were logged",
            )

        samples = [
            {k: v for k, v in sample.items() if not k.startswith("_")}
            for sample in self.samples.values()
        ]
        eligible = sum(s["eligible"] for s in samples)
        matched = sum(s["matched"] for s in samples)
        fallback = sum(s["camera_fallback"] for s in samples)
        disoccluded = sum(s["disoccluded"] for s in samples)
        aggregate = {
            "sample_count": len(samples),
            "available_sample_count": sum(1 for s in samples if s["coverage_available"]),
            "eligible_pixels": eligible,
            "matched_pixels": matched,
            "camera_fallback_pixels": fallback,
            "disoccluded_pixels": disoccluded,
            "fallback_percent": round(100.0 * fallback / eligible, 3) if eligible else None,
            "matched_percent": round(100.0 * matched / eligible, 3) if eligible else None,
        }
        last_sr = None
        if self.last_sr is not None:
            last_sr = dict(self.last_sr)
        return {
            "schema_version": SCHEMA_VERSION,
            "scope": SCOPE,
            "limitations": dict(LIMITATIONS),
            "sr_records": self.sr_records,
            "segments": self.segment,
            "last_sr": last_sr,
            "motion_records": self.motion_records,
            "malformed_records": self.malformed_records,
            "deduplicated_records": self.deduplicated_records,
            "conflicting_records": self.conflicting_records,
            "samples": samples,
            "aggregate": aggregate,
            "legacy": {
                "sr_records_with_legacy_fields": self.legacy_records,
                "comparable_to_motion_coverage": False,
                "note": LEGACY_NOTE,
            },
            "warnings": self.warnings,
            "warnings_dropped": self.warnings_dropped,
        }


def analyze_lines(lines: Iterable[str]) -> dict:
    """Parse renderer log lines and return the deterministic summary dict."""
    analyzer = _Analyzer()
    for line_no, line in enumerate(lines, start=1):
        analyzer.feed(line_no, line)
    return analyzer.result()


def has_usable_records(result: dict) -> bool:
    return bool(result.get("sr_records") or result.get("motion_records"))


def dumps(result: dict) -> str:
    return json.dumps(result, sort_keys=True, indent=2, allow_nan=False)


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        sys.stderr.write("usage: analyze_dlss_motion.py LOG_PATH\n")
        return 2
    try:
        with open(argv[0], "r", encoding="utf-8", errors="replace", newline="") as handle:
            result = analyze_lines(handle)
    except OSError as exc:
        error = {
            "schema_version": SCHEMA_VERSION,
            "scope": SCOPE,
            "error": {"type": "io_error", "detail": type(exc).__name__},
        }
        print(dumps(error))
        return 2
    usable = has_usable_records(result)
    if not usable:
        result["error"] = {"type": "no_dlss_records", "detail": "no DLSS records parsed"}
    print(dumps(result))
    return 0 if usable else 2


if __name__ == "__main__":
    sys.exit(main())
