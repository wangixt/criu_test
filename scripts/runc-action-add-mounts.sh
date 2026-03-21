#!/bin/bash
#
# runc-action-add-mounts.sh — CRIU action-script wrapper for criu-move-mount.
#
# Usage:
#   criu restore --action-script /path/to/runc-action-add-mounts.sh
#
# This is a thin wrapper that locates and executes the criu-move-mount binary.
# All logic (phase filtering, mount-rule parsing, namespace operations) is
# handled by criu-move-mount itself.
#
# The binary is located in this order:
#   1. $CRIU_MOVE_MOUNT_BIN  (explicit override)
#   2. Same directory as this script
#   3. /usr/local/bin/criu-move-mount
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
bin="${CRIU_MOVE_MOUNT_BIN:-}"

if [ -z "$bin" ]; then
	for p in "$SCRIPT_DIR/criu-move-mount" /usr/local/bin/criu-move-mount; do
		[ -x "$p" ] && { bin="$p"; break; }
	done
fi

[ -x "$bin" ] || { echo "add-mounts: criu-move-mount not found" >&2; exit 1; }

exec "$bin"
