# bloodborne-server

Seamless co-op for Bloodborne (CUSA03173, patch 01.09) on shadPS4, modeled on Elden Ring Seamless Co-op.

| Folder | Upstream (subtree, full history) | Role |
|---|---|---|
| `emulator/` | [Wozzardman/shadp2p](https://github.com/Wozzardman/shadp2p) | shadPS4 with Bloodborne P2P and the native game hooks (`src/core/bloodborne_re.cpp`) |
| `server/` | [Wozzardman/shadnet-p2p](https://github.com/Wozzardman/shadnet-p2p) | accounts, matchmaking, signaling, WebAPI and the Bloodborne summon broker |
| `docs/` | | [plan and milestones](docs/SEAMLESS_COOP_PLAN.md) |
| `tools/` | | Windows build, local CI and test-rig scripts |

## Dev setup (Windows)

Prerequisites: VS 2022 Build Tools (C++ workload, Windows SDK), LLVM **20.1.8** (clang-cl; 23 breaks
googletest), CMake, Ninja, jj, Qt 6.8.3 `msvc2022_64` in `C:\Qt` (via `aqt`), and Python 3.12 with
`clang-format==19.1.7` and `reuse`.

```powershell
jj git clone --colocate https://github.com/lKaniel/bloodborne-server.git C:\bb\bloodborne-server
cd C:\bb\bloodborne-server
git config submodule.emulator/externals/mesa-kosmickrisp.update none   # macOS-only
git submodule update --init --recursive
```

`BB_ROOT` (default `C:\bb`) holds the checkout. Build output goes to `C:\bb\out`, outside the tree.

| Script | What it does |
|---|---|
| `tools/env.ps1` | dot-source it to enter an MSVC x64 dev shell with LLVM and Qt on PATH |
| `tools/build-emu.ps1` | builds the emulator (`out\emulator\build`) and its unit tests (`out\emulator\tests`) |
| `tools/build-server.ps1` | builds shadnet and its tests (`out\server`) |
| `tools/ci-check.ps1 -Component emulator\|server [-All]` | runs the upstream CI checks locally: clang-format 19, trailing whitespace, REUSE, build, ctest |
| `tools/rig-setup.ps1` | creates a two-player test rig on one PC (own shadnet, backend, host and guest dirs) |
| `tools/online-kit.ps1` | packs the current build into a kit for playing with a friend over the internet ([guide](docs/ONLINE_TEST.md)) |

## Pulling upstream updates

```powershell
git fetch upstream-emulator main
git merge -s subtree -Xsubtree=emulator upstream-emulator/main
git fetch upstream-server main
git merge -s subtree -Xsubtree=server upstream-server/main
```

## Workflow

- Version control is jj. Commit signing is off for this repo.
- Red-green: write the failing test, see it fail, then make it pass.
- Run `tools/ci-check.ps1` for every touched component and get a green result before pushing.
