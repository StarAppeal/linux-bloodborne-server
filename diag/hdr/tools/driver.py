"""HDR scene diagnostic: one rig port game (host role) on the hdr-scene build, autoplay to bright
spots, frame/scene/capture dumps at each, then close everything the run started."""
import os
import pathlib
import shutil
import sys
import time

DIAG = pathlib.Path(r"C:\bb\hdr-diag")
os.environ["BB_RIG_PORT_HOME"] = str(DIAG / "build")
sys.path.insert(0, r"C:\bb\hdr-src\tools\rig")
import run_scenario as rs  # noqa: E402
import scenarios as sc  # noqa: E402

rs.use_engine("port")
ROLE = "host"
os.environ.update({
    "BB_HDR_SCENE_TEST": "1",
    "BB_DUMP_DIR": str(DIAG),
    "BB_FRAME_DUMP_TRIGGER": str(DIAG / "frame.trig"),
    "BB_DUMP_TRIGGER": str(DIAG / "scene.trig"),
    "BB_CAPTURE_TRIGGER": str(DIAG / "cap.trig"),
    "BB_CAPTURE_DIR": str(DIAG),
    "BB_DUMP_FRAMES": "2",
})

SETTINGS = {
    "upscaler": "dlss", "preset": "1", "output_res": "1920x1080", "live_resolution": "0",
    "model_lod": "0", "effect_chromatic_aberration": "1", "effect_dof": "1",
    "effect_motion_blur": "1", "effect_ssao": "1", "effect_game_aa": "1",
    "effect_dynamic_shadows": "1", "window_mode": "windowed", "hdr": "0", "hdr_scene": "1",
    "hdr_highlights": "0.50", "hdr_peak_nits": "1000", "hdr_paper_white": "200",
}

SCRIPT = [
    sc.START, "godmode", "wait 8",
    "log diag-spot1", "wait 6", "shot cy-lamp",
    "teleport -200.0 -40.5 206.8 0.0", "wait 4", "log diag-spot2", "wait 6", "shot cy-clinic-a",
    "teleport -200.0 -40.5 212.8 3.1416", "wait 4", "log diag-spot3", "wait 6",
    "shot cy-clinic-b",
    f"travel {sc.HUNTERS_DREAM}", "wait 12", "log diag-spot4", "wait 6", "shot dream",
    f"ensure-map {sc.CATHEDRAL_WARD} {sc.CATHEDRAL_WARD_LAMP}", "wait 12", "log diag-spot5",
    "wait 6", "shot ward",
    "done",
]


def write_settings():
    ini = rs.ENGINE.role_dir(ROLE) / "bbport.ini"
    lines = ini.read_text(encoding="utf-8").splitlines() if ini.exists() else []
    keys = set()
    out = []
    for line in lines:
        key = line.split("=", 1)[0].strip()
        if key in SETTINGS:
            out.append(f"{key}={SETTINGS[key]}")
            keys.add(key)
        else:
            out.append(line)
    out += [f"{k}={v}" for k, v in SETTINGS.items() if k not in keys]
    ini.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")


def main() -> int:
    if missing := rs.ENGINE.missing():
        print(missing)
        return 2
    if rs.games_running():
        print("A game is running; not starting another.")
        return 2
    run_dir = DIAG / "run2"
    run_dir.mkdir(parents=True, exist_ok=True)
    shots = rs.ENGINE.shots(ROLE)
    if shots.exists():
        shutil.rmtree(shots, ignore_errors=True)
    rs.restore_baseline(ROLE)
    rs.ENGINE.prepare(ROLE)
    write_settings()
    rs.start_servers(run_dir)
    game = rs.launch(ROLE, SCRIPT, party="diag:hdr")
    print("started", flush=True)
    seen = set()
    started = time.time()
    result = "timeout"
    try:
        while time.time() < started + 900:
            if game.poll() is not None:
                result = f"game exited {game.returncode}"
                break
            text = rs.read_log(ROLE)
            for n in range(1, 6):
                tag = f"diag-spot{n}"
                if tag in text and tag not in seen:
                    seen.add(tag)
                    time.sleep(2.5)
                    (DIAG / "scene.trig").write_text("1")
                    (DIAG / "frame.trig").write_text("1")
                    time.sleep(4)
                    spot = DIAG / tag
                    spot.mkdir(exist_ok=True)
                    for f in list(DIAG.glob("*.raw")):
                        shutil.move(str(f), str(spot / f.name))
                    print(f"{tag}: triggers at {time.time() - started:.0f} s", flush=True)
            if "autoplay: done" in text:
                result = "done (ok)" if "done (ok)" in text else "done (failed)"
                time.sleep(3)
                break
            time.sleep(0.5)
    finally:
        print("result:", result, flush=True)
        rs.stop_game(game)
        rs.stop_games()
        rs.stop_servers()
        rs.keep_logs(run_dir, [ROLE])
        if shots.exists():
            shutil.copytree(shots, run_dir / "shots", dirs_exist_ok=True)
        shutil.copy2(DIAG / "host-bbport.ini.bak", rs.ENGINE.role_dir(ROLE) / "bbport.ini")
    return 0


if __name__ == "__main__":
    sys.exit(main())
