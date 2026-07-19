#!/usr/bin/env python3
"""Hermetic contract tests for bounded x11trace 1.4 parsing."""

from __future__ import annotations

import dataclasses
import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
from pd2_x11trace import (  # noqa: E402
    MAX_LINE_BYTES,
    MAX_REQUESTS,
    MAX_TRACE_BYTES,
    MAX_TRACE_LINES,
    X11TraceError,
    parse_x11trace,
    require_tier_c_no_capture_or_blur,
)


HANDSHAKE = (
    "000:<: am lsb-first want 11:0 authorising with '' of length 0\n"
    "000:>: Success, version is 11:0 vendor='The X.Org Founda'... "
    "release=12101016 resource-id=0x00200000 resource-mask=0x001fffff "
    "motion-buffer-size=256 max-request-len=4194303 "
    "roots={root=0x0000050d root-depth=24;};\n"
)
DISCOVERY = (
    "000:<:0001: 20: Request(98): QueryExtension name='BIG-REQUESTS'\n"
    "000:>:0001:32: Reply to QueryExtension: present=true(0x01) "
    "major-opcode=133 first-event=0 first-error=0\n"
    "000:<:0002: 36: Request(16): InternAtom only-if-exists=true(0x01) "
    "name='_KDE_NET_WM_BLUR_BEHIND_REGION'\n"
    "000:>:0002:32: Reply to InternAtom: "
    "atom=0x000001ef(\"_KDE_NET_WM_BLUR_BEHIND_REGION\")\n"
    "000:<:0003: 24: Request(16): InternAtom only-if-exists=true(0x01) "
    "name='GW_BACKDROP_V1'\n"
    "000:>:0003:32: Reply to InternAtom: atom=0x000001f0(\"GW_BACKDROP_V1\")\n"
)


def trace(*request_lines: str, discovery: str = DISCOVERY) -> bytes:
    return (HANDSHAKE + discovery + "".join(f"{line}\n" for line in request_lines)).encode("utf-8")


class X11TraceParserTests(unittest.TestCase):
    def parse(self, payload: bytes):  # noqa: ANN201
        return parse_x11trace(payload, capture_complete=True)

    def require_clean(self, payload: bytes):  # noqa: ANN201
        return require_tier_c_no_capture_or_blur(payload, capture_complete=True)

    def test_allows_harmless_extension_and_atom_discovery(self) -> None:
        summary = self.require_clean(trace())

        self.assertTrue(summary.trace_complete)
        self.assertEqual(summary.request_count, 3)
        self.assertEqual(summary.reply_boundary_count, 3)
        self.assertEqual(summary.forbidden_request_count, 0)
        self.assertFalse(summary.shell_side_capture_observed)
        self.assertFalse(summary.blur_attempt_observed)
        self.assertTrue(
            all(
                type(getattr(summary, field.name)) in (bool, int)
                for field in dataclasses.fields(summary)
            )
        )

    def test_rejects_core_get_image_opcode_even_if_name_is_misleading(self) -> None:
        payload = trace(
            "000:<:0004: 20: Request(73): HarmlessName drawable=0x00200001 "
            "x=0 y=0 width=1 height=1"
        )
        summary = self.parse(payload)

        self.assertEqual(summary.core_get_image_count, 1)
        self.assertEqual(summary.forbidden_request_count, 1)
        self.assertTrue(summary.shell_side_capture_observed)
        with self.assertRaisesRegex(X11TraceError, "^tier_c_forbidden_x11_operation$"):
            self.require_clean(payload)

    def test_rejects_mit_shm_get_image(self) -> None:
        payload = trace(
            "000:<:0004: 40: MIT-SHM-Request(130,4): ShmGetImage "
            "drawable=0x00200001 x=0 y=0 width=1 height=1"
        )
        summary = self.parse(payload)

        self.assertEqual(summary.mit_shm_get_image_count, 1)
        self.assertEqual(summary.forbidden_request_count, 1)
        self.assertTrue(summary.shell_side_capture_observed)
        with self.assertRaisesRegex(X11TraceError, "^tier_c_forbidden_x11_operation$"):
            self.require_clean(payload)

    def test_rejects_each_composite_capture_or_redirect_request(self) -> None:
        operations = (
            ("NameWindowPixmap", "composite_name_window_pixmap_count"),
            ("RedirectWindow", "composite_redirect_window_count"),
            ("RedirectSubwindows", "composite_redirect_subwindows_count"),
        )
        for operation, field in operations:
            with self.subTest(operation=operation):
                payload = trace(
                    f"000:<:0004: 12: Composite-Request(142,0): {operation} "
                    "window=0x00200001"
                )
                summary = self.parse(payload)
                self.assertEqual(getattr(summary, field), 1)
                self.assertEqual(summary.forbidden_request_count, 1)
                self.assertTrue(summary.shell_side_capture_observed)
                with self.assertRaisesRegex(
                    X11TraceError, "^tier_c_forbidden_x11_operation$"
                ):
                    self.require_clean(payload)

    def test_rejects_each_fixed_backdrop_property_mutation(self) -> None:
        for index, atom in enumerate(
            ("_KDE_NET_WM_BLUR_BEHIND_REGION", "GW_BACKDROP_V1"), start=0x1EF
        ):
            with self.subTest(atom=atom):
                payload = trace(
                    "000:<:0004: 32: Request(18): ChangeProperty "
                    f"mode=Replace(0x00) window=0x00200001 property=0x{index:08x}(\"{atom}\") "
                    "type=0x00000006(\"CARDINAL\") format=32 data={0x00000001}"
                )
                summary = self.parse(payload)
                self.assertEqual(summary.backdrop_property_mutation_count, 1)
                self.assertEqual(summary.forbidden_request_count, 1)
                self.assertTrue(summary.blur_attempt_observed)
                with self.assertRaisesRegex(
                    X11TraceError, "^tier_c_forbidden_x11_operation$"
                ):
                    self.require_clean(payload)

    def test_does_not_treat_fixed_atom_text_outside_property_field_as_mutation(self) -> None:
        payload = trace(
            "000:<:0004: 32: Request(18): ChangeProperty mode=Replace(0x00) "
            "window=0x00200001 property=0x000001f1(\"SAFE_PROPERTY\") "
            "type=0x0000001f(\"STRING\") format=8 "
            "data='_KDE_NET_WM_BLUR_BEHIND_REGION'"
        )
        summary = self.require_clean(payload)

        self.assertEqual(summary.backdrop_property_mutation_count, 0)
        self.assertEqual(summary.forbidden_request_count, 0)

    def test_requires_supervisor_completion(self) -> None:
        with self.assertRaisesRegex(X11TraceError, "^trace_not_complete$"):
            parse_x11trace(trace(), capture_complete=False)

    def test_rejects_missing_terminal_newline_as_truncation(self) -> None:
        with self.assertRaisesRegex(X11TraceError, "^trace_missing_terminal_newline$"):
            self.parse(trace()[:-1])

    def test_rejects_capture_without_complete_request_reply_boundary(self) -> None:
        payload = (
            HANDSHAKE
            + "000:<:0001: 20: Request(98): QueryExtension name='BIG-REQUESTS'\n"
        ).encode("utf-8")
        with self.assertRaisesRegex(X11TraceError, "^trace_has_no_reply_boundary$"):
            self.parse(payload)

    def test_rejects_incomplete_handshake(self) -> None:
        payload = DISCOVERY.encode("utf-8")
        with self.assertRaisesRegex(X11TraceError, "^trace_request_before_handshake$"):
            self.parse(payload)

    def test_rejects_invalid_utf8_without_echoing_input(self) -> None:
        private_fragment = b"private-title-should-not-escape"
        payload = trace() + private_fragment + b"\xff\n"
        with self.assertRaises(X11TraceError) as context:
            self.parse(payload)
        self.assertEqual(str(context.exception), "trace_not_utf8")
        self.assertNotIn(private_fragment.decode("ascii"), str(context.exception))

    def test_rejects_nul(self) -> None:
        with self.assertRaisesRegex(X11TraceError, "^trace_contains_nul$"):
            self.parse(trace() + b"\x00")

    def test_rejects_carriage_return_line_endings(self) -> None:
        with self.assertRaisesRegex(X11TraceError, "^trace_control_character$"):
            self.parse(trace().replace(b"\n", b"\r\n"))

    def test_rejects_unknown_or_multiline_grammar(self) -> None:
        for suffix in (
            "not x11trace grammar\n",
            "000:<:0004: 12: DRI3-Request(136,0): UNKNOWN opcode=0x88\n",
            "000:<:0004: 12:\tRequest(43): GetInputFocus\n",
        ):
            with self.subTest(suffix=suffix[:20]):
                with self.assertRaises(X11TraceError):
                    self.parse(trace() + suffix.encode("utf-8"))

    def test_rejects_oversized_byte_line_and_line_count_inputs(self) -> None:
        oversized_bytes = b"x" * (MAX_TRACE_BYTES + 1)
        with self.assertRaisesRegex(X11TraceError, "^trace_byte_limit_exceeded$"):
            self.parse(oversized_bytes)

        oversized_line = (
            HANDSHAKE
            + "000:<:0001: 20: Request(98): QueryExtension name='"
            + "x" * MAX_LINE_BYTES
            + "'\n"
        ).encode("utf-8")
        with self.assertRaisesRegex(X11TraceError, "^trace_line_invalid$"):
            self.parse(oversized_line)

        too_many_lines = ("x\n" * (MAX_TRACE_LINES + 1)).encode("ascii")
        with self.assertRaisesRegex(X11TraceError, "^trace_line_limit_exceeded$"):
            self.parse(too_many_lines)

    def test_rejects_request_count_above_bound(self) -> None:
        lines = [
            f"000:<:{index:04x}: 4: Request(43): GetInputFocus"
            for index in range(1, MAX_REQUESTS + 2)
        ]
        payload = trace(
            *lines,
            discovery=(
                "000:<:0000: 20: Request(98): QueryExtension name='BIG-REQUESTS'\n"
                "000:>:0000:32: Reply to QueryExtension: present=true(0x01) "
                "major-opcode=133 first-event=0 first-error=0\n"
            ),
        )
        with self.assertRaisesRegex(X11TraceError, "^trace_request_limit_exceeded$"):
            self.parse(payload)


if __name__ == "__main__":
    unittest.main()
