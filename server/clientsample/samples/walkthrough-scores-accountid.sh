#!/usr/bin/env bash
# AccountId-routed score walkthrough.
#
# Demonstrates the AccountId path through the complete record→query→
# get-game-data flow. Mirrors walkthrough-scores.sh but uses the
# server-side user_id instead of the npid string for every lookup.
#
# Why this matters: the AccountId path is what PS4 games use when they
# track peers by the PSN account ID rather than the online ID. On this
# server the account ID is the user_id column returned in LoginReply, so
# once you've logged in once you can resolve both self and any known
# peer by their numeric ID without round-tripping through an npid lookup.
#
# Assumes setup-test-accounts.sh has already been run.
# Prereq: Shadow (user_id 1) and Stephen (user_id 2) exist. If your
# assignments differ, edit SHADOW_ID and STEPHEN_ID below.
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

COMID=NPWR00000_00
BOARD=1
SHADOW_ID=1
STEPHEN_ID=2
banner "Step 1: Shadow records 50000 with a game-data blob"
sample score-record Shadow 1234 "$COMID" "$BOARD" 0 50000 "AccountId demo"
echo
banner "Step 2: Stephen records 75000"
sample score-record Stephen 12345 "$COMID" "$BOARD" 0 75000 "Pushing Shadow"
echo
#   Expected: Stephen's entry with rank=1, score=75000
banner "Step 3: Look up Stephen's score by account ID ($STEPHEN_ID)"
sample score-get-accountid Shadow 1234 "$COMID" "$BOARD" "$STEPHEN_ID"
echo
#   The sample resolves 0 to our LoginReply.user_id automatically.
banner "Step 4: Shadow looks up his own score by account ID (pass 0 = self)"
sample score-get-accountid Shadow 1234 "$COMID" "$BOARD" 0
echo
#   Expected: empty rank entry, the server returns an empty-npid sentinel
banner "Step 5: Look up a non-existent account (99)"
sample score-get-accountid Shadow 1234 "$COMID" "$BOARD" 99
echo
#   leaderboard contents now include accountId in each entry.
banner "Step 6: Compare and fetch the same board via range to confirm"
sample score-range Shadow 1234 "$COMID" "$BOARD" 1 10
echo
banner "Walkthrough complete."
