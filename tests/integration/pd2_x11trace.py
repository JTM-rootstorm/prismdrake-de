#!/usr/bin/env python3
"""Bounded, redacting parser for Prismdrake's x11trace 1.4 evidence.

The raw trace is private transient input.  This module intentionally returns
only categorical booleans and counters; it never retains protocol lines or
protocol identifiers in its result.
"""

from __future__ import annotations

import re
from dataclasses import dataclass


MAX_TRACE_BYTES = 8 * 1_048_576
MAX_TRACE_LINES = 131_072
MAX_LINE_BYTES = 65_536
MAX_REQUESTS = 32_768


class X11TraceError(ValueError):
    """A fixed, redacted x11trace contract failure."""


@dataclass(frozen=True, slots=True)
class X11TraceSummary:
    """Categorical Tier C protocol observations with no raw trace values."""

    trace_complete: bool
    request_count: int
    reply_boundary_count: int
    forbidden_request_count: int
    core_get_image_count: int
    mit_shm_get_image_count: int
    composite_name_window_pixmap_count: int
    composite_redirect_window_count: int
    composite_redirect_subwindows_count: int
    backdrop_property_mutation_count: int
    shell_side_capture_observed: bool
    blur_attempt_observed: bool


_CLIENT_SETUP = re.compile(
    r"^(?P<connection>[0-9a-f]{3}):<: am (?:lsb|msb)-first want 11:0 "
    r"authorising with '' of length 0$"
)
_SERVER_SETUP = re.compile(
    r"^(?P<connection>[0-9a-f]{3}):>: Success, version is 11:0 "
    r"vendor='[^']*'(?:\.\.\.)? release=[0-9]+ resource-id=0x[0-9a-f]+ "
    r"resource-mask=0x[0-9a-f]+ .+ roots=\{.+;\};$"
)
_REQUEST = re.compile(
    r"^(?P<connection>[0-9a-f]{3}):<:(?P<sequence>[0-9a-f]{4}):"
    r"[ ]+(?P<length>[0-9]+): "
    r"(?:(?P<extension>[A-Za-z][A-Za-z0-9 -]*)-)?"
    r"Request\((?P<opcode>[0-9]+)(?:,(?P<minor>[0-9]+))?\): "
    r"(?P<operation>[A-Za-z][A-Za-z0-9]*)(?: .*)?$"
)
_REPLY = re.compile(
    r"^(?P<connection>[0-9a-f]{3}):>:(?P<sequence>[0-9a-f]{4}):"
    r"[0-9]+: Reply to (?P<operation>[A-Za-z][A-Za-z0-9]*):(?: .*)?$"
)
_SERVER_EVENT = re.compile(
    r"^(?P<connection>[0-9a-f]{3}):>:[0-9a-f]{4}: "
    r"Event (?:\(generated\) )?[A-Za-z][A-Za-z0-9-]*(?:\([0-9]+\))?(?: .*)?$"
)
_SERVER_ERROR = re.compile(
    r"^(?P<connection>[0-9a-f]{3}):>:[0-9a-f]{4}:(?:[0-9]+: )?"
    r"(?:Error(?: .*)?|unexpected Reply(?::.*| .*)?)$"
)
_CLIENT_GOODBYE = re.compile(r"^(?P<connection>[0-9a-f]{3}):>: GoodBye$")
_CLIENT_EOF = re.compile(r"^(?P<connection>[0-9a-f]{3}):<:sent EOF$")

_BACKDROP_ATOMS = (
    "_KDE_NET_WM_BLUR_BEHIND_REGION",
    "GW_BACKDROP_V1",
)


def _fail(category: str) -> None:
    raise X11TraceError(category)


def _contains_backdrop_property(details: str) -> bool:
    """Match only ChangeProperty's decoded property field."""

    for atom in _BACKDROP_ATOMS:
        pattern = (
            r"(?:^| )property=(?:0x[0-9a-fA-F]+|[0-9]+)\(\""
            + re.escape(atom)
            + r"\"\)(?: |$)"
        )
        if re.search(pattern, details) is not None:
            return True
    return False


def parse_x11trace(
    payload: bytes,
    *,
    capture_complete: bool,
) -> X11TraceSummary:
    """Parse a complete x11trace 1.4 capture or fail with a fixed category.

    ``capture_complete`` must be true only after the supervising collector has
    observed a clean x11trace exit and verified that neither its byte cap nor
    its time cap truncated the private trace.
    """

    if capture_complete is not True:
        _fail("trace_not_complete")
    if not isinstance(payload, bytes):
        _fail("trace_not_bytes")
    if not payload:
        _fail("trace_empty")
    if len(payload) > MAX_TRACE_BYTES:
        _fail("trace_byte_limit_exceeded")
    if b"\x00" in payload:
        _fail("trace_contains_nul")
    if not payload.endswith(b"\n"):
        _fail("trace_missing_terminal_newline")

    try:
        text = payload.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        _fail("trace_not_utf8")

    lines = text[:-1].split("\n")
    if len(lines) > MAX_TRACE_LINES:
        _fail("trace_line_limit_exceeded")
    if not lines:
        _fail("trace_empty")
    for line in lines:
        if not line or len(line.encode("utf-8")) > MAX_LINE_BYTES:
            _fail("trace_line_invalid")
        if any(ord(character) < 0x20 or ord(character) == 0x7F for character in line):
            _fail("trace_control_character")

    client_setups: set[str] = set()
    server_setups: set[str] = set()
    requests: dict[tuple[str, str], str] = {}
    reply_boundaries: set[tuple[str, str]] = set()
    closed_connections: set[str] = set()

    core_get_image_count = 0
    mit_shm_get_image_count = 0
    composite_name_window_pixmap_count = 0
    composite_redirect_window_count = 0
    composite_redirect_subwindows_count = 0
    backdrop_property_mutation_count = 0

    for line in lines:
        match = _CLIENT_SETUP.fullmatch(line)
        if match is not None:
            connection = match.group("connection")
            if connection in client_setups:
                _fail("trace_duplicate_client_setup")
            client_setups.add(connection)
            continue

        match = _SERVER_SETUP.fullmatch(line)
        if match is not None:
            connection = match.group("connection")
            if connection not in client_setups:
                _fail("trace_server_setup_before_client")
            if connection in server_setups:
                _fail("trace_duplicate_server_setup")
            server_setups.add(connection)
            continue

        match = _REQUEST.fullmatch(line)
        if match is not None:
            connection = match.group("connection")
            sequence = match.group("sequence")
            key = (connection, sequence)
            if (
                connection not in client_setups
                or connection not in server_setups
                or connection in closed_connections
            ):
                _fail("trace_request_before_handshake")
            if key in requests:
                _fail("trace_duplicate_request")
            if len(requests) >= MAX_REQUESTS:
                _fail("trace_request_limit_exceeded")

            extension = match.group("extension")
            opcode = int(match.group("opcode"))
            operation = match.group("operation")
            minor = match.group("minor")
            # x11trace 1.4 does not decode MIT-SHM CreateSegment (minor 7).
            # Its fixed extension/minor identity cannot be the forbidden
            # GetImage request (minor 4), so retain the record and still fail
            # closed for every other undecoded request.
            if operation == "UNKNOWN" and not (
                extension == "MIT-SHM" and minor == "7"
            ):
                _fail("trace_contains_undecoded_request")
            requests[key] = operation

            if extension is None and opcode == 73:
                core_get_image_count += 1
            elif extension == "MIT-SHM" and operation == "ShmGetImage":
                mit_shm_get_image_count += 1
            elif extension == "Composite":
                if operation == "NameWindowPixmap":
                    composite_name_window_pixmap_count += 1
                elif operation == "RedirectWindow":
                    composite_redirect_window_count += 1
                elif operation == "RedirectSubwindows":
                    composite_redirect_subwindows_count += 1

            if extension is None and operation == "ChangeProperty":
                operation_offset = line.find(operation) + len(operation)
                if _contains_backdrop_property(line[operation_offset:]):
                    backdrop_property_mutation_count += 1
            continue

        match = _REPLY.fullmatch(line)
        if match is not None:
            connection = match.group("connection")
            sequence = match.group("sequence")
            key = (connection, sequence)
            if connection in closed_connections:
                _fail("trace_reply_without_request")
            request_operation = requests.get(key)
            if request_operation is None:
                _fail("trace_reply_without_request")
            if request_operation != match.group("operation"):
                _fail("trace_reply_mismatch")
            if key in reply_boundaries:
                _fail("trace_duplicate_reply")
            reply_boundaries.add(key)
            continue

        match = _SERVER_EVENT.fullmatch(line) or _SERVER_ERROR.fullmatch(line)
        if match is not None:
            if (
                match.group("connection") not in server_setups
                or match.group("connection") in closed_connections
            ):
                _fail("trace_server_packet_before_handshake")
            continue

        match = _CLIENT_GOODBYE.fullmatch(line) or _CLIENT_EOF.fullmatch(line)
        if match is not None:
            connection = match.group("connection")
            if connection not in server_setups or connection in closed_connections:
                _fail("trace_grammar_invalid")
            closed_connections.add(connection)
            continue

        _fail("trace_grammar_invalid")

    if not client_setups or client_setups != server_setups:
        _fail("trace_handshake_incomplete")
    if not requests:
        _fail("trace_has_no_requests")
    if not reply_boundaries:
        _fail("trace_has_no_reply_boundary")

    shell_side_capture_count = (
        core_get_image_count
        + mit_shm_get_image_count
        + composite_name_window_pixmap_count
        + composite_redirect_window_count
        + composite_redirect_subwindows_count
    )
    forbidden_request_count = shell_side_capture_count + backdrop_property_mutation_count

    return X11TraceSummary(
        trace_complete=True,
        request_count=len(requests),
        reply_boundary_count=len(reply_boundaries),
        forbidden_request_count=forbidden_request_count,
        core_get_image_count=core_get_image_count,
        mit_shm_get_image_count=mit_shm_get_image_count,
        composite_name_window_pixmap_count=composite_name_window_pixmap_count,
        composite_redirect_window_count=composite_redirect_window_count,
        composite_redirect_subwindows_count=composite_redirect_subwindows_count,
        backdrop_property_mutation_count=backdrop_property_mutation_count,
        shell_side_capture_observed=shell_side_capture_count > 0,
        blur_attempt_observed=backdrop_property_mutation_count > 0,
    )


def require_tier_c_no_capture_or_blur(
    payload: bytes,
    *,
    capture_complete: bool,
) -> X11TraceSummary:
    """Return a clean Tier C summary or reject it with one redacted category."""

    summary = parse_x11trace(payload, capture_complete=capture_complete)
    if summary.forbidden_request_count != 0:
        _fail("tier_c_forbidden_x11_operation")
    return summary
