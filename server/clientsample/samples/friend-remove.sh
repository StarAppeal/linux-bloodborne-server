#!/usr/bin/env bash
# Remove a friend or cancel a pending friend request.
#
# Both parties receive a FriendLost notification.
#
# Usage:
#   friend-remove.sh <my_npid> <my_password> <target_npid>
#
# Example:
#   friend-remove.sh Shadow 1234 Stephen
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

if (( $# < 3 )); then
    echo "Usage: friend-remove.sh <my_npid> <my_password> <target_npid>" >&2
    exit 1
fi
echo "[friend-remove] $1 ${*:3}"
echo

sample friend-remove "$@"
status=$?

echo
echo "Exit code: $status"
exit $status
