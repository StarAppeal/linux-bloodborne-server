#!/usr/bin/env bash
# Fetch a score's attached game-data blob by account ID. This is the
# AccountId-routed counterpart of the NpId-routed GetScoreData path and
# corresponds to PS4 SDK's sceNpScoreGetGameDataByAccountId.
#
# Prereq: a previous score-record-data call must have attached a blob to
# (boardId, accountId, pcId). If there's no attached blob, the server
# returns NotFound.
#
# Usage:
#   score-get-data-accountid.sh <npid> <password> <comId> <boardId> <target_accountId> [pcId]
#
# pcId defaults to 0 when not specified.
# Pass 0 for <target_accountId> to look up your own blob.
#
# Examples:
#  # look up game-data blob for account 5
#   score-get-data-accountid.sh Shadow 1234 NPWR12345_00 1 5
#
#  # look up your own game-data blob
#   score-get-data-accountid.sh Shadow 1234 NPWR12345_00 1 0
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

if (( $# < 5 )); then
    echo "Usage: score-get-data-accountid.sh <npid> <password> <comId> <boardId> <target_accountId> [pcId]" >&2
    exit 1
fi
(( $# < 6 )) && set -- "$@" 0  # pcId defaults to 0
echo "[score-get-data-accountid] $1 ${*:3}"
echo

sample score-get-data-accountid "$@"
status=$?

echo
echo "Exit code: $status"
exit $status
