#!/usr/bin/env bash
# Query the configuration of a specific leaderboard.
#
# Usage:
#   score-board.sh <npid> <password> <comId> <boardId>
#
# Example:
#   score-board.sh Shadow 1234 NPWR12345_00 1
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

if (( $# < 4 )); then
    echo "Usage: score-board.sh <npid> <password> <comId> <boardId>" >&2
    exit 1
fi
echo "[score-board] $1 ${*:3}"
echo

sample score-board "$@"
status=$?

echo
echo "Exit code: $status"
exit $status
