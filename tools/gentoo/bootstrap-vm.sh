#!/bin/sh
# Guarded bootstrap for the Prismdrake Gentoo reference VM.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
DEFAULT_WORKSPACE=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd -P)
WORKSPACE=${PRISMDRAKE_WORKSPACE:-$DEFAULT_WORKSPACE}
SHARED_PATH=${PRISMDRAKE_SHARED_PATH:-/mnt/shared}
APPLY=false
SYNC=false

usage() {
	cat <<'EOF'
Usage: bootstrap-vm.sh [--apply] [--sync] [--workspace PATH] [--shared-path PATH]

Without --apply, print the detected state and planned guest changes. With
--apply, register prismdrake-local, write project-specific package USE policy,
run a Portage pretend, and then ask before emerging prismdrake-dev-env.

--sync is valid only with --apply and synchronizes the canonical Gentoo
repository before resolution. Environment overrides: PRISMDRAKE_WORKSPACE and
PRISMDRAKE_SHARED_PATH.
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
[ -r "$PORTAGE_REPO/profiles/repo_name" ] || die 'local repository metadata is missing'
[ "$(cat "$PORTAGE_REPO/profiles/repo_name")" = prismdrake-local ] || die 'unexpected local repository name'

[ -d "$SHARED_PATH" ] || die "shared path does not exist: $SHARED_PATH"
SHARED_PATH=$(CDPATH='' cd -- "$SHARED_PATH" && pwd -P)
case "$WORKSPACE/" in
	"$SHARED_PATH/"*) ;;
	*) die 'workspace must be contained by the shared path' ;;
esac
if command -v findmnt >/dev/null 2>&1; then
	[ "$(findmnt -T "$SHARED_PATH" -n -o FSTYPE 2>/dev/null)" = virtiofs ] ||
		die 'shared path is not on virtiofs'
	[ "$(findmnt -T "$WORKSPACE" -n -o FSTYPE 2>/dev/null)" = virtiofs ] ||
		die 'workspace itself is not on virtiofs'
else
	die 'findmnt is required to verify the host-fed share'
fi

MODE=$(stat -c '%a' "$SHARED_PATH")
OTHER_DIGIT=$((MODE % 10))
[ $((OTHER_DIGIT & 2)) -eq 0 ] || die 'refusing a world-writable shared path'

AVAILABLE_KIB=$(df -Pk / | awk 'NR == 2 { print $4 }')
[ "$AVAILABLE_KIB" -ge 5242880 ] || die 'less than 5 GiB is free on the guest root filesystem'
MEMORY_KIB=$(awk '/MemTotal:/ { print $2 }' /proc/meminfo)
[ "$MEMORY_KIB" -ge 4194304 ] || die 'less than 4 GiB of memory is available to the guest'

REPOS_FILE=/etc/portage/repos.conf/prismdrake-local.conf
USE_FILE=/etc/portage/package.use/prismdrake-dev
KEYWORDS_FILE=/etc/portage/package.accept_keywords/prismdrake-dev
LAYER_A_PACKAGES='app-eselect/eselect-repository dev-util/pkgcheck dev-util/pkgdev app-portage/gentoolkit app-portage/portage-utils dev-vcs/git'

printf 'Prismdrake Gentoo bootstrap\n'
printf '  mode: %s\n' "$(if [ "$APPLY" = true ]; then printf apply; else printf plan-only; fi)"
printf '  workspace: %s\n' "$WORKSPACE"
printf '  shared path: %s\n' "$SHARED_PATH"
printf '  root free space: %s GiB\n' "$(awk -v value="$AVAILABLE_KIB" 'BEGIN { printf "%.1f", value / 1048576 }')"
printf '  guest memory: %s GiB\n' "$(awk -v value="$MEMORY_KIB" 'BEGIN { printf "%.1f", value / 1048576 }')"

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
printf '  6. generate manifests and run pkgcheck before package resolution\n'
printf '  7. pretend the default, -qt6, clang, implementation-deps, and visual-tests combinations\n'
printf '  8. ask before emerging dev-util/prismdrake-dev-env\n'
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
# shellcheck disable=SC2086
emerge --pretend --verbose --noreplace --tree $LAYER_A_PACKAGES
# shellcheck disable=SC2086
emerge --ask --verbose --noreplace $LAYER_A_PACKAGES
command -v pkgdev >/dev/null 2>&1 || die 'pkgdev is unavailable after Layer A installation'
command -v pkgcheck >/dev/null 2>&1 || die 'pkgcheck is unavailable after Layer A installation'

install_project_file() {
	TARGET=$1
	CONTENT=$2
	TARGET_DIR=$(dirname -- "$TARGET")
	install -d -m 0755 "$TARGET_DIR"
	TEMP=$(mktemp "$TARGET.tmp.XXXXXX")
	trap 'rm -f "$TEMP"' EXIT HUP INT TERM
	printf '%s\n' "$CONTENT" >"$TEMP"
	chmod 0644 "$TEMP"
	if [ -e "$TARGET" ] && cmp -s "$TEMP" "$TARGET"; then
		rm -f "$TEMP"
		trap - EXIT HUP INT TERM
		printf 'Unchanged: %s\n' "$TARGET"
		return
	fi
	if [ -e "$TARGET" ]; then
		BACKUP=$TARGET.prismdrake-backup.$(date -u +%Y%m%dT%H%M%SZ)
		cp -p -- "$TARGET" "$BACKUP"
		printf 'Backed up: %s\n' "$BACKUP"
	fi
	mv -f -- "$TEMP" "$TARGET"
	trap - EXIT HUP INT TERM
	printf 'Updated: %s\n' "$TARGET"
}

REPOS_CONTENT="[prismdrake-local]
location = $PORTAGE_REPO
priority = 1000
auto-sync = no"

USE_CONTENT="# Prismdrake PD1 reference-VM policy; keep changes package-local.
dev-util/prismdrake-dev-env portage-qa debug-tools x11 qt6 -clang -implementation-deps -visual-tests
x11-base/xorg-server xephyr xvfb
x11-wm/openbox session xdg
x11-libs/libxkbcommon X tools
app-accessibility/at-spi2-core X
media-libs/mesa X llvm
dev-qt/qtbase:6 X accessibility dbus gui opengl -gtk
dev-qt/qtdeclarative:6 accessibility opengl qmlls
dev-qt/qttools:6 qdbus qtdiag qtplugininfo"

KEYWORDS_CONTENT="# The project-owned development metapackage is intentionally testing-only.
dev-util/prismdrake-dev-env ~amd64"

install_project_file "$REPOS_FILE" "$REPOS_CONTENT"
install_project_file "$USE_FILE" "$USE_CONTENT"
install_project_file "$KEYWORDS_FILE" "$KEYWORDS_CONTENT"

pkgdev manifest "$PORTAGE_REPO"
pkgcheck scan "$PORTAGE_REPO"

pretend_combination() {
	LABEL=$1
	USE_OVERRIDE=$2
	printf '\nResolving development metapackage combination: %s\n' "$LABEL"
	if [ -n "$USE_OVERRIDE" ]; then
		USE=$USE_OVERRIDE emerge --pretend --verbose --changed-use --deep --noreplace --tree \
			dev-util/prismdrake-dev-env
	else
		emerge --pretend --verbose --changed-use --deep --noreplace --tree \
			dev-util/prismdrake-dev-env
	fi
}

pretend_combination default ''
pretend_combination no-qt6 '-qt6'
pretend_combination clang 'clang'
pretend_combination implementation-deps 'implementation-deps'
pretend_combination visual-tests 'visual-tests'
emerge --ask --verbose --changed-use --deep --noreplace dev-util/prismdrake-dev-env

"$SCRIPT_DIR/verify-vm.sh" --workspace "$WORKSPACE" --shared-path "$SHARED_PATH"
