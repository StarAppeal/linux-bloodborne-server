#!/usr/bin/env bash
# Unblock a previously blocked user.
#
# Removing a block does not automatically restore the friendship — the friend
# request process must be started again from scratch.
#
# Usage:
#   block-remove.sh <my_npid> <my_password> <target_npid>
#
# Example:
#   block-remove.sh Shadow 1234 Stephen
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

if (( $# < 3 )); then
    echo "Usage: block-remove.sh <my_npid> <my_password> <target_npid>" >&2
    exit 1
fi
echo "[block-remove] $1 ${*:3}"
echo

sample block-remove "$@"
status=$?

echo
echo "Exit code: $status"
exit $status
