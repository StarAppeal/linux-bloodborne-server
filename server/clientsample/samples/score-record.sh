#!/usr/bin/env bash
# Submit a score to a leaderboard.
#
# The server applies update_mode rules automatically:
#   NORMAL_UPDATE — only stores the score if it is better than the existing one
#   FORCE_UPDATE  — always overwrites regardless of value
#
# The reply prints the 1-based rank achieved.
# If the score does not make the board (worse than all existing entries)
# the server returns ScoreNotBest.
#
# Usage:
#   score-record.sh <npid> <password> <comId> <boardId> <pcId> <score> [comment]
#
# pcId   — character/slot identifier; use 0 for single-character games
# score  — integer; higher is better for DESCENDING boards (most games)
#           lower is better for ASCENDING boards (time trial / speedrun)
#
# Examples:
#   score-record.sh Shadow 1234 NPWR12345_00 1 0 98500
#   score-record.sh Shadow 1234 NPWR12345_00 1 0 98500 "Perfect run"
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

if (( $# < 6 )); then
    echo "Usage: score-record.sh <npid> <password> <comId> <boardId> <pcId> <score> [comment]" >&2
    exit 1
fi
echo "[score-record] $1 ${*:3}"
echo

sample score-record "$@"
status=$?

echo
echo "Exit code: $status"
exit $status
