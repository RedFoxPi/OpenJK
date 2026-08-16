#!/usr/bin/env python3
"""
Compares screenshots from two renderer runs of the same scene (e.g. capture.py
run once with cl_renderer rdsp-vanilla and once with cl_renderer rd-vulkan)
and reports a similarity score per scene, plus a visual diff image.

This is intentionally simple - a per-pixel mean-difference score and an
amplified difference image - not a perceptual/SSIM metric. It's meant to catch
"this is obviously broken" (wrong colors, missing geometry, solid black) and
to make partial mismatches (like unimplemented shader effects) visible at a
glance, not to be a pixel-perfect regression gate.

Requires Pillow (pip install pillow) - not a dependency of capture.py itself,
only of this comparison step.

Usage:
    python3 diff.py --a screenshots-vanilla --b screenshots-vulkan --out diffs
    python3 diff.py --a screenshots-vanilla --b screenshots-vulkan --out diffs --scenes scenes.json
"""

import argparse
import json
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageStat


def load_scene_ids(scenes_path):
	with open(scenes_path) as f:
		data = json.load(f)
	return [s["id"] for s in data["scenes"]]


def compare_pair(path_a: Path, path_b: Path, diff_out: Path):
	img_a = Image.open(path_a).convert("RGB")
	img_b = Image.open(path_b).convert("RGB")

	if img_a.size != img_b.size:
		return {
			"status": "SIZE_MISMATCH",
			"detail": f"{img_a.size} vs {img_b.size}",
			"mean_diff_pct": None,
			"changed_pixels_pct": None,
		}

	diff = ImageChops.difference(img_a, img_b)
	total_pixels = img_a.size[0] * img_a.size[1]

	# mean absolute difference across all channels, as a % of the 0-255 range
	stat = ImageStat.Stat(diff)
	mean_diff_pct = (sum(stat.mean) / (3 * 255)) * 100

	# % of pixels where any channel differs by more than a visibility threshold -
	# a pixel counts if ANY of its R/G/B channels changed by more than that,
	# combined via per-channel thresholding + lighter (i.e. logical OR)
	threshold = 16
	changed_mask = Image.eval(diff.getchannel("R"), lambda r: 0)
	for band in ("R", "G", "B"):
		channel = diff.getchannel(band).point(lambda v: 255 if v > threshold else 0)
		changed_mask = ImageChops.lighter(changed_mask, channel)
	changed_hist = changed_mask.histogram()
	changed = changed_hist[255] if len(changed_hist) > 255 else 0
	changed_pixels_pct = (changed / total_pixels) * 100

	# amplified visual diff so small differences are still visible
	amplified = diff.point(lambda p: min(255, p * 4))
	amplified.save(diff_out)

	if mean_diff_pct < 0.5:
		status = "MATCH"
	elif mean_diff_pct < 5.0:
		status = "MINOR_DIFF"
	else:
		status = "MAJOR_DIFF"

	return {
		"status": status,
		"detail": None,
		"mean_diff_pct": round(mean_diff_pct, 3),
		"changed_pixels_pct": round(changed_pixels_pct, 2),
	}


def main():
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--a", required=True, help="Screenshot directory for renderer A (e.g. rd-vanilla reference)")
	ap.add_argument("--b", required=True, help="Screenshot directory for renderer B (e.g. rd-vulkan under test)")
	ap.add_argument("--out", required=True, help="Directory to write diff images into")
	ap.add_argument("--scenes", default=None, help="scenes.json to enumerate expected scene ids (default: infer from --a directory contents)")
	args = ap.parse_args()

	dir_a = Path(args.a)
	dir_b = Path(args.b)
	out_dir = Path(args.out)
	out_dir.mkdir(parents=True, exist_ok=True)

	if args.scenes:
		scene_ids = load_scene_ids(args.scenes)
	else:
		scene_ids = sorted(p.stem for p in dir_a.glob("*.png"))

	if not scene_ids:
		print("No scenes to compare.", file=sys.stderr)
		sys.exit(1)

	results = []
	for scene_id in scene_ids:
		path_a = dir_a / f"{scene_id}.png"
		path_b = dir_b / f"{scene_id}.png"

		if not path_a.exists() or not path_b.exists():
			missing = path_a if not path_a.exists() else path_b
			print(f"[{scene_id}] SKIPPED - missing {missing}")
			results.append((scene_id, {"status": "MISSING", "detail": str(missing), "mean_diff_pct": None, "changed_pixels_pct": None}))
			continue

		diff_out = out_dir / f"{scene_id}.diff.png"
		result = compare_pair(path_a, path_b, diff_out)
		results.append((scene_id, result))

		if result["status"] == "SIZE_MISMATCH":
			print(f"[{scene_id}] SIZE_MISMATCH ({result['detail']})")
		else:
			print(f"[{scene_id}] {result['status']:10s} mean_diff={result['mean_diff_pct']}% "
				f"changed_pixels={result['changed_pixels_pct']}% -> {diff_out}")

	print("\n=== Summary ===")
	for scene_id, result in results:
		print(f"  {scene_id:25s} {result['status']}")

	non_match = [r for _, r in results if r["status"] not in ("MATCH",)]
	if non_match:
		sys.exit(1)


if __name__ == "__main__":
	main()
