#!/usr/bin/env bash
# Builds dist/Bloodborne-bbport-x86_64.AppImage: the port's current build (run build.sh first),
# its scripts and the launcher, bundled with their Nix closure (nix-appimage: the AppImage mounts
# its /nix/store with user namespaces, available on SteamOS and most desktops).
# Running it opens the launcher; `--play` starts the game with the launcher's saved settings.
# Data (generated files, saves, bbport.ini): ~/.local/share/bbport-coop (BB_DATA_DIR), launcher
# settings: ~/.config/bbport-launcher-gtk (as bb-launcher.sh); co-op keeps its
# own in ~/.local/share/bloodborne-coop.
# The co-op release's version goes into the package's VERSION.txt (the server refuses a game of
# another release): BB_RELEASE_VERSION=1.3.0, else bbport/VERSION.txt or the nearest v* git tag.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
[[ -f out/bb-probe && -f out/gpu/libbbgpu.so ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
root=$PWD
work=
made_version=
trap '[[ -z $work ]] || rm -rf "$work"; [[ -z $made_version ]] || rm -f VERSION.txt' EXIT
# The libraries' store paths (RUNPATH entries and their closures come along).
{
    echo '['
    for elf in out/bb-probe out/gpu/libbbgpu.so; do
        readelf -d "$elf" | sed -n 's/.*\[\(.*\)\]/\1/p' | tr ':' '\n'
    done | grep -o '^/nix/store/[^/]*' | sort -u | grep -v -- '-nix-shell$' | sed 's/.*/  "&"/'
    echo ']'
} > packaging/runtime-paths.nix
# Built against the system's libraries (pacman, apt) instead of nix-shell's, the binaries would
# load the host's libraries at runtime, which other distributions do not have in those versions.
grep -q /nix/store packaging/runtime-paths.nix || {
    echo 'The build does not use /nix/store libraries: rebuild in nix-shell --pure shell.nix --run "bash build.sh".' >&2
    exit 1
}
# FSR 4 assets (MIT, a pinned download; the Windows release bundles them too).
[[ -d fsr4_shaders ]] || bash tools/fetch_fsr4_assets.sh
version=${BB_RELEASE_VERSION:-$(python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1]); import bbport_coop; print(bbport_coop.release_version())' launcher)}
[[ -n $version ]] || { echo 'No release version: set BB_RELEASE_VERSION (e.g. 1.3.0).' >&2; exit 1; }
if [[ ! -f VERSION.txt ]]; then
    printf 'Yharnam Together: Bloodborne Seamless Co-op v%s (Linux AppImage)\n' "$version" > VERSION.txt
    made_version=1
fi
echo "Release version: $(head -n1 VERSION.txt)"
work=$(mktemp -d)
(cd "$work" && nix bundle --impure --bundler github:ralismark/nix-appimage \
    --expr "import $root/packaging {}")
mkdir -p dist
install -m755 "$work/bbport.AppImage" dist/Bloodborne-bbport-x86_64.AppImage
ls -lh dist/Bloodborne-bbport-x86_64.AppImage
