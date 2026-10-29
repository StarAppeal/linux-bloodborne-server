# Shared by the sample scripts (the Linux counterparts of the .bat files): sourced, not run.
# HOST, PORT and SHADNET_SAMPLE (the client's path) can be set in the environment.

HOST=${HOST:-127.0.0.1}
PORT=${PORT:-31313}

samples_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ -z ${SHADNET_SAMPLE:-} ]]; then
    for candidate in "$(command -v shadnet-sample 2>/dev/null)" \
                     "$samples_dir/../build/shadnet-sample" "$samples_dir/shadnet-sample"; do
        if [[ -n $candidate && -x $candidate ]]; then SHADNET_SAMPLE=$candidate; break; fi
    done
fi
if [[ -z ${SHADNET_SAMPLE:-} ]]; then
    echo 'shadnet-sample not found: build server/clientsample or set SHADNET_SAMPLE.' >&2
    exit 1
fi

# The client against HOST:PORT, with the rest of the command line.
sample() {
    "$SHADNET_SAMPLE" "$HOST" "$PORT" "$@"
}

banner() {
    echo '============================================================'
    echo " $*"
    echo '============================================================'
}
