#!/usr/bin/env bash
# Register two test accounts (Shadow and Stephen) used by the other example scripts.
# Run this once before trying the friend/block/score examples.
#
# Accounts created:
#   npid=Shadow     password=1234     email=shadow@test.local
#   npid=Stephen    password=12345    email=stephen@test.local
#
# If the server requires a registration secret key, pass it as the first argument:
#   setup-test-accounts.sh MySecretKey
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

SECRET=${1:-}
echo "[setup] Registering Shadow ..."
sample register Shadow 1234 shadow@test.local ${SECRET:+"$SECRET"}
status=$?
(( status != 0 )) && echo "[setup] Shadow registration failed (account may already exist)"
echo
echo "[setup] Registering Stephen ..."
sample register Stephen 12345 stephen@test.local ${SECRET:+"$SECRET"}
status=$?
(( status != 0 )) && echo "[setup] Stephen registration failed (account may already exist)"
echo
echo "[setup] Done. Run walkthrough-friends.sh to test friend commands."
