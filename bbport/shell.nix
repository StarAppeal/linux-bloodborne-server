# Build environment: `nix-shell native_probe/shell.nix --run 'bash native_probe/run.sh'`
{ pkgs ? import <nixpkgs> {} }:
# Clang (CC/CXX from its stdenv): GCC 13 lacks deducing `this`, and GCC 16 finds the
# `x = {}` assignments to Vulkan-Hpp structs ambiguous.
(pkgs.mkShell.override { stdenv = pkgs.llvmPackages.stdenv; }) {
  packages = with pkgs; [
    gnumake cmake ninja pkg-config python3 binutils git llvmPackages.lld llvmPackages.llvm
    vulkan-headers vulkan-loader sdl3
    # GPU library (gpu/): shadPS4 video core dependencies
    ffmpeg-headless boost fmt magic-enum robin-map xxhash vulkan-memory-allocator glslang spirv-cross xbyak zydis spirv-headers miniz libx11 libxcb xorgproto wayland
    # Seamless co-op (gpu/bbcoop, the emulator's bloodborne_* sources)
    protobuf nlohmann_json openssl zlib
  ];
}
