#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly script_dir
source_dir="$(cd -- "${script_dir}/.." && pwd)"
readonly source_dir
readonly build_dir="${1:-/var/tmp/prismdrake-pd1-toolkit-spike-build}"
readonly output_root="${2:-/mnt/shared/pd1-toolkit-evidence}"
readonly profile="${3:-lustre}"
readonly requested_text_scale="${4:-}"
readonly scenario="${5:-${profile}}"
readonly display_number="${PRISMDRAKE_SPIKE_DISPLAY:-99}"
readonly display=":${display_number}"
readonly runtime_dir="/tmp/prismdrake-pd1-toolkit-runtime-${UID}-${display_number}"
readonly output_dir="${output_root}/${scenario}"
readonly executable="${build_dir}/prismdrake-pd1-toolkit-spike"

if [[ "${profile}" != "lustre" && "${profile}" != "forge" ]]; then
    printf 'profile must be lustre or forge\n' >&2
    exit 2
fi

for command in Xvfb dbus-run-session gdbus openbox xdotool xprop xrandr xwd xwininfo; do
    if ! command -v "${command}" >/dev/null 2>&1; then
        printf 'required command not found: %s\n' "${command}" >&2
        exit 2
    fi
done

if [[ ! -x "${executable}" ]]; then
    printf 'spike executable not found: %s\n' "${executable}" >&2
    exit 2
fi

if [[ -e "${runtime_dir}" ]]; then
    printf 'refusing to reuse runtime directory: %s\n' "${runtime_dir}" >&2
    exit 2
fi

mkdir -p -- "${runtime_dir}" "${output_dir}"
chmod 0700 "${runtime_dir}"

cleanup() {
    if [[ -n "${xvfb_pid:-}" ]]; then
        kill "${xvfb_pid}" 2>/dev/null || true
        wait "${xvfb_pid}" 2>/dev/null || true
    fi
    rm -rf -- "${runtime_dir}"
}
trap cleanup EXIT

XDG_RUNTIME_DIR="${runtime_dir}" Xvfb "${display}" \
    -screen 0 1280x720x24 -nolisten tcp >"${output_dir}/xvfb.log" 2>&1 &
xvfb_pid=$!

for _ in {1..50}; do
    if DISPLAY="${display}" xdpyinfo >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done
if ! DISPLAY="${display}" xdpyinfo >/dev/null 2>&1; then
    printf 'Xvfb did not become ready on %s\n' "${display}" >&2
    exit 1
fi

export PRISMDRAKE_SPIKE_BUILD_DIR="${build_dir}"
export PRISMDRAKE_SPIKE_DISPLAY_VALUE="${display}"
export PRISMDRAKE_SPIKE_OUTPUT_DIR="${output_dir}"
export PRISMDRAKE_SPIKE_PROFILE_VALUE="${profile}"
export PRISMDRAKE_SPIKE_TEXT_SCALE_VALUE="${requested_text_scale}"
export PRISMDRAKE_SPIKE_SOURCE_DIR="${source_dir}"
export XDG_RUNTIME_DIR="${runtime_dir}"

# The session body expands only its explicitly exported environment.
# shellcheck disable=SC2016
dbus-run-session -- bash -euo pipefail -c '
    cleanup_session() {
        for pid in "${spike_pid:-}" "${openbox_pid:-}"; do
            if [[ -n "${pid}" ]]; then
                kill "${pid}" 2>/dev/null || true
                wait "${pid}" 2>/dev/null || true
            fi
        done
    }
    trap cleanup_session EXIT

    export DISPLAY="${PRISMDRAKE_SPIKE_DISPLAY_VALUE}"
    export QT_ACCESSIBILITY=1
    export QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1
    export QT_QPA_PLATFORM=xcb
    export QT_QUICK_BACKEND=software

    openbox >"${PRISMDRAKE_SPIKE_OUTPUT_DIR}/openbox.log" 2>&1 &
    openbox_pid=$!
    gdbus call --session \
        --dest org.a11y.Bus \
        --object-path /org/a11y/bus \
        --method org.a11y.Bus.GetAddress \
        >"${PRISMDRAKE_SPIKE_OUTPUT_DIR}/atspi-bus.txt"

    extra_args=()
    if [[ "${PRISMDRAKE_SPIKE_PROFILE_VALUE}" == "lustre" ]]; then
        extra_args+=(--disable-transparency --reduced-motion)
    fi
    if [[ -n "${PRISMDRAKE_SPIKE_TEXT_SCALE_VALUE}" ]]; then
        extra_args+=(--text-scale "${PRISMDRAKE_SPIKE_TEXT_SCALE_VALUE}")
    fi
    "${PRISMDRAKE_SPIKE_BUILD_DIR}/prismdrake-pd1-toolkit-spike" \
        --profile "${PRISMDRAKE_SPIKE_PROFILE_VALUE}" \
        "${extra_args[@]}" \
        >"${PRISMDRAKE_SPIKE_OUTPUT_DIR}/application.log" 2>&1 &
    spike_pid=$!

    window_id=""
    for _ in {1..100}; do
        window_id="$(xdotool search --name "^Prismdrake PD1 toolkit evidence spike$" 2>/dev/null | head -n 1 || true)"
        if [[ -n "${window_id}" ]]; then
            break
        fi
        if ! kill -0 "${spike_pid}" 2>/dev/null; then
            wait "${spike_pid}"
        fi
        sleep 0.1
    done
    if [[ -z "${window_id}" ]]; then
        printf "spike window did not appear\n" >&2
        exit 1
    fi

    printf "%s\n" "${window_id}" >"${PRISMDRAKE_SPIKE_OUTPUT_DIR}/window-id.txt"
    xprop -id "${window_id}" \
        _NET_WM_WINDOW_TYPE _NET_WM_STRUT _NET_WM_STRUT_PARTIAL WM_HINTS WM_NORMAL_HINTS \
        >"${PRISMDRAKE_SPIKE_OUTPUT_DIR}/x11-properties.txt"
    xwininfo -id "${window_id}" -all >"${PRISMDRAKE_SPIKE_OUTPUT_DIR}/xwininfo.txt"
    xdpyinfo >"${PRISMDRAKE_SPIKE_OUTPUT_DIR}/xdpyinfo.txt"
    xrandr --current >"${PRISMDRAKE_SPIKE_OUTPUT_DIR}/xrandr.txt"

    xdotool windowfocus --sync "${window_id}"
    python3 "${PRISMDRAKE_SPIKE_SOURCE_DIR}/tests/inspect_atspi.py" \
        --expect-focused "Open launcher" \
        >"${PRISMDRAKE_SPIKE_OUTPUT_DIR}/atspi-initial.json"
    xdotool key --window "${window_id}" Tab
    python3 "${PRISMDRAKE_SPIKE_SOURCE_DIR}/tests/inspect_atspi.py" \
        --expect-focused "Files task" \
        >"${PRISMDRAKE_SPIKE_OUTPUT_DIR}/atspi-after-tab.json"
    xdotool key --window "${window_id}" shift+Tab
    python3 "${PRISMDRAKE_SPIKE_SOURCE_DIR}/tests/inspect_atspi.py" \
        --expect-focused "Open launcher" \
        >"${PRISMDRAKE_SPIKE_OUTPUT_DIR}/atspi-after-backtab.json"
    xdotool key --window "${window_id}" space
    python3 "${PRISMDRAKE_SPIKE_SOURCE_DIR}/tests/inspect_atspi.py" \
        --expect-focused "Close launcher" \
        >"${PRISMDRAKE_SPIKE_OUTPUT_DIR}/atspi-launcher-open.json"
    xwd -silent -id "${window_id}" \
        -out "${PRISMDRAKE_SPIKE_OUTPUT_DIR}/window-launcher-open.xwd"
    xdotool key --window "${window_id}" Escape
    python3 "${PRISMDRAKE_SPIKE_SOURCE_DIR}/tests/inspect_atspi.py" \
        --expect-focused "Open launcher" \
        >"${PRISMDRAKE_SPIKE_OUTPUT_DIR}/atspi-launcher-dismissed.json"
    xwd -silent -id "${window_id}" \
        -out "${PRISMDRAKE_SPIKE_OUTPUT_DIR}/window.xwd"

    kill "${spike_pid}"
    wait "${spike_pid}" 2>/dev/null || true
'

printf 'wrote %s evidence to %s\n' "${profile}" "${output_dir}"
