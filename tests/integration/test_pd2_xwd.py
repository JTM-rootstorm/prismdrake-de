#!/usr/bin/env python3
"""Hermetic tests for bounded, privacy-preserving XWD evidence."""

from __future__ import annotations

import dataclasses
import struct
import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
from pd2_xwd import (  # noqa: E402
    FINGERPRINT_BYTES,
    MAX_CAPTURE_BYTES,
    MAX_DIMENSION,
    SurfaceTransitionSummary,
    XwdEvidenceError,
    fingerprint_xwd,
    require_complete_surface_transition,
)


def xwd(
    pixels: bytes = bytes.fromhex("10203000 40506000"),
    *,
    name: bytes = b"Controlled surface",
    width: int = 2,
    height: int = 1,
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
        0,
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
