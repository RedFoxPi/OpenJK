#!/usr/bin/env python3
"""Render an animated GIF: a 3D countdown from 10 to 0 that ends in an
opulent particle firework, with a "Happy Birthday!" banner flying into
the foreground.

Dependencies:
    pip install matplotlib numpy pillow

Usage:
    python3 countdown_birthday_gif.py [--out countdown_birthday.gif]
                                       [--fps 20] [--quick]

--quick renders a short, low-frame-count preview for fast iteration;
omit it for the full-length animation.
"""

import argparse
import random

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patheffects as pe
from mpl_toolkits.mplot3d import Axes3D  # noqa: F401  (registers the 3d projection)
from PIL import Image


# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

CUBE = 10.0            # half-extent of the 3D scene
NUM_LAYERS = 10         # depth layers used to "extrude" the countdown digits
STAR_COUNT = 140

GOLD = "#FFD54A"
COUNTDOWN_COLORS = ["#3fb6ff", "#ff5f6d", "#ffd93f", "#6dffb0", "#c86dff"]
FIREWORK_COLORS = ["#ff5f6d", "#ffd93f", "#6dffb0", "#3fb6ff", "#ff9de2", "#c86dff", "#ffffff"]


def ease_out_back(t, overshoot=1.7):
    """Overshoot easing used for the pop-in / fly-in animations."""
    t = min(max(t, 0.0), 1.0)
    c1 = overshoot
    c3 = c1 + 1
    return 1 + c3 * (t - 1) ** 3 + c1 * (t - 1) ** 2


def ease_out_cubic(t):
    t = min(max(t, 0.0), 1.0)
    return 1 - (1 - t) ** 3


def hex_to_rgb(color):
    color = color.lstrip("#")
    return tuple(int(color[i:i + 2], 16) / 255 for i in (0, 2, 4))


# --------------------------------------------------------------------------
# Scene setup
# --------------------------------------------------------------------------

def make_stars(n=STAR_COUNT, spread=CUBE * 1.4):
    rng = np.random.default_rng(42)
    pts = rng.uniform(-spread, spread, size=(n, 3))
    return pts


def draw_stars(ax, stars):
    ax.scatter(stars[:, 0], stars[:, 1], stars[:, 2], s=3, c="white",
               alpha=0.35, linewidths=0, depthshade=False)


def setup_axes(ax):
    ax.set_facecolor("black")
    ax.set_xlim(-CUBE, CUBE)
    ax.set_ylim(-CUBE, CUBE)
    ax.set_zlim(-CUBE, CUBE)
    ax.set_box_aspect((1, 1, 1))
    ax.set_axis_off()
    try:
        ax.xaxis.pane.set_alpha(0)
        ax.yaxis.pane.set_alpha(0)
        ax.zaxis.pane.set_alpha(0)
    except Exception:
        pass


def draw_countdown_number(ax, text, progress, color):
    """Draw a popping-in, layered (pseudo-extruded) 3D number."""
    scale = ease_out_back(min(progress / 0.45, 1.0))
    alpha = ease_out_cubic(min(progress / 0.25, 1.0))
    if progress > 0.8:
        # fade out slightly right before the next number pops in
        alpha *= ease_out_cubic(1 - (progress - 0.8) / 0.2)

    fontsize = 150 * scale
    if fontsize <= 1 or alpha <= 0.01:
        return

    base_rgb = hex_to_rgb(color)
    for i in range(NUM_LAYERS):
        depth_t = i / (NUM_LAYERS - 1)
        z = -3.0 + depth_t * 3.0
        shade = 0.35 + 0.65 * depth_t  # back layers darker -> gives the extruded look
        rgb = tuple(c * shade for c in base_rgb)
        layer_alpha = alpha * (0.5 if depth_t < 1.0 else 1.0)
        ax.text(0, 0, z, text, color=rgb, alpha=layer_alpha, fontsize=fontsize,
                 ha="center", va="center", zdir="z", fontweight="bold",
                 path_effects=[pe.withStroke(linewidth=max(fontsize * 0.05, 1), foreground="black")])


# --------------------------------------------------------------------------
# Fireworks
# --------------------------------------------------------------------------

class Firework:
    """One firework burst: a cloud of particles exploding from a center."""

    def __init__(self, center, n_particles=90, speed=6.0, color=None):
        self.center = np.array(center, dtype=float)
        rng = np.random.default_rng()
        dirs = rng.normal(size=(n_particles, 3))
        dirs /= np.linalg.norm(dirs, axis=1, keepdims=True)
        speeds = rng.uniform(0.4, 1.0, size=n_particles) * speed
        self.pos = np.tile(self.center, (n_particles, 1))
        self.vel = dirs * speeds[:, None]
        self.age = 0.0
        self.lifetime = rng.uniform(1.6, 2.4)
        self.color = color or random.choice(FIREWORK_COLORS)

    def alive(self):
        return self.age < self.lifetime

    def update(self, dt):
        self.age += dt
        gravity = np.array([0.0, 0.0, -3.2])
        self.vel += gravity * dt
        self.vel *= 0.98  # air drag
        self.pos += self.vel * dt

    def draw(self, ax):
        life_frac = max(0.0, 1.0 - self.age / self.lifetime)
        alpha = life_frac ** 0.7
        size = 14 * life_frac + 2
        ax.scatter(self.pos[:, 0], self.pos[:, 1], self.pos[:, 2],
                   s=size, c=self.color, alpha=alpha, linewidths=0,
                   depthshade=False)


def draw_happy_birthday(fig, progress):
    """Overlay 'Happy Birthday!' flying from the background into the foreground."""
    t = ease_out_back(progress, overshoot=1.2)
    fontsize = 14 + 46 * t
    alpha = ease_out_cubic(min(progress / 0.5, 1.0))
    y = 0.12 + 0.38 * t
    return fig.text(0.5, y, "Happy Birthday!", ha="center", va="center",
                     fontsize=fontsize, fontweight="bold", color=GOLD, alpha=alpha,
                     family="DejaVu Sans",
                     path_effects=[pe.withStroke(linewidth=max(fontsize * 0.08, 1), foreground="#7a1d1d")])


# --------------------------------------------------------------------------
# Main render loop
# --------------------------------------------------------------------------

def generate(output="countdown_birthday.gif", fps=20, quick=False):
    frames_per_number = int(fps * (0.35 if quick else 0.6))
    numbers = list(range(10, -1, -1))
    firework_seconds = 1.5 if quick else 4.0
    firework_frames = int(fps * firework_seconds)
    end_hold_frames = int(fps * (0.5 if quick else 1.5))

    fig = plt.figure(figsize=(6, 4.5), dpi=70 if quick else 90)
    fig.patch.set_facecolor("black")
    ax = fig.add_subplot(111, projection="3d")

    stars = make_stars()
    frames = []
    azim = 0.0
    birthday_artists = []

    def clear_birthday():
        for artist in birthday_artists:
            artist.remove()
        birthday_artists.clear()

    def capture():
        fig.canvas.draw()
        buf = np.asarray(fig.canvas.buffer_rgba())
        frames.append(Image.fromarray(buf).convert("RGB"))

    # --- Countdown phase ---
    for idx, num in enumerate(numbers):
        color = COUNTDOWN_COLORS[idx % len(COUNTDOWN_COLORS)]
        for f in range(frames_per_number):
            progress = f / frames_per_number
            ax.cla()
            setup_axes(ax)
            azim += 1.1
            ax.view_init(elev=18, azim=azim)
            draw_stars(ax, stars)
            draw_countdown_number(ax, str(num), progress, color)
            capture()

    # --- Firework + "Happy Birthday" phase ---
    fireworks = []
    spawn_every = max(1, int(fps * 0.35))
    text_fly_start = int(fps * 0.4)
    text_fly_duration = int(fps * 1.2)
    dt = 0.06

    for f in range(firework_frames):
        ax.cla()
        setup_axes(ax)
        clear_birthday()
        azim += 0.8
        ax.view_init(elev=16, azim=azim)
        draw_stars(ax, stars)

        if f % spawn_every == 0:
            center = (
                random.uniform(-CUBE * 0.5, CUBE * 0.5),
                random.uniform(-CUBE * 0.5, CUBE * 0.5),
                random.uniform(-1.0, CUBE * 0.6),
            )
            fireworks.append(Firework(center))

        for fw in fireworks:
            fw.update(dt)
        fireworks = [fw for fw in fireworks if fw.alive()]
        for fw in fireworks:
            fw.draw(ax)

        if f >= text_fly_start:
            text_progress = min(1.0, (f - text_fly_start) / text_fly_duration)
            birthday_artists.append(draw_happy_birthday(fig, text_progress))

        capture()

    # --- Final hold: keep the finale look, fireworks keep sparkling gently ---
    for f in range(end_hold_frames):
        ax.cla()
        setup_axes(ax)
        clear_birthday()
        azim += 0.5
        ax.view_init(elev=16, azim=azim)
        draw_stars(ax, stars)

        if f % (spawn_every * 2) == 0:
            center = (
                random.uniform(-CUBE * 0.4, CUBE * 0.4),
                random.uniform(-CUBE * 0.4, CUBE * 0.4),
                random.uniform(0.0, CUBE * 0.5),
            )
            fireworks.append(Firework(center, n_particles=60))

        for fw in fireworks:
            fw.update(dt)
        fireworks = [fw for fw in fireworks if fw.alive()]
        for fw in fireworks:
            fw.draw(ax)

        birthday_artists.append(draw_happy_birthday(fig, 1.0))
        capture()

    plt.close(fig)

    duration_ms = int(1000 / fps)
    frames[0].save(output, save_all=True, append_images=frames[1:],
                    duration=duration_ms, loop=0, optimize=False)
    print(f"Saved {len(frames)} frames -> {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="countdown_birthday.gif",
                         help="Output GIF file path.")
    parser.add_argument("--fps", type=int, default=20, help="Frames per second.")
    parser.add_argument("--quick", action="store_true",
                         help="Render a fast, low-frame-count preview for testing.")
    args = parser.parse_args()
    generate(args.out, fps=args.fps, quick=args.quick)


if __name__ == "__main__":
    main()
