#!/usr/bin/env bash
# Seamless co-op on Linux without the launcher window (the Windows kit's 1 - SETUP, 2 - PLAY and
# HOST - Start Servers.bat). The co-op folder ($BB_COOP_DIR, else ~/.local/share/bloodborne-coop)
# keeps the settings (user/coop.json, as BLauncher.exe's), saves, logs and the prepared game.
#
#   coop-run.sh --host <IP or server> --game <CUSA03173 folder> [--update <1.09 folder>]
#       First start: the game folder with the 1.09 update over it (links, nothing copied) and the
#       co-op event files made from it (tools/re/party_boss_events.py); then the game, signed in
#       with this PC's own player id. The options are kept: later starts need none.
#   coop-run.sh [options] [-- run.sh arguments]
#       --harder N, --p2p-port N, --rules rot,travel,vote,scale|off, --unlocked-fps 1,
#       --prepare (make the game folder and event files again), --dry-run, --show
#       (bbport/launcher/bbport_coop_cli.py --help). The party is made in the game's team screen.
#   coop-run.sh server [--listen ADDRESS]
#       Hosts: shadnet (server/build/shadnet, or $SHADNET) in <co-op folder>/server, with
#       BloodborneSeamlessCoop=true. --listen <the address the others reach: LAN, Tailscale> lets
#       them in (the default 127.0.0.1 is this PC only); open UDP 31314 and TCP 31315/20443 in the
#       firewall (ufw/firewalld) for them (31313 stays local).
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
coop_dir=${BB_COOP_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/bloodborne-coop}
python=${PYTHON:-$(command -v python3 || true)}
if [[ -z $python ]]; then echo 'Install Python 3 or set PYTHON.' >&2; exit 1; fi

if [[ ${1:-} == server ]]; then
    shift
    listen=
    while (( $# )); do
        case $1 in
        --listen) listen=${2:?--listen needs an address}; shift 2 ;;
        *) echo "coop-run.sh server: unknown option $1" >&2; exit 1 ;;
        esac
    done
    shadnet=${SHADNET:-$here/../server/build/shadnet}
    if [[ ! -x $shadnet ]]; then
        echo "No shadnet at $shadnet: build server/ (cmake -S server -B server/build -G Ninja && ninja -C server/build shadnet) or set SHADNET." >&2
        exit 1
    fi
    server_dir=$coop_dir/server
    mkdir -p "$server_dir"
    for file in worlds.cfg scoreboards.cfg; do
        [[ -f $server_dir/$file ]] || cp "$here/../server/$file" "$server_dir/$file"
    done
    # The settings this host needs, set at every start (shadnet fills in the others it does not
    # find): seamless co-op on (its default is off; docs/SEAMLESS_COOP_PLAN.md: the summon broker's
    # cross-map claims need it), and the address with --listen.
    settings=(BloodborneSeamlessCoop=true)
    [[ -n $listen ]] && settings+=("Host=$listen")
    "$python" -I - "$server_dir/shadnet.cfg" "${settings[@]}" <<'EOF'
import pathlib, sys
path, wanted = pathlib.Path(sys.argv[1]), dict(arg.split('=', 1) for arg in sys.argv[2:])
lines = path.read_text(encoding='utf-8').splitlines() if path.exists() else ['[General]']
if '[General]' not in lines:
    lines.insert(0, '[General]')
for key, value in wanted.items():
    at = [i for i, line in enumerate(lines) if line.split('=', 1)[0] == key]
    if at:
        lines[at[0]] = f'{key}={value}'
    else:
        lines.insert(lines.index('[General]') + 1, f'{key}={value}')
path.write_text('\n'.join(lines) + '\n', encoding='utf-8')
EOF
    echo "shadnet in $server_dir (settings: shadnet.cfg)"
    # shadnet works in its own folder unless SHADNET_HOME names another (server/src/main.cpp).
    export SHADNET_HOME=$server_dir
    # The build check on join: this server is the release VERSION.txt names (shadnet reads BB_RELEASE_VERSION).
    version=$("$python" -I -c "import sys; sys.path.insert(0, sys.argv[1]); import bbport_coop; print(bbport_coop.release_version())" "$here/launcher")
    [[ -n $version ]] && export BB_RELEASE_VERSION=$version
    exec "$shadnet"
fi

exec "$python" "$here/launcher/bbport_coop_cli.py" --dir "$coop_dir" "$@"
