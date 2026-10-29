#!/usr/bin/env bash
# NPScore walkthrough — end-to-end demo using Shadow and Stephen.
#
# Assumes setup-test-accounts.sh has already been run.
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

COMID=NPWR00000_00
BOARD=1
banner "Step 1: Query board config (may show defaults if no scores yet)"
sample score-board Shadow 1234 "$COMID" "$BOARD"
echo
banner "Step 2: Shadow records 50000"
sample score-record Shadow 1234 "$COMID" "$BOARD" 0 50000 "First run"
echo
banner "Step 3: Stephen records 75000"
sample score-record Stephen 12345 "$COMID" "$BOARD" 0 75000 "Better run"
echo
banner "Step 4: Shadow tries 40000 should fail with ScoreNotBest"
sample score-record Shadow 1234 "$COMID" "$BOARD" 0 40000 "Bad run"
echo
banner "Step 5: Shadow records 90000 beats Stephen, should be rank 1"
sample score-record Shadow 1234 "$COMID" "$BOARD" 0 90000 "Personal best"
echo
#   Expected: Shadow #1 90000, Stephen #2 75000
banner "Step 6: Fetch full leaderboard (top 10)"
sample score-range Shadow 1234 "$COMID" "$BOARD" 1 10
echo
banner "Step 7: Look up Stephen's score by NP ID"
sample score-get-npid Shadow 1234 "$COMID" "$BOARD" Stephen
echo
banner "Step 8: Look up Shadow's own score by NP ID"
sample score-get-npid Shadow 1234 "$COMID" "$BOARD" Shadow
echo
banner "Walkthrough complete."
