#!/usr/bin/env bash
# Block flow walkthrough.
#
# Demonstrates blocking a friend (severs the friendship) and then unblocking.
# Assumes walkthrough-friends.sh has already been run so Shadow and Stephen
# are mutual friends before this script starts.
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

banner "Step 1: Shadow logs in (Stephen should be a friend)"
sample login Shadow 1234
echo
banner "Step 2: Shadow blocks Stephen (friendship severed, FriendLost sent to Stephen)"
sample block-add Shadow 1234 Stephen
echo
banner "Step 3: Shadow logs in (Stephen should be in blocked list, not friends)"
sample login Shadow 1234
echo
banner "Step 4: Stephen tries to add Shadow back (should fail with Blocked)"
sample friend-add Stephen 12345 Shadow
echo
banner "Step 5: Shadow unblocks Stephen"
sample block-remove Shadow 1234 Stephen
echo
banner "Step 6: Shadow logs in (Stephen should be gone from blocked list)"
sample login Shadow 1234
echo
banner "Step 7: Shadow adds Stephen again (unblocking does not restore the friendship)"
sample friend-add Shadow 1234 Stephen
echo
sample friend-add Stephen 12345 Shadow
echo
banner "Step 8: Shadow logs in (Stephen should be a friend again)"
sample login Shadow 1234
echo
banner "Walkthrough complete."
