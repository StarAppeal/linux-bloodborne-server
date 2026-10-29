#!/usr/bin/env bash
# Look up a specific player's score on a leaderboard by their NP ID.
#
# Returns that player's rank entry (or an empty/zero entry if they have
# no score on this board).
#
# Usage:
#   score-get-npid.sh <npid> <password> <comId> <boardId> <target_npid> [pcId]
#
# pcId defaults to 0 when not specified.
#
# Examples:
#   score-get-npid.sh Shadow 1234 NPWR12345_00 1 Stephen
#   score-get-npid.sh Shadow 1234 NPWR12345_00 1 Stephen 0
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

if (( $# < 5 )); then
    echo "Usage: score-get-npid.sh <npid> <password> <comId> <boardId> <target_npid> [pcId]" >&2
    exit 1
fi
(( $# < 6 )) && set -- "$@" 0  # pcId defaults to 0
echo "[score-get-npid] $1 ${*:3}"
echo

sample score-get-npid "$@"
status=$?

echo
echo "Exit code: $status"
exit $status
