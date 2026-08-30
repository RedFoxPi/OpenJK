#!/usr/bin/env python3
"""
Render-regression capture harness.

Launches the engine once per scene in scenes.json, headlessly (SDL2's
"offscreen" video driver, no display required), and captures a screenshot.
Works against any renderer build (rd-vanilla today, a future replacement
renderer later) as long as it implements the same refexport_t interface -
point --bindir at a different build to compare.

Requires the user's own legally-owned game assets (fs_basepath pointing at
a directory containing base/assets0-3.pk3, e.g. extracted from the GOG
installer). This script and scenes.json contain no game content.

Usage:
    python3 capture.py --bindir /path/to/install/JediAcademy \\
        --basepath /path/to/gamedata --homepath /path/to/gamedata/home \\
        --out /path/to/output/dir

Exit code is nonzero if any scene failed to launch/screenshot, or if a
sanitizer (ASan/UBSan) report was found in a scene's log.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

SANITIZER_PATTERNS = [
    re.compile(r"ERROR: AddressSanitizer"),
    re.compile(r"ERROR: LeakSanitizer"),
    re.compile(r"runtime error:"),
    re.compile(r"SUMMARY: \w+Sanitizer"),
]

BINARIES = {
    "sp": {"exe": "openjk_sp.x86_64"},
    "mp": {"exe": "openjk.x86_64"},
}


def find_screenshot(homepath: Path, scene_id: str) -> Path | None:
    # fs_game can land screenshots under home/base or home/OpenJK depending
    # on the compiled-in default mod dir; check both.
    for moddir in ("base", "OpenJK"):
        candidate = homepath / moddir / "screenshots" / f"{scene_id}.png"
        if candidate.is_file():
            return candidate
    return None


def build_args(scene: dict, basepath: Path, homepath: Path) -> list[str]:
    args = [
        "+set", "fs_basepath", str(basepath),
        "+set", "fs_homepath", str(homepath),
        "+set", "r_fullscreen", "0",
        "+set", "r_mode", "-1",
        "+set", "r_customwidth", "800",
        "+set", "r_customheight", "600",
        "+set", "nosound", "1",
        "+set", "s_initsound", "0",
        "+set", "com_journal", "0",
        "+set", "in_nograb", "1",
        "+set", "com_maxfps", "0",
        "+set", "cg_draw2D", "0" if scene.get("map") else "1",
    ]
    for k, v in scene.get("extra_set", {}).items():
        args += ["+set", k, str(v)]
    if scene.get("map"):
        args += ["+devmap", scene["map"]]
    args += ["+wait", str(scene.get("wait_frames", 120))]
    args += ["+screenshot_png", scene["id"]]
    args += ["+quit"]
    return args


def run_scene(scene: dict, bindir: Path, basepath: Path, homepath: Path,
              out_dir: Path, timeout: int, env: dict) -> dict:
    exe = bindir / BINARIES[scene["binary"]]["exe"]
    args = build_args(scene, basepath, homepath)
    log_path = out_dir / f"{scene['id']}.log"

    result = {"id": scene["id"], "ok": False, "sanitizer_hits": [], "log": str(log_path)}

    old_shot = find_screenshot(homepath, scene["id"])
    if old_shot:
        old_shot.unlink()

    try:
        proc = subprocess.run(
            [str(exe), *args],
            cwd=str(bindir),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
        )
        output = proc.stdout.decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired as e:
        output = (e.stdout or b"").decode("utf-8", errors="replace")
        output += "\n[capture.py] TIMED OUT\n"

    log_path.write_text(output)

    for pattern in SANITIZER_PATTERNS:
        if pattern.search(output):
            result["sanitizer_hits"].append(pattern.pattern)

    shot = find_screenshot(homepath, scene["id"])
    if shot:
        dest = out_dir / f"{scene['id']}.png"
        shutil.copy(shot, dest)
        result["screenshot"] = str(dest)
        result["ok"] = True
    else:
        result["ok"] = False

    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bindir", required=True, type=Path, help="Directory containing the engine executables and renderer/game .so files")
    ap.add_argument("--basepath", required=True, type=Path, help="fs_basepath (directory containing base/assets*.pk3)")
    ap.add_argument("--homepath", required=True, type=Path, help="fs_homepath (writable dir for configs/screenshots)")
    ap.add_argument("--out", required=True, type=Path, help="Output directory for screenshots + logs")
    ap.add_argument("--scenes", default=Path(__file__).parent / "scenes.json", type=Path)
    ap.add_argument("--filter", default=None, help="Only run scene ids containing this substring")
    ap.add_argument("--timeout", default=60, type=int)
    args = ap.parse_args()

    manifest = json.loads(args.scenes.read_text())
    scenes = manifest["scenes"]
    if args.filter:
        scenes = [s for s in scenes if args.filter in s["id"]]

    args.out.mkdir(parents=True, exist_ok=True)
    args.homepath.mkdir(parents=True, exist_ok=True)

    env = dict(os.environ)

    results = []
    for scene in scenes:
        print(f"[{scene['id']}] running ({scene.get('map') or 'menu'})...", flush=True)
        t0 = time.time()
        r = run_scene(scene, args.bindir, args.basepath, args.homepath, args.out, args.timeout, env)
        dt = time.time() - t0
        status = "OK" if r["ok"] and not r["sanitizer_hits"] else ("SANITIZER" if r["sanitizer_hits"] else "FAIL")
        print(f"[{scene['id']}] {status} ({dt:.1f}s)")
        if r["sanitizer_hits"]:
            print(f"    sanitizer patterns matched: {r['sanitizer_hits']} -> see {r['log']}")
        results.append(r)

    print("\n=== Summary ===")
    failed = 0
    for r in results:
        status = "OK" if r["ok"] and not r["sanitizer_hits"] else ("SANITIZER" if r["sanitizer_hits"] else "FAIL")
        if status != "OK":
            failed += 1
        print(f"  {r['id']:24s} {status}")

    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
