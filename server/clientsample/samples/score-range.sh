#!/usr/bin/env bash
# Fetch a range of leaderboard entries by rank position.
#
# Returns entries startRank through startRank+numRanks-1 (1-based).
# Prints each entry's rank, NP ID, score, and pcId.
#
# Usage:
#   score-range.sh <npid> <password> <comId> <boardId> <startRank> <numRanks>
#
# Examples:
#   score-range.sh Shadow 1234 NPWR12345_00 1 1 10    <- top 10
#   score-range.sh Shadow 1234 NPWR12345_00 1 11 10   <- positions 11-20
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

if (( $# < 6 )); then
    echo "Usage: score-range.sh <npid> <password> <comId> <boardId> <startRank> <numRanks>" >&2
    exit 1
fi
echo "[score-range] $1 ${*:3}"
echo

sample score-range "$@"
status=$?

echo
echo "Exit code: $status"
exit $status
