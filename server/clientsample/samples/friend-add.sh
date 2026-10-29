#!/usr/bin/env bash
# Send a friend request to another user, or accept a pending incoming request.
#
# If the target has already sent you a request, this command completes the
# mutual friendship and both sides receive a FriendNew notification.
# If no request exists yet, a FriendQuery notification is sent to the target.
#
# Usage:
#   friend-add.sh <my_npid> <my_password> <target_npid>
#
# Examples:
#   friend-add.sh Shadow 1234 Stephen     <- Shadow sends Stephen a request
#   friend-add.sh Stephen 12345   Shadow   <- Stephen accepts (mutual friendship formed)
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

if (( $# < 3 )); then
    echo "Usage: friend-add.sh <my_npid> <my_password> <target_npid>" >&2
    exit 1
fi
echo "[friend-add] $1 ${*:3}"
echo

sample friend-add "$@"
status=$?

echo
echo "Exit code: $status"
exit $status
