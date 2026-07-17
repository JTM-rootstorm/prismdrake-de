#!/bin/sh
# Read-only checks for the Prismdrake Gentoo reference VM.

set -u

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
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

printf '\nResources\n'
uname -r 2>/dev/null || true
awk '/MemTotal:/ { printf "memory: %.1f GiB\n", $2 / 1048576 }' /proc/meminfo 2>/dev/null || true
df -h / 2>/dev/null || warn 'could not inspect root filesystem capacity'

printf '\nShared artifact mount\n'
if [ ! -d "$SHARED_PATH" ]; then
	fail "shared path does not exist: $SHARED_PATH"
elif command -v findmnt >/dev/null 2>&1; then
	MOUNT_TYPE=$(findmnt -T "$SHARED_PATH" -n -o FSTYPE 2>/dev/null || true)
	MOUNT_OPTIONS=$(findmnt -T "$SHARED_PATH" -n -o OPTIONS 2>/dev/null || true)
	case "$MOUNT_TYPE" in
		virtiofs) pass 'shared path is on virtiofs' ;;
		'') fail 'no mount contains the shared path' ;;
		*) fail 'shared path is not on virtiofs' ;;
	esac
	case ",$MOUNT_OPTIONS," in
		*,rw,*) pass 'shared artifact mount is read-write' ;;
		*) warn 'shared artifact mount is not reported read-write' ;;
	esac
	OWNER_MODE=$(stat -c '%u:%g %a' "$SHARED_PATH" 2>/dev/null || true)
	if [ -n "$OWNER_MODE" ]; then
		printf 'INFO: shared artifact ownership and mode: %s\n' "$OWNER_MODE"
		MODE=${OWNER_MODE##* }
		OTHER_DIGIT=$((MODE % 10))
		if [ $((OTHER_DIGIT & 2)) -ne 0 ]; then
			fail 'shared artifact mount is world-writable'
		else
			pass 'shared artifact mount is not world-writable'
		fi
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
	if command -v findmnt >/dev/null 2>&1 &&
		[ "$(findmnt -T "$WORKSPACE" -n -o FSTYPE 2>/dev/null)" = virtiofs ]; then
		pass 'workspace itself is on virtiofs'
	else
		fail 'workspace itself is not on virtiofs'
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
	EDIT_OTHER_DIGIT=$((EDIT_MODE % 10))
	if [ $((EDIT_OTHER_DIGIT & 2)) -ne 0 ]; then
		fail "source path is world-writable: $EDIT_PATH"
	fi
	EDIT_USER=$(getent passwd "$EDIT_OWNER" 2>/dev/null | cut -d: -f1)
	if [ -z "$EDIT_USER" ]; then
		fail "source owner UID $EDIT_OWNER has no guest account"
	elif [ "$EDIT_OWNER" -eq 0 ]; then
		fail "source path is root-owned instead of editable by an ordinary user: $EDIT_PATH"
	elif [ "$(id -u)" -eq 0 ] && command -v runuser >/dev/null 2>&1; then
		if runuser -u "$EDIT_USER" -- test -r "$EDIT_PATH" &&
			runuser -u "$EDIT_USER" -- test -w "$EDIT_PATH"; then
			pass "$EDIT_USER can read and write $EDIT_PATH without world-write"
		else
			fail "$EDIT_USER cannot read and write $EDIT_PATH"
		fi
	else
		warn "ordinary-user access was not checked for $EDIT_PATH; run verification as root"
	fi
done

printf '\nLocal Portage repository\n'
if [ -r "$PORTAGE_REPO/profiles/repo_name" ] &&
	[ "$(cat "$PORTAGE_REPO/profiles/repo_name")" = prismdrake-local ]; then
	pass 'tracked prismdrake-local metadata is present'
else
	fail "tracked prismdrake-local metadata is missing from $WORKSPACE"
fi

REGISTERED_REPO=
if command -v portageq >/dev/null 2>&1; then
	REGISTERED_REPO=$(portageq get_repo_path / prismdrake-local 2>/dev/null || true)
	if [ -n "$REGISTERED_REPO" ]; then
		pass 'Portage recognizes prismdrake-local'
		if [ -d "$PORTAGE_REPO" ] &&
			[ "$(readlink -f "$REGISTERED_REPO" 2>/dev/null)" != "$(readlink -f "$PORTAGE_REPO" 2>/dev/null)" ]; then
			fail "registered prismdrake-local does not point at $WORKSPACE"
		fi
	else
		fail 'Portage does not recognize prismdrake-local'
	fi
else
	fail 'portageq is unavailable'
fi

printf '\nRepository QA\n'
if command -v pkgcheck >/dev/null 2>&1; then
	if pkgcheck scan "$PORTAGE_REPO"; then
		pass 'pkgcheck scan passed'
	else
		fail 'pkgcheck scan reported repository findings'
	fi
else
	fail 'pkgcheck is unavailable; enable prismdrake-dev-env[portage-qa]'
fi

printf '\nTool availability\n'
for TOOL in gcc cmake ninja git gdb strace valgrind lsof Xvfb Xephyr openbox dbus-run-session qmake6; do
	if command -v "$TOOL" >/dev/null 2>&1; then
		printf 'PASS: %-18s available\n' "$TOOL"
	else
		printf 'WARN: %-18s unavailable\n' "$TOOL"
		WARNINGS=$((WARNINGS + 1))
	fi
done

printf '\nPackage and USE resolution\n'
if command -v emerge >/dev/null 2>&1 && [ -n "$REGISTERED_REPO" ]; then
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
