#!/usr/bin/env bash
# Register a new account on the shadNet server.
#
# Usage:
#   register.sh <npid> <password> <email> [secretKey]
#
# secretKey is optional. If the server has RegistrationSecretKey set in its
# config, you must supply the matching key or the registration will be rejected.
#
# Examples:
#   register.sh Shadow 1234 shadow@example.com
#   register.sh Shadow 1234 shadow@example.com MySecretKey
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

if (( $# < 3 )); then
    echo "Usage: register.sh <npid> <password> <email> [secretKey]" >&2
    exit 1
fi
echo "[register] $1 ${*:3}"
echo

sample register "$@"
status=$?

echo
echo "Exit code: $status"
exit $status
