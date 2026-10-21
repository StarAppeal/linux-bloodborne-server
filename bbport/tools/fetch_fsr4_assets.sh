#!/usr/bin/env bash
# Downloads the FSR 4 v07 INT8/DOT4 asset set (SPIR-V passes, model initializers, pre-pass
# weights) used by the FSR-Vulkan provider into fsr4_shaders/. The assets were built by
# Q2RTX from AMD's MIT-licensed FidelityFX SDK FSR 4 source (see LICENSE-FSR4-v07.txt);
# they are not part of this repository. Output 1920x1080 needs the 1080 tier, 1440p and 2160p
# outputs (menu: output resolution) the 2160 tier; BB_FSR4_TIERS selects them. BB_FSR4_DIR is
# the folder (default fsr4_shaders). BB_FSR4_CACHE is a download cache shared by checkouts
# (make-release.ps1: <BB_ROOT>\cache\fsr4): files missing there are downloaded into it, eight at a
# time, and copied from it, so a fresh checkout does not fetch the 189 files again.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
commit=ae8d628fae208813172446d1e49ed94150b04658
base="https://raw.githubusercontent.com/FireBurn/Q2RTX/$commit/baseq2/fsr4_shaders"
dest=${BB_FSR4_DIR:-fsr4_shaders}
mkdir -p "$dest"
# The files of one pinned commit never change, so the cache keeps them by commit.
store=$dest
if [[ -n ${BB_FSR4_CACHE:-} ]]; then store=$BB_FSR4_CACHE/$commit; mkdir -p "$store"; fi
files=(LICENSE-FSR4-v07.txt rcas.spv spd_auto_exposure.spv)
for model in native quality balanced performance ultraperf drs; do
    files+=("fsr4_model_v07_i8_${model}_initializers.bin"
            "fsr4_model_v07_i8_${model}_pre_weights.bin"
            "fsr4_model_v07_i8_${model}_shader_manifest.json")
    for tier in ${BB_FSR4_TIERS:-1080 2160}; do
        files+=("fsr4_model_v07_i8_${model}_${tier}_pre.spv"
                "fsr4_model_v07_i8_${model}_${tier}_post.spv")
        for pass in $(seq 1 12); do
            files+=("fsr4_model_v07_i8_${model}_${tier}_pass${pass}.spv")
        done
    done
done
missing=()
for file in "${files[@]}"; do
    if [[ ! -s $dest/$file && ! -s $store/$file ]]; then missing+=("$file"); fi
done
# A file appears only once complete (.part, then mv): a failed download leaves nothing behind.
download() { # download <file> <folder> <base URL>
    curl -fsSL --retry 3 -o "$2/$1.part" "$3/$1" && mv "$2/$1.part" "$2/$1" || { rm -f "$2/$1.part"; exit 1; }
}
export -f download
if [[ ${#missing[@]} -gt 0 ]]; then
    printf '%s\0' "${missing[@]}" | xargs -0 -n 1 -P 8 bash -c 'download "$2" "$0" "$1"' "$store" "$base" ||
        { echo "FSR 4 assets: a download failed (run again to fetch the rest)" >&2; exit 1; }
fi
if [[ $store != "$dest" ]]; then
    for file in "${files[@]}"; do
        if [[ ! -s $dest/$file ]]; then cp "$store/$file" "$dest/$file"; fi
    done
fi
echo "FSR 4 assets: ${#files[@]} files in $(cd "$dest" && pwd) (${#missing[@]} downloaded)"
# Faster, bit-exact post passes (fsr4_shaders/opt), when spirv-cross and glslang are available.
if command -v spirv-cross >/dev/null && command -v glslangValidator >/dev/null; then
    bash tools/fsr4_optimize.sh
fi
