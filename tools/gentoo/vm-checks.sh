#!/bin/sh
# Shared validation and file-reconciliation helpers for Gentoo VM tooling.

prismdrake_validation_error() {
	printf '%s\n' "$1"
	return 1
}

prismdrake_is_unsigned_integer() {
	case "$1" in
		''|*[!0-9]*) return 1 ;;
		*) return 0 ;;
	esac
}

prismdrake_validate_mount_state() {
	MOUNT_KIND=$1
	MOUNT_FLAGS=$2

	case "$MOUNT_KIND" in
		virtiofs) ;;
		'') prismdrake_validation_error 'no mount contains the requested path'; return ;;
		*) prismdrake_validation_error 'requested path is not on virtiofs'; return ;;
	esac

	case ",$MOUNT_FLAGS," in
		*,rw,*) return 0 ;;
		*) prismdrake_validation_error 'virtiofs mount is not reported read-write' ;;
	esac
}

prismdrake_validate_mode() {
	PATH_MODE=$1
	PATH_LABEL=$2

	prismdrake_is_unsigned_integer "$PATH_MODE" || {
		prismdrake_validation_error "could not determine permissions for $PATH_LABEL"
		return
	}
	OTHER_DIGIT=$((PATH_MODE % 10))
	[ $((OTHER_DIGIT & 2)) -eq 0 ] ||
		prismdrake_validation_error "refusing world-writable path: $PATH_LABEL"
}

prismdrake_validate_owner_state() {
	OWNER_UID=$1
	OWNER_USER=$2
	OWNER_READABLE=$3
	OWNER_WRITABLE=$4

	prismdrake_is_unsigned_integer "$OWNER_UID" || {
		prismdrake_validation_error 'could not determine workspace owner UID'
		return
	}
	[ "$OWNER_UID" -ne 0 ] || {
		prismdrake_validation_error 'workspace must be owned by an ordinary guest user'
		return
	}
	[ -n "$OWNER_USER" ] || {
		prismdrake_validation_error "workspace owner UID $OWNER_UID has no guest account"
		return
	}
	[ "$OWNER_READABLE" = true ] || {
		prismdrake_validation_error "workspace owner $OWNER_USER cannot read the local repository"
		return
	}
	[ "$OWNER_WRITABLE" = true ] ||
		prismdrake_validation_error "workspace owner $OWNER_USER cannot write the local repository"
}

prismdrake_validate_resources() {
	FREE_KIB=$1
	TOTAL_MEMORY_KIB=$2

	prismdrake_is_unsigned_integer "$FREE_KIB" || {
		prismdrake_validation_error 'could not determine free space on the guest root filesystem'
		return
	}
	prismdrake_is_unsigned_integer "$TOTAL_MEMORY_KIB" || {
		prismdrake_validation_error 'could not determine guest memory'
		return
	}
	[ "$FREE_KIB" -ge 5242880 ] || {
		prismdrake_validation_error 'less than 5 GiB is free on the guest root filesystem'
		return
	}
	[ "$TOTAL_MEMORY_KIB" -ge 4194304 ] ||
		prismdrake_validation_error 'less than 4 GiB of memory is available to the guest'
}

prismdrake_validate_registered_repo() {
	EXPECTED_REPO=$1
	REGISTERED_REPO=$2

	[ -n "$REGISTERED_REPO" ] || {
		prismdrake_validation_error 'Portage does not recognize prismdrake-local'
		return
	}
	[ -n "$EXPECTED_REPO" ] || {
		prismdrake_validation_error 'could not resolve the tracked prismdrake-local path'
		return
	}
	[ "$REGISTERED_REPO" = "$EXPECTED_REPO" ] ||
		prismdrake_validation_error 'Portage resolved prismdrake-local to a stale or unexpected path'
}

prismdrake_validate_repo_name() {
	REPOSITORY_NAME=$1

	[ "$REPOSITORY_NAME" = prismdrake-local ] ||
		prismdrake_validation_error 'local repository metadata has a missing or unexpected repo_name'
}

prismdrake_install_project_file() {
	PROJECT_TARGET=$1
	PROJECT_CONTENT=$2
	PROJECT_BACKUP_DIR=$3
	PROJECT_TARGET_DIR=$(dirname -- "$PROJECT_TARGET")

	install -d -m 0755 "$PROJECT_TARGET_DIR"
	PROJECT_TEMP=$(mktemp "$PROJECT_TARGET.tmp.XXXXXX")
	trap 'rm -f "$PROJECT_TEMP"' EXIT HUP INT TERM
	printf '%s\n' "$PROJECT_CONTENT" >"$PROJECT_TEMP"
	chmod 0644 "$PROJECT_TEMP"

	PROJECT_MODE=
	if [ -e "$PROJECT_TARGET" ]; then
		PROJECT_MODE=$(stat -c '%a' "$PROJECT_TARGET" 2>/dev/null || true)
	fi
	if [ -e "$PROJECT_TARGET" ] && cmp -s "$PROJECT_TEMP" "$PROJECT_TARGET" &&
		[ "$PROJECT_MODE" = 644 ]; then
		rm -f "$PROJECT_TEMP"
		trap - EXIT HUP INT TERM
		printf 'Unchanged: %s\n' "$PROJECT_TARGET"
		return
	fi

	if [ -e "$PROJECT_TARGET" ]; then
		install -d -m 0700 "$PROJECT_BACKUP_DIR"
		PROJECT_BACKUP=$(mktemp "$PROJECT_BACKUP_DIR/$(basename -- "$PROJECT_TARGET").$(date -u +%Y%m%dT%H%M%SZ).XXXXXX")
		cp -p -- "$PROJECT_TARGET" "$PROJECT_BACKUP"
		printf 'Backed up: %s\n' "$PROJECT_BACKUP"
	fi
	mv -f -- "$PROJECT_TEMP" "$PROJECT_TARGET"
	trap - EXIT HUP INT TERM
	printf 'Updated: %s\n' "$PROJECT_TARGET"
}
