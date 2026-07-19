#!/usr/bin/env python3
"""Strict, privacy-preserving XWD fingerprints for PD2 live evidence.

Raw window captures are transient private inputs.  This module parses only the
bounded XWD shape emitted by the reference Xorg environment and returns a
SHA-256 fingerprint of the pixel payload.  It never returns window names,
pixels, or captured application content.
"""

from __future__ import annotations

import hashlib
import os
import re
import stat
import struct
import subprocess
import tempfile
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol


XWD_FILE_VERSION = 7
XWD_Z_PIXMAP = 2
XWD_HEADER_WORDS = 25
XWD_HEADER_BYTES = XWD_HEADER_WORDS * 4
XWD_COLOR_BYTES = 12
MAX_HEADER_BYTES = 4_096
MAX_CAPTURE_BYTES = 64 * 1_024 * 1_024
MAX_DIMENSION = 8_192
MAX_COLORS = 4_096
MAX_SURFACES = 8
FINGERPRINT_BYTES = 32
CAPTURE_TIMEOUT_SECONDS = 3.0
KILL_TIMEOUT_SECONDS = 0.5
_DISPLAY_PATTERN = re.compile(r":[0-9]{1,5}(?:\.[0-9]{1,3})?")


class XwdEvidenceError(ValueError):
    """A fixed, redacted XWD evidence failure."""


class XwdCaptureProcess(Protocol):
    """Minimal child boundary needed for bounded capture and reap."""

    def wait(self, timeout: float | None = None) -> int: ...

    def kill(self) -> None: ...


class XwdCaptureRunner(Protocol):
    """Injectable exact-exec boundary for live and hermetic capture."""

    def start(
        self, argv: Sequence[str], environment: Mapping[str, str]
    ) -> XwdCaptureProcess: ...


class SubprocessXwdCaptureRunner:
    """Run xwd directly without a shell or inherited ambient environment."""

    def start(
        self, argv: Sequence[str], environment: Mapping[str, str]
    ) -> XwdCaptureProcess:
        return subprocess.Popen(
            tuple(argv),
            env=dict(environment),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            close_fds=True,
        )


@dataclass(frozen=True, slots=True)
class SurfaceTransitionSummary:
    """Categorical repaint evidence without surface pixels or identifiers."""

    surface_count: int
    changed_surface_count: int
    complete_transition_observed: bool


def _fail(category: str) -> None:
    raise XwdEvidenceError(category) from None


def _parse_xwd(
    payload: bytes,
) -> tuple[int, int, int, int, int, int, int, int, int, bytes]:
    """Parse one complete bounded XWD pixel plane into private layout state."""

    if not isinstance(payload, bytes):
        _fail("xwd_not_bytes")
    if len(payload) < XWD_HEADER_BYTES:
        _fail("xwd_header_truncated")
    if len(payload) > MAX_CAPTURE_BYTES:
        _fail("xwd_capture_too_large")

    try:
        fields = struct.unpack(">25I", payload[:XWD_HEADER_BYTES])
    except struct.error:
        _fail("xwd_header_invalid")

    (
        header_size,
        file_version,
        pixmap_format,
        pixmap_depth,
        pixmap_width,
        pixmap_height,
        x_offset,
        byte_order,
        bitmap_unit,
        bitmap_bit_order,
        bitmap_pad,
        bits_per_pixel,
        bytes_per_line,
        visual_class,
        red_mask,
        green_mask,
        blue_mask,
        bits_per_rgb,
        colormap_entries,
        color_count,
        window_width,
        window_height,
        window_x,
        window_y,
        window_border_width,
    ) = fields

    del (
        x_offset,
        visual_class,
        red_mask,
        green_mask,
        blue_mask,
        bits_per_rgb,
        window_x,
        window_y,
        window_border_width,
    )

    if file_version != XWD_FILE_VERSION or pixmap_format != XWD_Z_PIXMAP:
        _fail("xwd_format_unsupported")
    if not XWD_HEADER_BYTES < header_size <= MAX_HEADER_BYTES:
        _fail("xwd_header_size_invalid")
    if header_size > len(payload) or payload[header_size - 1] != 0:
        _fail("xwd_window_name_invalid")
    if b"\x00" in payload[XWD_HEADER_BYTES : header_size - 1]:
        _fail("xwd_window_name_invalid")
    if not 1 <= pixmap_depth <= 32 or not 1 <= bits_per_pixel <= 32:
        _fail("xwd_pixel_layout_invalid")
    if byte_order not in (0, 1) or bitmap_bit_order not in (0, 1):
        _fail("xwd_pixel_layout_invalid")
    if bitmap_unit not in (8, 16, 32) or bitmap_pad not in (8, 16, 32):
        _fail("xwd_pixel_layout_invalid")
    if not 1 <= pixmap_width <= MAX_DIMENSION or not 1 <= pixmap_height <= MAX_DIMENSION:
        _fail("xwd_dimensions_invalid")
    if window_width != pixmap_width or window_height != pixmap_height:
        _fail("xwd_window_dimensions_mismatch")

    minimum_line_bytes = (pixmap_width * bits_per_pixel + 7) // 8
    maximum_line_bytes = MAX_DIMENSION * 4
    if not minimum_line_bytes <= bytes_per_line <= maximum_line_bytes:
        _fail("xwd_scanline_invalid")
    if color_count > MAX_COLORS or color_count > colormap_entries:
        _fail("xwd_colormap_invalid")

    color_bytes = color_count * XWD_COLOR_BYTES
    pixel_bytes = bytes_per_line * pixmap_height
    pixel_offset = header_size + color_bytes
    if pixel_offset > len(payload) or pixel_bytes != len(payload) - pixel_offset:
        _fail("xwd_pixel_payload_invalid")

    return (
        pixmap_depth,
        pixmap_width,
        pixmap_height,
        byte_order,
        bitmap_unit,
        bitmap_bit_order,
        bitmap_pad,
        bits_per_pixel,
        bytes_per_line,
        payload[pixel_offset:],
    )


def fingerprint_xwd(payload: bytes) -> bytes:
    """Return a fixed-size fingerprint of one complete bounded XWD pixel plane."""

    (
        depth,
        width,
        height,
        _byte_order,
        _bitmap_unit,
        _bitmap_bit_order,
        _bitmap_pad,
        bits_per_pixel,
        bytes_per_line,
        pixels,
    ) = _parse_xwd(payload)
    digest = hashlib.sha256()
    digest.update(
        struct.pack(">5I", depth, width, height, bits_per_pixel, bytes_per_line)
    )
    digest.update(pixels)
    return digest.digest()


def fingerprint_xwd_samples(
    payload: bytes, points: Sequence[tuple[int, int]]
) -> bytes:
    """Fingerprint only fixed pixels at strict caller-selected coordinates."""

    (
        depth,
        width,
        height,
        byte_order,
        bitmap_unit,
        bitmap_bit_order,
        bitmap_pad,
        bits_per_pixel,
        bytes_per_line,
        pixels,
    ) = _parse_xwd(payload)
    if bits_per_pixel not in (8, 16, 24, 32):
        _fail("xwd_sample_layout_invalid")
    if (
        not isinstance(points, Sequence)
        or isinstance(points, (str, bytes, bytearray))
        or not 1 <= len(points) <= 16
    ):
        _fail("xwd_sample_points_invalid")
    normalized: list[tuple[int, int]] = []
    for point in points:
        if (
            not isinstance(point, tuple)
            or len(point) != 2
            or type(point[0]) is not int
            or type(point[1]) is not int
            or not 0 <= point[0] < width
            or not 0 <= point[1] < height
            or point in normalized
        ):
            _fail("xwd_sample_points_invalid")
        normalized.append(point)

    pixel_bytes = bits_per_pixel // 8
    digest = hashlib.sha256()
    digest.update(
        struct.pack(
            ">10I",
            depth,
            width,
            height,
            byte_order,
            bitmap_unit,
            bitmap_bit_order,
            bitmap_pad,
            bits_per_pixel,
            bytes_per_line,
            len(normalized),
        )
    )
    for x, y in normalized:
        offset = y * bytes_per_line + x * pixel_bytes
        sample = pixels[offset : offset + pixel_bytes]
        if len(sample) != pixel_bytes:
            _fail("xwd_sample_layout_invalid")
        digest.update(struct.pack(">2I", x, y))
        digest.update(sample)
    return digest.digest()


def capture_window_fingerprint(
    xwd_executable: Path,
    window_id: int,
    private_directory: Path,
    x11_environment: Mapping[str, str],
    runner: XwdCaptureRunner | None = None,
) -> bytes:
    """Capture one exact X11 window and return only its bounded fingerprint."""

    _require_xwd_executable(xwd_executable)
    if type(window_id) is not int or not 1 <= window_id <= 0xFFFFFFFF:
        _fail("xwd_window_id_invalid")
    _require_private_directory(private_directory)
    environment = _minimal_x11_environment(x11_environment)
    capture_runner = runner or SubprocessXwdCaptureRunner()

    output: Path | None = None
    descriptor: int | None = None
    try:
        try:
            descriptor, native_output = tempfile.mkstemp(
                prefix=".pd2-xwd-", suffix=".xwd", dir=private_directory
            )
            output = Path(native_output)
            os.fchmod(descriptor, 0o600)
            created = os.fstat(descriptor)
            if (
                not stat.S_ISREG(created.st_mode)
                or created.st_uid != os.geteuid()
                or stat.S_IMODE(created.st_mode) != 0o600
                or created.st_size != 0
            ):
                _fail("xwd_output_create_failed")
        except XwdEvidenceError:
            raise
        except OSError:
            _fail("xwd_output_create_failed")
        finally:
            if descriptor is not None:
                try:
                    os.close(descriptor)
                except OSError:
                    _fail("xwd_output_create_failed")
                descriptor = None

        if output is None:
            _fail("xwd_output_create_failed")
        argv = (
            str(xwd_executable),
            "-silent",
            "-id",
            f"0x{window_id:x}",
            "-out",
            str(output),
        )
        try:
            child = capture_runner.start(argv, environment)
        except Exception:
            _fail("xwd_capture_start_failed")
        try:
            return_code = child.wait(CAPTURE_TIMEOUT_SECONDS)
        except (subprocess.TimeoutExpired, TimeoutError):
            try:
                child.kill()
                child.wait(KILL_TIMEOUT_SECONDS)
            except Exception:
                _fail("xwd_capture_cleanup_failed")
            _fail("xwd_capture_timeout")
        except Exception:
            _fail("xwd_capture_failed")
        if type(return_code) is not int or return_code != 0:
            _fail("xwd_capture_failed")
        return fingerprint_xwd(_read_capture(output, created))
    finally:
        if descriptor is not None:
            try:
                os.close(descriptor)
            except OSError:
                pass
        if output is not None:
            try:
                output.unlink(missing_ok=True)
            except OSError:
                _fail("xwd_capture_cleanup_failed")


def _require_xwd_executable(path: Path) -> None:
    try:
        metadata = path.lstat()
    except (AttributeError, OSError):
        _fail("xwd_executable_invalid")
    if (
        not path.is_absolute()
        or stat.S_ISLNK(metadata.st_mode)
        or not stat.S_ISREG(metadata.st_mode)
        or metadata.st_mode & 0o111 == 0
    ):
        _fail("xwd_executable_invalid")


def _require_private_directory(path: Path) -> None:
    try:
        metadata = path.lstat()
    except (AttributeError, OSError):
        _fail("xwd_private_directory_invalid")
    if (
        not path.is_absolute()
        or stat.S_ISLNK(metadata.st_mode)
        or not stat.S_ISDIR(metadata.st_mode)
        or metadata.st_uid != os.geteuid()
        or stat.S_IMODE(metadata.st_mode) != 0o700
    ):
        _fail("xwd_private_directory_invalid")


def _minimal_x11_environment(environment: Mapping[str, str]) -> dict[str, str]:
    if not isinstance(environment, Mapping) or not set(environment) <= {
        "DISPLAY",
        "XAUTHORITY",
    }:
        _fail("xwd_environment_invalid")
    display = environment.get("DISPLAY")
    authority = environment.get("XAUTHORITY")
    if (
        not isinstance(display, str)
        or _DISPLAY_PATTERN.fullmatch(display) is None
        or (authority is not None and (
            not isinstance(authority, str)
            or not authority
            or len(authority) > 4_096
            or "\x00" in authority
        ))
    ):
        _fail("xwd_environment_invalid")
    minimal = {"DISPLAY": display, "LC_ALL": "C"}
    if authority is not None:
        minimal["XAUTHORITY"] = authority
    return minimal


def _read_capture(path: Path, created: os.stat_result) -> bytes:
    try:
        metadata = path.lstat()
    except OSError:
        _fail("xwd_output_invalid")
    if (
        stat.S_ISLNK(metadata.st_mode)
        or not stat.S_ISREG(metadata.st_mode)
        or metadata.st_uid != os.geteuid()
        or stat.S_IMODE(metadata.st_mode) != 0o600
        or metadata.st_dev != created.st_dev
        or metadata.st_ino != created.st_ino
    ):
        _fail("xwd_output_invalid")
    if metadata.st_size > MAX_CAPTURE_BYTES:
        _fail("xwd_capture_too_large")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError:
        _fail("xwd_capture_read_failed")
    try:
        opened = os.fstat(descriptor)
        if (
            opened.st_dev != metadata.st_dev
            or opened.st_ino != metadata.st_ino
            or opened.st_size != metadata.st_size
            or stat.S_IMODE(opened.st_mode) != 0o600
        ):
            _fail("xwd_output_invalid")
        blocks: list[bytes] = []
        remaining = MAX_CAPTURE_BYTES + 1
        while remaining > 0:
            block = os.read(descriptor, min(1024 * 1024, remaining))
            if not block:
                break
            blocks.append(block)
            remaining -= len(block)
    except XwdEvidenceError:
        raise
    except OSError:
        _fail("xwd_capture_read_failed")
    finally:
        try:
            os.close(descriptor)
        except OSError:
            _fail("xwd_capture_read_failed")
    payload = b"".join(blocks)
    if len(payload) != metadata.st_size:
        _fail("xwd_output_invalid")
    return payload


def require_complete_surface_transition(
    before: Mapping[str, bytes],
    after: Mapping[str, bytes],
    expected_surfaces: Sequence[str],
) -> SurfaceTransitionSummary:
    """Require every fixed controlled surface to have a new pixel fingerprint."""

    expected = tuple(expected_surfaces)
    if not expected or len(expected) > MAX_SURFACES or len(set(expected)) != len(expected):
        _fail("surface_set_invalid")
    if any(
        not isinstance(surface, str)
        or not surface
        or len(surface) > 64
        or not surface.replace("_", "").isalnum()
        for surface in expected
    ):
        _fail("surface_set_invalid")
    if set(before) != set(expected) or set(after) != set(expected):
        _fail("surface_set_incomplete")

    changed = 0
    for surface in expected:
        before_fingerprint = before[surface]
        after_fingerprint = after[surface]
        if (
            not isinstance(before_fingerprint, bytes)
            or not isinstance(after_fingerprint, bytes)
            or len(before_fingerprint) != FINGERPRINT_BYTES
            or len(after_fingerprint) != FINGERPRINT_BYTES
        ):
            _fail("surface_fingerprint_invalid")
        if before_fingerprint == after_fingerprint:
            _fail("surface_transition_incomplete")
        changed += 1

    return SurfaceTransitionSummary(
        surface_count=len(expected),
        changed_surface_count=changed,
        complete_transition_observed=True,
    )
