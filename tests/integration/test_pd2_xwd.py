#!/usr/bin/env python3
"""Hermetic tests for bounded, privacy-preserving XWD evidence."""

from __future__ import annotations

import dataclasses
import os
import subprocess
import struct
import sys
import tempfile
import unittest
from collections.abc import Mapping, Sequence
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
from pd2_xwd import (  # noqa: E402
    FINGERPRINT_BYTES,
    KILL_TIMEOUT_SECONDS,
    MAX_CAPTURE_BYTES,
    MAX_DIMENSION,
    CAPTURE_TIMEOUT_SECONDS,
    SurfaceTransitionSummary,
    XwdCaptureProcess,
    XwdEvidenceError,
    capture_window_fingerprint,
    fingerprint_xwd,
    fingerprint_xwd_samples,
    require_complete_surface_transition,
    transparent_reference_fingerprint,
)


def xwd(
    pixels: bytes = bytes.fromhex("10203000 40506000"),
    *,
    name: bytes = b"Controlled surface",
    width: int = 2,
    height: int = 1,
    byte_order: int = 0,
    bits_per_pixel: int = 32,
    bytes_per_line: int = 8,
    colors: bytes = b"",
    color_count: int = 0,
    colormap_entries: int = 256,
) -> bytes:
    header_size = 100 + len(name) + 1
    fields = (
        header_size,
        7,
        2,
        24,
        width,
        height,
        0,
        byte_order,
        32,
        0,
        32,
        bits_per_pixel,
        bytes_per_line,
        4,
        0x00FF0000,
        0x0000FF00,
        0x000000FF,
        8,
        colormap_entries,
        color_count,
        width,
        height,
        0,
        0,
        0,
    )
    return struct.pack(">25I", *fields) + name + b"\x00" + colors + pixels


class FakeCaptureProcess(XwdCaptureProcess):
    def __init__(self, return_code: int = 0, *, times_out: bool = False) -> None:
        self.return_code = return_code
        self.times_out = times_out
        self.killed = False
        self.wait_calls: list[float | None] = []

    def wait(self, timeout: float | None = None) -> int:
        self.wait_calls.append(timeout)
        if self.times_out and not self.killed:
            raise subprocess.TimeoutExpired(("private",), timeout)
        return self.return_code

    def kill(self) -> None:
        self.killed = True


class FakeCaptureRunner:
    def __init__(
        self,
        action: str = "success",
        payload: bytes | None = None,
    ) -> None:
        self.action = action
        self.payload = payload if payload is not None else xwd(name=b"Private title")
        self.calls: list[tuple[tuple[str, ...], dict[str, str]]] = []
        self.output: Path | None = None
        self.process = FakeCaptureProcess(
            17 if action == "failure" else 0,
            times_out=action == "timeout",
        )

    def start(
        self, argv: Sequence[str], environment: Mapping[str, str]
    ) -> XwdCaptureProcess:
        self.calls.append((tuple(argv), dict(environment)))
        self.output = Path(argv[-1])
        if self.action == "start_failure":
            raise OSError(f"private path and title: {self.output}")
        if self.action == "oversize":
            os.truncate(self.output, MAX_CAPTURE_BYTES + 1)
        elif self.action == "permission":
            self.output.write_bytes(self.payload)
            self.output.chmod(0o644)
        elif self.action == "symlink":
            target = self.output.parent / "private-target.xwd"
            target.write_bytes(self.payload)
            target.chmod(0o600)
            self.output.unlink()
            self.output.symlink_to(target)
        elif self.action not in {"timeout", "failure"}:
            self.output.write_bytes(self.payload)
        return self.process


class CaptureFixture:
    def __init__(self, root: Path) -> None:
        self.executable = root / "configured-xwd"
        self.executable.write_bytes(b"exact executable")
        self.executable.chmod(0o700)
        self.private = root / "capture"
        self.private.mkdir(mode=0o700)

    def capture(self, runner: FakeCaptureRunner, window_id: int = 0x1234) -> bytes:
        return capture_window_fingerprint(
            self.executable,
            window_id,
            self.private,
            {"DISPLAY": ":140"},
            runner,
        )

    def capture_samples(
        self, runner: FakeCaptureRunner, points: Sequence[tuple[int, int]]
    ) -> bytes:
        return capture_window_fingerprint(
            self.executable,
            0x1234,
            self.private,
            {"DISPLAY": ":140"},
            runner,
            points,
        )


class XwdCaptureBoundaryTests(unittest.TestCase):
    def test_transparent_reference_matches_the_exact_reviewed_shape(self) -> None:
        points = ((0, 0), (1, 0))
        transparent = xwd(pixels=b"\x00" * 8, name=b"ignored")

        self.assertEqual(
            transparent_reference_fingerprint(2, 1, points),
            fingerprint_xwd_samples(transparent, points),
        )
        self.assertNotEqual(
            transparent_reference_fingerprint(2, 1, points),
            fingerprint_xwd_samples(xwd(), points),
        )

        for dimensions in ((0, 1), (1, 0), (MAX_DIMENSION + 1, 1)):
            with self.subTest(dimensions=dimensions), self.assertRaisesRegex(
                XwdEvidenceError, "^xwd_dimensions_invalid$"
            ):
                transparent_reference_fingerprint(*dimensions, points)
        for depth in (False, "bad", 0, 33):
            with self.subTest(depth=depth), self.assertRaisesRegex(
                XwdEvidenceError, "^xwd_pixel_layout_invalid$"
            ):
                transparent_reference_fingerprint(2, 1, points, depth=depth)
        with self.assertRaisesRegex(XwdEvidenceError, "^xwd_capture_too_large$"):
            transparent_reference_fingerprint(MAX_DIMENSION, MAX_DIMENSION, points)

    def test_runs_exact_executable_and_returns_only_a_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary).resolve())
            runner = FakeCaptureRunner()

            result = fixture.capture(runner)

            self.assertEqual(result, fingerprint_xwd(runner.payload))
            self.assertEqual(len(result), FINGERPRINT_BYTES)
            self.assertEqual(
                runner.calls[0][0][:-1],
                (
                    str(fixture.executable),
                    "-silent",
                    "-id",
                    "0x1234",
                    "-out",
                ),
            )
            self.assertEqual(runner.calls[0][1], {"DISPLAY": ":140", "LC_ALL": "C"})
            self.assertIsNotNone(runner.output)
            self.assertFalse(runner.output.exists())

    def test_sample_capture_returns_only_reviewed_pixel_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary).resolve())
            runner = FakeCaptureRunner()
            points = ((0, 0),)

            result = fixture.capture_samples(runner, points)

            self.assertEqual(result, fingerprint_xwd_samples(runner.payload, points))
            self.assertEqual(len(result), FINGERPRINT_BYTES)
            self.assertIsNotNone(runner.output)
            self.assertFalse(runner.output.exists())

    def test_timeout_kills_reaps_and_unlinks_the_private_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary).resolve())
            runner = FakeCaptureRunner("timeout")

            with self.assertRaisesRegex(XwdEvidenceError, "^xwd_capture_timeout$"):
                fixture.capture(runner)

            self.assertTrue(runner.process.killed)
            self.assertEqual(
                runner.process.wait_calls,
                [CAPTURE_TIMEOUT_SECONDS, KILL_TIMEOUT_SECONDS],
            )
            self.assertIsNotNone(runner.output)
            self.assertFalse(runner.output.exists())

    def test_rejects_oversize_failure_and_malformed_capture_then_unlinks(self) -> None:
        cases = (
            ("oversize", None, "xwd_capture_too_large"),
            ("failure", None, "xwd_capture_failed"),
            ("success", b"private malformed pixels and title", "xwd_header_truncated"),
        )
        for action, payload, category in cases:
            with self.subTest(category=category), tempfile.TemporaryDirectory() as temporary:
                fixture = CaptureFixture(Path(temporary).resolve())
                runner = FakeCaptureRunner(action, payload)
                with self.assertRaisesRegex(XwdEvidenceError, f"^{category}$"):
                    fixture.capture(runner)
                self.assertIsNotNone(runner.output)
                self.assertFalse(runner.output.exists())

    def test_rejects_output_permission_and_symlink_substitution_then_unlinks(self) -> None:
        for action in ("permission", "symlink"):
            with self.subTest(action=action), tempfile.TemporaryDirectory() as temporary:
                fixture = CaptureFixture(Path(temporary).resolve())
                runner = FakeCaptureRunner(action)
                with self.assertRaisesRegex(XwdEvidenceError, "^xwd_output_invalid$"):
                    fixture.capture(runner)
                self.assertIsNotNone(runner.output)
                self.assertFalse(runner.output.exists())

    def test_rejects_invalid_exact_executable_and_window_identifiers(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            fixture = CaptureFixture(root)
            runner = FakeCaptureRunner()
            fixture.executable.chmod(0o600)
            with self.assertRaisesRegex(XwdEvidenceError, "^xwd_executable_invalid$"):
                fixture.capture(runner)
            fixture.executable.chmod(0o700)
            link = root / "xwd-link"
            link.symlink_to(fixture.executable)
            with self.assertRaisesRegex(XwdEvidenceError, "^xwd_executable_invalid$"):
                capture_window_fingerprint(
                    link, 1, fixture.private, {"DISPLAY": ":140"}, runner
                )
            for window_id in (False, 0, -1, 0x1_0000_0000):
                with self.subTest(window_id=window_id):
                    with self.assertRaisesRegex(
                        XwdEvidenceError, "^xwd_window_id_invalid$"
                    ):
                        fixture.capture(runner, window_id)  # type: ignore[arg-type]
            self.assertFalse(runner.calls)

    def test_rejects_insecure_or_symlinked_private_directory_and_ambient_env(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            fixture = CaptureFixture(root)
            runner = FakeCaptureRunner()
            fixture.private.chmod(0o755)
            with self.assertRaisesRegex(
                XwdEvidenceError, "^xwd_private_directory_invalid$"
            ):
                fixture.capture(runner)
            fixture.private.chmod(0o700)
            link = root / "capture-link"
            link.symlink_to(fixture.private, target_is_directory=True)
            with self.assertRaisesRegex(
                XwdEvidenceError, "^xwd_private_directory_invalid$"
            ):
                capture_window_fingerprint(
                    fixture.executable, 1, link, {"DISPLAY": ":140"}, runner
                )
            with self.assertRaisesRegex(XwdEvidenceError, "^xwd_environment_invalid$"):
                capture_window_fingerprint(
                    fixture.executable,
                    1,
                    fixture.private,
                    {"DISPLAY": ":140", "HOME": "/private/home"},
                    runner,
                )
            self.assertFalse(runner.calls)

    def test_start_failure_is_redacted_and_unlinks_the_precreated_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary).resolve())
            runner = FakeCaptureRunner("start_failure")

            with self.assertRaises(XwdEvidenceError) as captured:
                fixture.capture(runner)

            self.assertEqual(captured.exception.args, ("xwd_capture_start_failed",))
            self.assertTrue(captured.exception.__suppress_context__)
            self.assertNotIn("private", str(captured.exception))
            self.assertIsNotNone(runner.output)
            self.assertFalse(runner.output.exists())


class XwdFingerprintTests(unittest.TestCase):
    def test_fingerprints_only_pixel_shape_and_content(self) -> None:
        first = fingerprint_xwd(xwd(name=b"Private first title"))
        renamed = fingerprint_xwd(xwd(name=b"Private second title"))
        changed = fingerprint_xwd(xwd(pixels=bytes.fromhex("10203000 40506100")))

        self.assertEqual(len(first), FINGERPRINT_BYTES)
        self.assertEqual(first, renamed)
        self.assertNotEqual(first, changed)

    def test_accepts_bounded_colormap_before_pixels(self) -> None:
        color = struct.pack(">IHHHBB", 0x10, 0xFFFF, 0x8000, 0, 0x07, 0)
        self.assertEqual(
            len(fingerprint_xwd(xwd(colors=color, color_count=1))),
            FINGERPRINT_BYTES,
        )

    def test_rejects_nonbytes_and_truncated_headers(self) -> None:
        for payload, category in (
            ("private", "xwd_not_bytes"),
            (b"private", "xwd_header_truncated"),
        ):
            with self.subTest(category=category):
                with self.assertRaisesRegex(XwdEvidenceError, f"^{category}$"):
                    fingerprint_xwd(payload)  # type: ignore[arg-type]

    def test_rejects_unsupported_version_or_pixmap_format(self) -> None:
        for index in (1, 2):
            fields = list(struct.unpack(">25I", xwd()[:100]))
            fields[index] = 99
            payload = struct.pack(">25I", *fields) + xwd()[100:]
            with self.subTest(index=index):
                with self.assertRaisesRegex(XwdEvidenceError, "^xwd_format_unsupported$"):
                    fingerprint_xwd(payload)

    def test_rejects_invalid_header_name_and_embedded_nul(self) -> None:
        invalid = bytearray(xwd())
        invalid[103] = 0
        with self.assertRaisesRegex(XwdEvidenceError, "^xwd_window_name_invalid$"):
            fingerprint_xwd(bytes(invalid))

        missing_terminal_nul = bytearray(xwd())
        header_size = struct.unpack(">I", missing_terminal_nul[:4])[0]
        missing_terminal_nul[header_size - 1] = ord("x")
        with self.assertRaisesRegex(XwdEvidenceError, "^xwd_window_name_invalid$"):
            fingerprint_xwd(bytes(missing_terminal_nul))

    def test_rejects_dimensions_scanline_and_payload_mismatch(self) -> None:
        cases = (
            (xwd(width=0), "xwd_dimensions_invalid"),
            (xwd(width=MAX_DIMENSION + 1), "xwd_dimensions_invalid"),
            (xwd(bytes_per_line=1), "xwd_scanline_invalid"),
            (xwd()[:-1], "xwd_pixel_payload_invalid"),
            (xwd() + b"x", "xwd_pixel_payload_invalid"),
        )
        for payload, category in cases:
            with self.subTest(category=category):
                with self.assertRaisesRegex(XwdEvidenceError, f"^{category}$"):
                    fingerprint_xwd(payload)

    def test_rejects_window_dimension_and_colormap_mismatch(self) -> None:
        fields = list(struct.unpack(">25I", xwd()[:100]))
        fields[20] = 3
        mismatch = struct.pack(">25I", *fields) + xwd()[100:]
        with self.assertRaisesRegex(XwdEvidenceError, "^xwd_window_dimensions_mismatch$"):
            fingerprint_xwd(mismatch)

        with self.assertRaisesRegex(XwdEvidenceError, "^xwd_colormap_invalid$"):
            fingerprint_xwd(xwd(color_count=1, colormap_entries=0))

    def test_rejects_oversized_capture_before_parsing_private_content(self) -> None:
        with self.assertRaisesRegex(XwdEvidenceError, "^xwd_capture_too_large$"):
            fingerprint_xwd(b"x" * (MAX_CAPTURE_BYTES + 1))


class XwdSampleFingerprintTests(unittest.TestCase):
    def test_hashes_only_selected_fixed_pixels_and_layout(self) -> None:
        base = bytes.fromhex(
            "01020300 11121300 21222300 31323300 41424300 51525300"
        )
        unsampled_change = bytes.fromhex(
            "01020300 111213ff 21222300 31323300 41424300 51525300"
        )
        sampled_change = bytes.fromhex(
            "01020300 11121300 21222300 31323300 41424300 515253ff"
        )
        points = ((0, 0), (2, 1))

        fingerprint = fingerprint_xwd_samples(
            xwd(base, name=b"First private title", width=3, height=2, bytes_per_line=12),
            points,
        )
        renamed = fingerprint_xwd_samples(
            xwd(base, name=b"Second private title", width=3, height=2, bytes_per_line=12),
            points,
        )
        unsampled = fingerprint_xwd_samples(
            xwd(unsampled_change, width=3, height=2, bytes_per_line=12), points
        )
        changed = fingerprint_xwd_samples(
            xwd(sampled_change, width=3, height=2, bytes_per_line=12), points
        )

        self.assertEqual(len(fingerprint), FINGERPRINT_BYTES)
        self.assertEqual(fingerprint, renamed)
        self.assertEqual(fingerprint, unsampled)
        self.assertNotEqual(fingerprint, changed)

    def test_uses_scanline_layout_without_hashing_padding(self) -> None:
        first = xwd(
            bytes.fromhex("10 aabbcc 20 ddeeff"),
            width=1,
            height=2,
            bits_per_pixel=8,
            bytes_per_line=4,
        )
        changed_padding = xwd(
            bytes.fromhex("10 010203 20 040506"),
            width=1,
            height=2,
            bits_per_pixel=8,
            bytes_per_line=4,
        )
        points = ((0, 0), (0, 1))

        self.assertEqual(
            fingerprint_xwd_samples(first, points),
            fingerprint_xwd_samples(changed_padding, points),
        )

    def test_rejects_malformed_duplicate_and_out_of_bounds_points(self) -> None:
        payload = xwd()
        cases = (
            (),
            tuple((0, 0) for _ in range(17)),
            ((0, 0), (0, 0)),
            ((2, 0),),
            ((0, 1),),
            ((-1, 0),),
            ((False, 0),),
            ([0, 0],),
            "private",
        )
        for points in cases:
            with self.subTest(points=points):
                with self.assertRaisesRegex(
                    XwdEvidenceError, "^xwd_sample_points_invalid$"
                ):
                    fingerprint_xwd_samples(payload, points)  # type: ignore[arg-type]

    def test_rejects_non_byte_aligned_sample_layout(self) -> None:
        payload = xwd(
            b"\x00\x00",
            width=2,
            height=1,
            bits_per_pixel=7,
            bytes_per_line=2,
        )
        with self.assertRaisesRegex(
            XwdEvidenceError, "^xwd_sample_layout_invalid$"
        ):
            fingerprint_xwd_samples(payload, ((0, 0),))

    def test_binds_samples_to_the_declared_pixel_layout(self) -> None:
        points = ((0, 0), (1, 0))
        little_endian = fingerprint_xwd_samples(xwd(byte_order=0), points)
        big_endian = fingerprint_xwd_samples(xwd(byte_order=1), points)

        self.assertNotEqual(little_endian, big_endian)


class SurfaceTransitionTests(unittest.TestCase):
    def test_requires_every_expected_surface_to_change(self) -> None:
        expected = ("panel_primary", "launcher", "quick_settings")
        before = {name: bytes([index]) * FINGERPRINT_BYTES for index, name in enumerate(expected)}
        after = {
            name: bytes([index + len(expected)]) * FINGERPRINT_BYTES
            for index, name in enumerate(expected)
        }

        summary = require_complete_surface_transition(before, after, expected)

        self.assertEqual(
            summary,
            SurfaceTransitionSummary(3, 3, True),
        )
        self.assertTrue(
            all(
                type(getattr(summary, field.name)) in (bool, int)
                for field in dataclasses.fields(summary)
            )
        )

    def test_rejects_missing_extra_or_duplicate_surface_sets(self) -> None:
        fingerprint = b"a" * FINGERPRINT_BYTES
        changed = b"b" * FINGERPRINT_BYTES
        cases = (
            ({}, {}, (), "surface_set_invalid"),
            ({"panel": fingerprint}, {"panel": changed}, ("panel", "panel"), "surface_set_invalid"),
            ({"panel": fingerprint}, {"panel": changed}, ("not-valid",), "surface_set_invalid"),
            ({"panel": fingerprint}, {"other": changed}, ("panel",), "surface_set_incomplete"),
        )
        for before, after, expected, category in cases:
            with self.subTest(category=category):
                with self.assertRaisesRegex(XwdEvidenceError, f"^{category}$"):
                    require_complete_surface_transition(before, after, expected)

    def test_rejects_invalid_or_unchanged_fingerprints(self) -> None:
        fingerprint = b"a" * FINGERPRINT_BYTES
        with self.assertRaisesRegex(XwdEvidenceError, "^surface_fingerprint_invalid$"):
            require_complete_surface_transition(
                {"panel": b"short"}, {"panel": fingerprint}, ("panel",)
            )
        with self.assertRaisesRegex(XwdEvidenceError, "^surface_transition_incomplete$"):
            require_complete_surface_transition(
                {"panel": fingerprint}, {"panel": fingerprint}, ("panel",)
            )


if __name__ == "__main__":
    unittest.main()
