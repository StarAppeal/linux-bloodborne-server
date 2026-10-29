#!/usr/bin/env bash
# Block a user.
#
# If the target was a friend, the friendship is simultaneously severed and
# both sides receive a FriendLost notification.
# Blocked users cannot send you friend requests.
#
# Usage:
#   block-add.sh <my_npid> <my_password> <target_npid>
#
# Example:
#   block-add.sh Shadow 1234 Stephen
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

if (( $# < 3 )); then
    echo "Usage: block-add.sh <my_npid> <my_password> <target_npid>" >&2
    exit 1
fi
echo "[block-add] $1 ${*:3}"
echo

sample block-add "$@"
status=$?

echo
echo "Exit code: $status"
exit $status
