#!/bin/sh
# Guarded bootstrap for the Prismdrake Gentoo reference VM.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
# shellcheck source=tools/gentoo/vm-checks.sh
. "$SCRIPT_DIR/vm-checks.sh"
DEFAULT_WORKSPACE=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd -P)
WORKSPACE=${PRISMDRAKE_WORKSPACE:-$DEFAULT_WORKSPACE}
SHARED_PATH=${PRISMDRAKE_SHARED_PATH:-/mnt/shared}
APPLY=false
SYNC=false
USE_BINPKGS=false
BINPKG_OPTION=

usage() {
	cat <<'EOF'
Usage: bootstrap-vm.sh [--apply] [--sync] [--use-binpkgs] [--workspace PATH]
                       [--shared-path PATH]

Without --apply, print the detected state and planned guest changes. With
--apply, register prismdrake-local, write project-specific package USE policy,
run a Portage pretend, and then ask before emerging prismdrake-dev-env.

--sync is valid only with --apply and synchronizes the canonical Gentoo
repository before resolution. --use-binpkgs asks Portage to use the guest's
configured binary package repositories when package USE settings match; review
the reported provenance and every source fallback before approving the merge.
Environment overrides: PRISMDRAKE_WORKSPACE and PRISMDRAKE_SHARED_PATH.
EOF
}

die() {
	printf 'ERROR: %s\n' "$1" >&2
	exit 1
}

while [ "$#" -gt 0 ]; do
	case "$1" in
		--apply)
			APPLY=true
			shift
			;;
		--sync)
			SYNC=true
			shift
			;;
		--use-binpkgs)
			USE_BINPKGS=true
			BINPKG_OPTION=--getbinpkg
			shift
			;;
		--workspace)
			[ "$#" -ge 2 ] || die 'missing value for --workspace'
			WORKSPACE=$2
			shift 2
			;;
		--shared-path)
			[ "$#" -ge 2 ] || die 'missing value for --shared-path'
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
'*) die 'workspace and shared paths must not contain newlines' ;;
esac

[ "$SYNC" = false ] || [ "$APPLY" = true ] || die '--sync requires --apply'
[ -r /etc/gentoo-release ] || die 'this helper must run inside a Gentoo guest'
[ "$(uname -m)" = x86_64 ] || die 'PD1 expects an x86_64 Gentoo guest'
[ -d "$WORKSPACE" ] || die "workspace does not exist: $WORKSPACE"
WORKSPACE=$(CDPATH='' cd -- "$WORKSPACE" && pwd -P)
PORTAGE_REPO=$WORKSPACE/packaging/gentoo/repository
REPOSITORY_NAME=$(cat "$PORTAGE_REPO/profiles/repo_name" 2>/dev/null || true)
if ! REPO_NAME_ERROR=$(prismdrake_validate_repo_name "$REPOSITORY_NAME"); then
	die "$REPO_NAME_ERROR"
fi

[ -d "$SHARED_PATH" ] || die "shared path does not exist: $SHARED_PATH"
SHARED_PATH=$(CDPATH='' cd -- "$SHARED_PATH" && pwd -P)
case "$WORKSPACE/" in
	"$SHARED_PATH/"*) ;;
	*) die 'workspace must be contained by the shared path' ;;
esac
if command -v findmnt >/dev/null 2>&1; then
	for MOUNT_PATH in "$SHARED_PATH" "$WORKSPACE"; do
		MOUNT_TYPE=$(findmnt -T "$MOUNT_PATH" -n -o FSTYPE 2>/dev/null || true)
		MOUNT_OPTIONS=$(findmnt -T "$MOUNT_PATH" -n -o OPTIONS 2>/dev/null || true)
		if ! MOUNT_ERROR=$(prismdrake_validate_mount_state "$MOUNT_TYPE" "$MOUNT_OPTIONS"); then
			die "$MOUNT_PATH: $MOUNT_ERROR"
		fi
	done
else
	die 'findmnt is required to verify the host-fed share'
fi

MODE=$(stat -c '%a' "$SHARED_PATH" 2>/dev/null || true)
if ! MODE_ERROR=$(prismdrake_validate_mode "$MODE" "$SHARED_PATH"); then
	die "$MODE_ERROR"
fi
for SAFE_PATH in "$WORKSPACE" "$PORTAGE_REPO"; do
	SAFE_MODE=$(stat -c '%a' "$SAFE_PATH" 2>/dev/null || true)
	if ! MODE_ERROR=$(prismdrake_validate_mode "$SAFE_MODE" "$SAFE_PATH"); then
		die "$MODE_ERROR"
	fi
done
WORKSPACE_OWNER=$(stat -c '%u' "$WORKSPACE" 2>/dev/null || true)
WORKSPACE_USER=$(getent passwd "$WORKSPACE_OWNER" 2>/dev/null | cut -d: -f1)
command -v runuser >/dev/null 2>&1 || die 'runuser is required for unprivileged repository QA'
OWNER_READABLE=false
OWNER_WRITABLE=false
if [ -n "$WORKSPACE_USER" ]; then
	runuser -u "$WORKSPACE_USER" -- test -r "$PORTAGE_REPO" && OWNER_READABLE=true
	runuser -u "$WORKSPACE_USER" -- test -w "$PORTAGE_REPO" && OWNER_WRITABLE=true
fi
if ! OWNER_ERROR=$(prismdrake_validate_owner_state \
	"$WORKSPACE_OWNER" "$WORKSPACE_USER" "$OWNER_READABLE" "$OWNER_WRITABLE"); then
	die "$OWNER_ERROR"
fi

AVAILABLE_KIB=$(df -Pk / 2>/dev/null | awk 'NR == 2 { print $4 }' || true)
MEMORY_KIB=$(awk '/MemTotal:/ { print $2 }' /proc/meminfo 2>/dev/null || true)
if ! RESOURCE_ERROR=$(prismdrake_validate_resources "$AVAILABLE_KIB" "$MEMORY_KIB"); then
	die "$RESOURCE_ERROR"
fi

REPOS_FILE=/etc/portage/repos.conf/prismdrake-local.conf
USE_FILE=/etc/portage/package.use/prismdrake-dev
KEYWORDS_FILE=/etc/portage/package.accept_keywords/prismdrake-dev
BACKUP_DIR=/etc/portage/prismdrake-backups
LAYER_A_PACKAGES='app-eselect/eselect-repository dev-util/pkgcheck dev-util/pkgdev app-portage/gentoolkit app-portage/portage-utils dev-vcs/git'

printf 'Prismdrake Gentoo bootstrap\n'
printf '  mode: %s\n' "$(if [ "$APPLY" = true ]; then printf apply; else printf plan-only; fi)"
printf '  workspace: %s\n' "$WORKSPACE"
printf '  workspace owner: %s (UID %s)\n' "$WORKSPACE_USER" "$WORKSPACE_OWNER"
printf '  shared path: %s\n' "$SHARED_PATH"
printf '  binary packages: %s\n' "$(if [ "$USE_BINPKGS" = true ]; then printf enabled; else printf disabled; fi)"
printf '  root free space: %s GiB\n' "$(awk -v value="$AVAILABLE_KIB" 'BEGIN { printf "%.1f", value / 1048576 }')"
printf '  guest memory: %s GiB\n' "$(awk -v value="$MEMORY_KIB" 'BEGIN { printf "%.1f", value / 1048576 }')"
printf '  release: %s\n' "$(cat /etc/gentoo-release)"
printf '  kernel: %s\n' "$(uname -r)"
printf '  init: %s\n' "$(readlink /proc/1/exe 2>/dev/null || printf unknown)"
printf '  Portage: %s\n' "$(emerge --version 2>/dev/null | sed -n '1p' || printf unavailable)"
printf '  compiler: %s\n' "$(gcc --version 2>/dev/null | sed -n '1p' || printf unavailable)"

if [ -e "$REPOS_FILE" ]; then
	printf '  repository config: present; content will be reconciled\n'
else
	printf '  repository config: missing; file will be created\n'
fi
if [ -e "$USE_FILE" ]; then
	printf '  package USE config: present; content will be reconciled\n'
else
	printf '  package USE config: missing; file will be created\n'
fi
if [ -e "$KEYWORDS_FILE" ]; then
	printf '  package keywords config: present; content will be reconciled\n'
else
	printf '  package keywords config: missing; file will be created\n'
fi

printf '\nPlanned guest-only actions:\n'
printf '  1. pretend and ask before installing the canonical Layer A QA tools\n'
printf '  2. write %s\n' "$REPOS_FILE"
printf '  3. write %s\n' "$USE_FILE"
printf '  4. write %s\n' "$KEYWORDS_FILE"
[ "$SYNC" = false ] || printf '  5. synchronize only the canonical gentoo repository\n'
printf '  %s. generate manifests and run pkgcheck before package resolution\n' \
	"$(if [ "$SYNC" = true ]; then printf 6; else printf 5; fi)"
printf '  %s. pretend the default, -qt6, clang, implementation-deps, and visual-tests combinations\n' \
	"$(if [ "$SYNC" = true ]; then printf 7; else printf 6; fi)"
printf '  %s. ask before emerging dev-util/prismdrake-dev-env\n' \
	"$(if [ "$SYNC" = true ]; then printf 8; else printf 7; fi)"
printf 'No make.conf, profile, global keyword, depclean, or host virtualization change is made.\n'

if [ "$APPLY" = false ]; then
	printf '\nPlan only. Re-run with --apply after reviewing the detected state.\n'
	"$SCRIPT_DIR/verify-vm.sh" --workspace "$WORKSPACE" --shared-path "$SHARED_PATH" || true
	exit 0
fi

[ "$(id -u)" -eq 0 ] || die '--apply must run as root inside the guest'

command -v emerge >/dev/null 2>&1 || die 'emerge is unavailable'
if [ "$SYNC" = true ]; then
	command -v emaint >/dev/null 2>&1 || die 'emaint is unavailable'
	emaint sync -r gentoo
fi

# The local overlay cannot validate itself before pkgcheck and pkgdev exist.
# Bootstrap those tools exclusively from the already configured Gentoo
# repository, with the same reviewed pretend-before-ask posture as later
# layers.
# BINPKG_OPTION is either empty or the single constant --getbinpkg.
# shellcheck disable=SC2086
emerge $BINPKG_OPTION --pretend --verbose --noreplace --tree $LAYER_A_PACKAGES
# shellcheck disable=SC2086
emerge $BINPKG_OPTION --ask --verbose --noreplace $LAYER_A_PACKAGES
command -v pkgdev >/dev/null 2>&1 || die 'pkgdev is unavailable after Layer A installation'
command -v pkgcheck >/dev/null 2>&1 || die 'pkgcheck is unavailable after Layer A installation'

REPOS_CONTENT="[prismdrake-local]
location = $PORTAGE_REPO
priority = 1000
auto-sync = no"

USE_CONTENT="# Prismdrake PD1 reference-VM policy; keep changes package-local.
dev-util/prismdrake-dev-env portage-qa debug-tools x11 qt6 clang implementation-deps -visual-tests
x11-base/xorg-server xephyr xvfb
x11-wm/openbox session xdg
x11-libs/libxkbcommon X tools
x11-libs/pango X
x11-libs/cairo X
app-accessibility/at-spi2-core X
media-libs/freetype harfbuzz
media-libs/libglvnd X
media-libs/mesa X llvm
dev-qt/qtbase:6 X accessibility dbus gui opengl -gtk
dev-qt/qtdeclarative:6 accessibility opengl qmlls
dev-qt/qttools:6 opengl qdbus qtdiag qtplugininfo"

KEYWORDS_CONTENT="# Project-owned packages use package-local keyword exceptions only.
# The development metapackage is testing-only; the live product ebuild is intentionally unkeyworded.
dev-util/prismdrake-dev-env ~amd64
x11-misc/prismdrake **"

prismdrake_install_project_file "$REPOS_FILE" "$REPOS_CONTENT" "$BACKUP_DIR"
prismdrake_install_project_file "$USE_FILE" "$USE_CONTENT" "$BACKUP_DIR"
prismdrake_install_project_file "$KEYWORDS_FILE" "$KEYWORDS_CONTENT" "$BACKUP_DIR"

REGISTERED_REPO=$(portageq get_repo_path / prismdrake-local 2>/dev/null || true)
EXPECTED_REPO=$(readlink -f "$PORTAGE_REPO" 2>/dev/null || true)
RESOLVED_REPO=$(readlink -f "$REGISTERED_REPO" 2>/dev/null || true)
if ! REPO_ERROR=$(prismdrake_validate_registered_repo "$EXPECTED_REPO" "$RESOLVED_REPO"); then
	die "$REPO_ERROR after registration"
fi
eselect repository list -i | grep -q 'prismdrake-local' ||
	die 'eselect does not report prismdrake-local as installed'
printf 'Verified live repository path: %s\n' "$REGISTERED_REPO"

QA_REPO=$(mktemp -d /tmp/prismdrake-repository-qa.XXXXXX)
PKGCHECK_CACHE=$(mktemp -d /tmp/prismdrake-pkgcheck.XXXXXX)
WORKSPACE_GROUP=$(stat -c '%g' "$WORKSPACE")
chown "$WORKSPACE_OWNER:$WORKSPACE_GROUP" "$QA_REPO" "$PKGCHECK_CACHE"
trap 'rm -rf "$QA_REPO" "$PKGCHECK_CACHE"' EXIT HUP INT TERM
runuser -u "$WORKSPACE_USER" -- cp -a "$PORTAGE_REPO/." "$QA_REPO/"
# The single-quoted script expands $1 only in the unprivileged child shell.
# shellcheck disable=SC2016
runuser -u "$WORKSPACE_USER" -- sh -c \
	'cd -- "$1" && exec pkgdev manifest' sh "$QA_REPO"
runuser -u "$WORKSPACE_USER" -- pkgcheck scan \
	--cache-dir "$PKGCHECK_CACHE" "$QA_REPO"
if find "$QA_REPO" -type f -name Manifest -print -quit | grep -q .; then
	die 'QA generated a Manifest; review and commit it before rerunning bootstrap'
fi
rm -rf "$QA_REPO" "$PKGCHECK_CACHE"
trap - EXIT HUP INT TERM

pretend_combination() {
	LABEL=$1
	USE_OVERRIDE=$2
	printf '\nResolving development metapackage combination: %s\n' "$LABEL"
	if [ -n "$USE_OVERRIDE" ]; then
		# shellcheck disable=SC2086
		USE=$USE_OVERRIDE emerge $BINPKG_OPTION --pretend --verbose --changed-use --deep --noreplace --tree \
			dev-util/prismdrake-dev-env
	else
		# shellcheck disable=SC2086
		emerge $BINPKG_OPTION --pretend --verbose --changed-use --deep --noreplace --tree \
			dev-util/prismdrake-dev-env
	fi
}

pretend_combination default ''
pretend_combination no-qt6 '-qt6'
pretend_combination clang 'clang'
pretend_combination implementation-deps 'implementation-deps'
pretend_combination visual-tests 'visual-tests'
# shellcheck disable=SC2086
emerge $BINPKG_OPTION --ask --verbose --changed-use --deep --noreplace \
	dev-util/prismdrake-dev-env

"$SCRIPT_DIR/verify-vm.sh" --workspace "$WORKSPACE" --shared-path "$SHARED_PATH"
