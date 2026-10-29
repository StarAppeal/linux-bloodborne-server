#!/usr/bin/env bash
# Look up a specific player's score on a leaderboard by their account ID
# (the server-issued user_id that comes back in LoginReply). This is the
# AccountId-routed counterpart of score-get-npid.sh and corresponds to
# the PS4 SDK's sceNpScoreGetRankingByAccountId path.
#
# The server resolves the account ID directly against its primary key,
# with no npid-string lookup needed. Returns the same rank-entry shape
# as score-get-npid (or an empty/zero entry if that account has no score
# on this board).
#
# Usage:
#   score-get-accountid.sh <npid> <password> <comId> <boardId> <target_accountId> [pcId]
#
# pcId defaults to 0 when not specified.
# Pass 0 for <target_accountId> to look up your own score.
#
# Examples:
#  # look up account 5 (whatever user_id 5 is on your server)
#   score-get-accountid.sh Shadow 1234 NPWR12345_00 1 5
#
#  # look up yourself (resolved from LoginReply.user_id)
#   score-get-accountid.sh Shadow 1234 NPWR12345_00 1 0
#
#  # same but with explicit pcId
#   score-get-accountid.sh Shadow 1234 NPWR12345_00 1 5 0
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

if (( $# < 5 )); then
    echo "Usage: score-get-accountid.sh <npid> <password> <comId> <boardId> <target_accountId> [pcId]" >&2
    exit 1
fi
(( $# < 6 )) && set -- "$@" 0  # pcId defaults to 0
echo "[score-get-accountid] $1 ${*:3}"
echo

sample score-get-accountid "$@"
status=$?

echo
echo "Exit code: $status"
exit $status
