#!/usr/bin/env bash
# Full friend flow walkthrough.
#
# Demonstrates the complete request -> accept cycle between Shadow and Stephen.
# Assumes setup-test-accounts.sh has already been run.
#
# Steps performed:
#   1. Shadow logs in                    (shows empty friend lists)
#   2. Shadow sends Stephen a friend request (Stephen gets FriendQuery notification)
#   3. Shadow logs in again              (shows Stephen in requests_sent)
#   4. Stephen accepts Shadow's request      (both get FriendNew notification)
#   5. Shadow logs in again              (shows Stephen as a friend)
#   6. Stephen logs in                      (shows Shadow as a friend)
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

banner "Step 1: Shadow logs in (no friends yet)"
sample login Shadow 1234
echo
banner "Step 2: Shadow sends Stephen a friend request"
sample friend-add Shadow 1234 Stephen
echo
banner "Step 3: Shadow logs in again (Stephen should appear in requests_sent)"
sample login Shadow 1234
echo
banner "Step 4: Stephen accepts Shadow's request (mutual friendship formed)"
sample friend-add Stephen 12345 Shadow
echo
banner "Step 5: Shadow logs in (Stephen should now appear as a friend)"
sample login Shadow 1234
echo
banner "Step 6: Stephen logs in (Shadow should appear as a friend)"
sample login Stephen 12345
echo
banner "Walkthrough complete."
