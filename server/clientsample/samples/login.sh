#!/usr/bin/env bash
# Log in and print the current friend lists for an account.
#
# Usage:
#   login.sh <npid> <password> [token]
#
# Example:
#   login.sh shadow 1234
#   login.sh shadow 1234 MYTOKEN123   (with email validation token unsupported)
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

if (( $# < 2 )); then
    echo "Usage: login.sh <npid> <password> [token]" >&2
    exit 1
fi
echo "[login] $1 ${*:3}"
echo

sample login "$@"
status=$?

echo
echo "Exit code: $status"
exit $status
