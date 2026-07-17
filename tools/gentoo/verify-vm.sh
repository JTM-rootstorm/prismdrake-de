#!/bin/sh
# Read-only checks for the Prismdrake Gentoo reference VM.

set -u

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
# shellcheck source=tools/gentoo/vm-checks.sh
. "$SCRIPT_DIR/vm-checks.sh"
DEFAULT_WORKSPACE=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd -P)
WORKSPACE=${PRISMDRAKE_WORKSPACE:-$DEFAULT_WORKSPACE}
SHARED_PATH=${PRISMDRAKE_SHARED_PATH:-/mnt/shared}
FAILURES=0
WARNINGS=0

usage() {
	cat <<'EOF'
Usage: verify-vm.sh [--workspace PATH] [--shared-path PATH]

Run read-only checks for the Gentoo guest, shared artifact mount, local Portage
repository, development package set, and PD1 test tools. Environment overrides:
PRISMDRAKE_WORKSPACE and PRISMDRAKE_SHARED_PATH.
EOF
}

fail() {
	printf 'FAIL: %s\n' "$1" >&2
	FAILURES=$((FAILURES + 1))
}

warn() {
	printf 'WARN: %s\n' "$1" >&2
	WARNINGS=$((WARNINGS + 1))
}

pass() {
	printf 'PASS: %s\n' "$1"
}

while [ "$#" -gt 0 ]; do
	case "$1" in
		--workspace)
			[ "$#" -ge 2 ] || { printf 'Missing value for --workspace\n' >&2; exit 2; }
			WORKSPACE=$2
			shift 2
			;;
		--shared-path)
			[ "$#" -ge 2 ] || { printf 'Missing value for --shared-path\n' >&2; exit 2; }
			SHARED_PATH=$2
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			printf 'Unknown argument: %s\n' "$1" >&2
			usage >&2
			exit 2
			;;
	esac
done

case "$WORKSPACE$SHARED_PATH" in
	*'
'*)
		printf 'Workspace and shared paths must not contain newlines\n' >&2
		exit 2
		;;
esac

WORKSPACE=$(readlink -m -- "$WORKSPACE" 2>/dev/null || printf '%s\n' "$WORKSPACE")
SHARED_PATH=$(readlink -m -- "$SHARED_PATH" 2>/dev/null || printf '%s\n' "$SHARED_PATH")
PORTAGE_REPO=$WORKSPACE/packaging/gentoo/repository

printf 'Prismdrake Gentoo VM verification\n'
printf '  workspace: %s\n' "$WORKSPACE"
printf '  shared path: %s\n' "$SHARED_PATH"

if [ -r /etc/gentoo-release ]; then
	pass "Gentoo guest detected ($(cat /etc/gentoo-release))"
else
	fail '/etc/gentoo-release is missing or unreadable'
fi

ARCH=$(uname -m 2>/dev/null || printf unknown)
if [ "$ARCH" = x86_64 ]; then
	pass 'guest architecture is x86_64 (Gentoo amd64)'
else
	fail "guest architecture is $ARCH; PD1 expects x86_64"
fi

if command -v eselect >/dev/null 2>&1; then
	printf '\nProfile\n'
	eselect profile show 2>&1 || fail 'eselect could not report the selected profile'
else
	fail 'eselect is unavailable'
fi

printf 'Portage: %s\n' "$(emerge --version 2>/dev/null | sed -n '1p' || printf unavailable)"
printf 'compiler: %s\n' "$(gcc --version 2>/dev/null | sed -n '1p' || printf unavailable)"
printf 'init: %s\n' "$(readlink /proc/1/exe 2>/dev/null || printf unknown)"

printf '\nResources\n'
uname -r 2>/dev/null || true
awk '/MemTotal:/ { printf "memory: %.1f GiB\n", $2 / 1048576 }' /proc/meminfo 2>/dev/null || true
df -h / 2>/dev/null || warn 'could not inspect root filesystem capacity'
AVAILABLE_KIB=$(df -Pk / 2>/dev/null | awk 'NR == 2 { print $4 }' || true)
MEMORY_KIB=$(awk '/MemTotal:/ { print $2 }' /proc/meminfo 2>/dev/null || true)
if RESOURCE_ERROR=$(prismdrake_validate_resources "$AVAILABLE_KIB" "$MEMORY_KIB"); then
	pass 'guest meets the 5 GiB disk and 4 GiB memory safety floors'
else
	fail "$RESOURCE_ERROR"
fi

printf '\nShared artifact mount\n'
if [ ! -d "$SHARED_PATH" ]; then
	fail "shared path does not exist: $SHARED_PATH"
elif command -v findmnt >/dev/null 2>&1; then
	MOUNT_TYPE=$(findmnt -T "$SHARED_PATH" -n -o FSTYPE 2>/dev/null || true)
	MOUNT_OPTIONS=$(findmnt -T "$SHARED_PATH" -n -o OPTIONS 2>/dev/null || true)
	if MOUNT_ERROR=$(prismdrake_validate_mount_state "$MOUNT_TYPE" "$MOUNT_OPTIONS"); then
		pass 'shared path is on read-write virtiofs'
	else
		fail "shared path: $MOUNT_ERROR"
	fi
	OWNER_MODE=$(stat -c '%u:%g %a' "$SHARED_PATH" 2>/dev/null || true)
	if [ -n "$OWNER_MODE" ]; then
		printf 'INFO: shared artifact ownership and mode: %s\n' "$OWNER_MODE"
		MODE=${OWNER_MODE##* }
		if MODE_ERROR=$(prismdrake_validate_mode "$MODE" "$SHARED_PATH"); then
			pass 'shared artifact mount is not world-writable'
		else
			fail "$MODE_ERROR"
		fi
	else
		fail 'could not inspect shared artifact ownership and mode'
	fi
else
	fail 'findmnt is unavailable'
fi

if [ ! -d "$WORKSPACE" ]; then
	fail "workspace does not exist: $WORKSPACE"
else
	case "$WORKSPACE/" in
		"$SHARED_PATH/"*) pass 'workspace is contained by the shared path' ;;
		*) fail 'workspace is not contained by the shared path' ;;
	esac
	if command -v findmnt >/dev/null 2>&1; then
		WORKSPACE_MOUNT_TYPE=$(findmnt -T "$WORKSPACE" -n -o FSTYPE 2>/dev/null || true)
		WORKSPACE_MOUNT_OPTIONS=$(findmnt -T "$WORKSPACE" -n -o OPTIONS 2>/dev/null || true)
		if MOUNT_ERROR=$(prismdrake_validate_mount_state \
			"$WORKSPACE_MOUNT_TYPE" "$WORKSPACE_MOUNT_OPTIONS"); then
			pass 'workspace itself is on read-write virtiofs'
		else
			fail "workspace: $MOUNT_ERROR"
		fi
	else
		fail 'findmnt is unavailable for workspace inspection'
	fi
fi

printf '\nWorkspace ownership\n'
for EDIT_PATH in "$WORKSPACE" "$PORTAGE_REPO"; do
	if [ ! -d "$EDIT_PATH" ]; then
		fail "source path is unavailable: $EDIT_PATH"
		continue
	fi
	EDIT_OWNER=$(stat -c '%u' "$EDIT_PATH" 2>/dev/null || true)
	EDIT_OWNER_GROUP_MODE=$(stat -c '%u:%g %a' "$EDIT_PATH" 2>/dev/null || true)
	printf 'INFO: %s ownership and mode: %s\n' "$EDIT_PATH" "$EDIT_OWNER_GROUP_MODE"
	if [ -z "$EDIT_OWNER" ]; then
		fail "could not inspect source ownership: $EDIT_PATH"
		continue
	fi
	EDIT_MODE=${EDIT_OWNER_GROUP_MODE##* }
	if ! MODE_ERROR=$(prismdrake_validate_mode "$EDIT_MODE" "$EDIT_PATH"); then
		fail "$MODE_ERROR"
	fi
	EDIT_USER=$(getent passwd "$EDIT_OWNER" 2>/dev/null | cut -d: -f1)
	EDIT_READABLE=unchecked
	EDIT_WRITABLE=unchecked
	if [ "$(id -u)" -eq 0 ] && command -v runuser >/dev/null 2>&1 && [ -n "$EDIT_USER" ]; then
		EDIT_READABLE=false
		EDIT_WRITABLE=false
		runuser -u "$EDIT_USER" -- test -r "$EDIT_PATH" && EDIT_READABLE=true
		runuser -u "$EDIT_USER" -- test -w "$EDIT_PATH" && EDIT_WRITABLE=true
	elif prismdrake_is_unsigned_integer "$EDIT_OWNER" &&
		[ "$(id -u)" -eq "$EDIT_OWNER" ]; then
		EDIT_READABLE=false
		EDIT_WRITABLE=false
		[ -r "$EDIT_PATH" ] && EDIT_READABLE=true
		[ -w "$EDIT_PATH" ] && EDIT_WRITABLE=true
	fi
	if [ "$EDIT_READABLE" = unchecked ]; then
		if ! OWNER_ERROR=$(prismdrake_validate_owner_state \
			"$EDIT_OWNER" "$EDIT_USER" true true); then
			fail "$EDIT_PATH: $OWNER_ERROR"
		else
			fail "ordinary-user access was not checked for $EDIT_PATH; run verification as root or its owner"
		fi
	elif OWNER_ERROR=$(prismdrake_validate_owner_state \
		"$EDIT_OWNER" "$EDIT_USER" "$EDIT_READABLE" "$EDIT_WRITABLE"); then
		pass "$EDIT_USER can read and write $EDIT_PATH without world-write"
	else
		fail "$EDIT_PATH: $OWNER_ERROR"
	fi
done

printf '\nLocal Portage repository\n'
OVERLAY_VALID=false
REPOSITORY_NAME=$(cat "$PORTAGE_REPO/profiles/repo_name" 2>/dev/null || true)
if REPO_NAME_ERROR=$(prismdrake_validate_repo_name "$REPOSITORY_NAME"); then
	pass 'tracked prismdrake-local metadata is present'
	OVERLAY_VALID=true
else
	fail "$PORTAGE_REPO: $REPO_NAME_ERROR"
fi

REGISTERED_REPO=
REGISTERED_REPO_VALID=false
if command -v portageq >/dev/null 2>&1; then
	REGISTERED_REPO=$(portageq get_repo_path / prismdrake-local 2>/dev/null || true)
	EXPECTED_REPO=$(readlink -f "$PORTAGE_REPO" 2>/dev/null || true)
	RESOLVED_REPO=$(readlink -f "$REGISTERED_REPO" 2>/dev/null || true)
	if REPO_ERROR=$(prismdrake_validate_registered_repo "$EXPECTED_REPO" "$RESOLVED_REPO"); then
		pass 'Portage recognizes prismdrake-local'
		REGISTERED_REPO_VALID=true
	else
		fail "$REPO_ERROR"
	fi
else
	fail 'portageq is unavailable'
fi
if command -v eselect >/dev/null 2>&1 &&
	eselect repository list -i 2>/dev/null | grep -q 'prismdrake-local'; then
	pass 'eselect reports prismdrake-local as installed'
else
	fail 'eselect does not report prismdrake-local as installed'
fi

printf '\nRepository QA\n'
if [ "$OVERLAY_VALID" = true ] && command -v pkgcheck >/dev/null 2>&1; then
	QA_REPO=$(mktemp -d /tmp/prismdrake-repository-qa.XXXXXX)
	PKGCHECK_CACHE=$(mktemp -d /tmp/prismdrake-pkgcheck.XXXXXX)
	trap 'rm -rf "$QA_REPO" "$PKGCHECK_CACHE"' EXIT HUP INT TERM
	cp -a "$PORTAGE_REPO/." "$QA_REPO/"
	if pkgcheck scan --cache-dir "$PKGCHECK_CACHE" "$QA_REPO"; then
		pass 'pkgcheck scan passed'
	else
		fail 'pkgcheck scan reported repository findings'
	fi
	rm -rf "$QA_REPO" "$PKGCHECK_CACHE"
	trap - EXIT HUP INT TERM
elif [ "$OVERLAY_VALID" != true ]; then
	printf 'INFO: pkgcheck scan skipped because tracked repository metadata is invalid or unavailable\n'
else
	fail 'pkgcheck is unavailable; enable prismdrake-dev-env[portage-qa]'
fi

printf '\nTool availability\n'
for TOOL in gcc cmake ninja git gdb strace valgrind lsof Xvfb Xephyr openbox \
	dbus-daemon dbus-run-session gdbus qmake6; do
	if command -v "$TOOL" >/dev/null 2>&1; then
		printf 'PASS: %-18s available\n' "$TOOL"
	else
		fail "$TOOL is unavailable"
	fi
done
AT_SPI_LAUNCHER=
if command -v qlist >/dev/null 2>&1 &&
	qlist -IC app-accessibility/at-spi2-core >/dev/null 2>&1; then
	AT_SPI_LAUNCHER=$(qlist -e app-accessibility/at-spi2-core 2>/dev/null |
		awk '/\/at-spi-bus-launcher$/ { print; exit }')
fi
if [ -n "$AT_SPI_LAUNCHER" ] && [ -x "$AT_SPI_LAUNCHER" ]; then
	pass "AT-SPI core bus launcher is installed and executable ($AT_SPI_LAUNCHER)"
else
	fail 'AT-SPI core or its bus launcher is unavailable'
fi

printf '\nPackage and USE resolution\n'
if command -v emerge >/dev/null 2>&1 && [ "$REGISTERED_REPO_VALID" = true ]; then
	if command -v qlist >/dev/null 2>&1 &&
		qlist -IC dev-util/prismdrake-dev-env >/dev/null 2>&1; then
		pass 'development metapackage is installed'
	else
		fail 'development metapackage is not installed'
	fi
	if command -v equery >/dev/null 2>&1; then
		printf 'Installed development metapackage USE state:\n'
		equery uses dev-util/prismdrake-dev-env || fail 'could not inspect installed metapackage USE state'
	else
		fail 'equery is unavailable for installed USE inspection'
	fi
	if emerge --pretend --verbose --tree dev-util/prismdrake-dev-env; then
		pass 'default development metapackage resolves'
	else
		fail 'default development metapackage does not resolve'
	fi

	if [ -f "$PORTAGE_REPO/x11-misc/prismdrake/prismdrake-9999.ebuild" ]; then
		if emerge --pretend --verbose --tree x11-misc/prismdrake; then
			pass 'live Prismdrake product ebuild resolves'
		else
			fail 'live Prismdrake product ebuild does not resolve'
		fi
	else
		printf 'INFO: product ebuild is deferred until build and install targets are accepted\n'
	fi

	for MATRIX_ENTRY in 'no-qt6|-qt6' 'clang|clang' \
		'implementation-deps|implementation-deps' 'visual-tests|visual-tests'; do
		MATRIX_LABEL=${MATRIX_ENTRY%%|*}
		MATRIX_USE=${MATRIX_ENTRY#*|}
		if USE=$MATRIX_USE emerge --pretend --verbose --changed-use --deep --noreplace --tree \
			dev-util/prismdrake-dev-env; then
			pass "$MATRIX_LABEL development metapackage combination resolves"
		else
			fail "$MATRIX_LABEL development metapackage combination does not resolve"
		fi
	done
else
	warn 'Portage resolution skipped because emerge or prismdrake-local is unavailable'
fi

printf '\nResult: %d failure(s), %d warning(s)\n' "$FAILURES" "$WARNINGS"
[ "$FAILURES" -eq 0 ]
