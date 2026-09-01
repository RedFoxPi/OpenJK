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
from matplotlib.textpath import TextPath
from matplotlib.font_manager import FontProperties
from matplotlib.transforms import Affine2D
from matplotlib.tri import Triangulation
from mpl_toolkits.mplot3d import Axes3D  # noqa: F401  (registers the 3d projection)
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
from PIL import Image


# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

CUBE = 10.0            # half-extent of the 3D scene
DIGIT_HEIGHT = 7.0      # target height (in scene units) of an extruded digit
DIGIT_DEPTH = 2.2       # extrusion thickness of an extruded digit
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


_DIGIT_FONT = FontProperties(family="DejaVu Sans", weight="bold")


def build_digit_mesh(text, target_height=DIGIT_HEIGHT, depth=DIGIT_DEPTH):
    """Build a real extruded 3D mesh for a digit from its font outline.

    Local axes (before any object rotation is applied), chosen so the
    digit's rest pose faces the camera upright -- the same way the flat
    "Happy Birthday!" overlay always faces the viewer:
        x = left/right, y = extrusion depth (towards/away from camera),
        z = up.

    Returns a dict with 'faces' (N, 3-or-4, 3) local vertex arrays and a
    matching 'colors01' shade array (0..1) used to tint the base color.
    """
    raw_path = TextPath((0, 0), text, size=200, prop=_DIGIT_FONT)
    ext = raw_path.get_extents()
    cx = ext.x0 + ext.width / 2.0
    cy = ext.y0 + ext.height / 2.0
    scale = target_height / ext.height if ext.height > 0 else 1.0
    path = raw_path.transformed(Affine2D().translate(-cx, -cy).scale(scale))

    polygons = [p for p in path.to_polygons() if len(p) >= 3]
    pts = np.vstack([p[:-1] if np.allclose(p[0], p[-1]) else p for p in polygons])
    x, y = pts[:, 0], pts[:, 1]

    # matplotlib's compound-path nonzero-winding contains_points() does not
    # reliably detect counters/holes for every glyph (e.g. "0"), so decide
    # inside/outside ourselves with an even-odd count across each subpath
    # treated as its own simple polygon.
    from matplotlib.path import Path as MplPath
    subpaths = [MplPath(poly) for poly in polygons]

    tri = Triangulation(x, y)
    centroids = np.column_stack([x[tri.triangles].mean(axis=1), y[tri.triangles].mean(axis=1)])
    hit_counts = np.zeros(len(centroids), dtype=int)
    for sp in subpaths:
        hit_counts += sp.contains_points(centroids).astype(int)
    inside = (hit_counts % 2) == 1
    tri.set_mask(~inside)
    cap_tri_idx = tri.get_masked_triangles()

    faces = []       # each entry: list of (x, y_depth, z) local vertices
    shades = []       # matching flat-shade brightness in [0, 1]
    half = depth / 2.0

    for a, b, c in cap_tri_idx:
        faces.append([(x[a], half, y[a]), (x[b], half, y[b]), (x[c], half, y[c])])
        shades.append(1.0)  # front cap: brightest
        faces.append([(x[a], -half, y[a]), (x[c], -half, y[c]), (x[b], -half, y[b])])
        shades.append(0.4)  # back cap: darkest

    light_dir = np.array([0.6, 0.5])
    light_dir /= np.linalg.norm(light_dir)
    for poly in polygons:
        p = poly[:-1] if np.allclose(poly[0], poly[-1]) else poly
        n = len(p)
        for i in range(n):
            x0, y0 = p[i]
            x1, y1 = p[(i + 1) % n]
            edge = np.array([x1 - x0, y1 - y0])
            edge_len = np.linalg.norm(edge)
            if edge_len < 1e-6:
                continue
            normal = np.array([edge[1], -edge[0]]) / edge_len
            shade = 0.45 + 0.5 * max(0.0, float(np.dot(normal, light_dir)))
            faces.append([
                (x0, half, y0), (x1, half, y1),
                (x1, -half, y1), (x0, -half, y0),
            ])
            shades.append(shade)

    return {
        "faces": [np.array(f) for f in faces],
        "shades": np.array(shades),
    }


DIGIT_MESHES = {str(n): build_digit_mesh(str(n)) for n in range(11)}


def rotation_matrix(angle_x_deg, angle_y_deg, angle_z_deg):
    ax_, ay_, az_ = np.radians([angle_x_deg, angle_y_deg, angle_z_deg])
    rx = np.array([[1, 0, 0], [0, np.cos(ax_), -np.sin(ax_)], [0, np.sin(ax_), np.cos(ax_)]])
    ry = np.array([[np.cos(ay_), 0, np.sin(ay_)], [0, 1, 0], [-np.sin(ay_), 0, np.cos(ay_)]])
    rz = np.array([[np.cos(az_), -np.sin(az_), 0], [np.sin(az_), np.cos(az_), 0], [0, 0, 1]])
    return rz @ ry @ rx


def smoothstep(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3 - 2 * t)


def draw_countdown_number(ax, text, progress, color, spin):
    """Draw a real, extruded 3D digit that tumbles in and settles back to
    the same camera-facing orientation as "Happy Birthday!" at rest."""
    scale = ease_out_back(min(progress / 0.45, 1.0))
    alpha = ease_out_cubic(min(progress / 0.25, 1.0))
    if progress > 0.8:
        # fade out slightly right before the next number pops in
        alpha *= ease_out_cubic(1 - (progress - 0.8) / 0.2)
    if scale <= 0.02 or alpha <= 0.01:
        return

    mesh = DIGIT_MESHES[text]
    spin_t = smoothstep(progress)
    turns_x, turns_y, turns_z = spin
    rot = rotation_matrix(turns_x * 360 * spin_t, turns_y * 360 * spin_t, turns_z * 360 * spin_t)

    base_rgb = np.array(hex_to_rgb(color))
    poly_verts = []
    poly_colors = []
    for local_verts, shade in zip(mesh["faces"], mesh["shades"]):
        world = (rot @ (local_verts * scale).T).T
        poly_verts.append(world)
        poly_colors.append(tuple(base_rgb * shade))

    coll = Poly3DCollection(poly_verts, facecolors=poly_colors, edgecolors="none",
                             alpha=alpha, antialiased=False)
    ax.add_collection3d(coll)


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
    fontsize = 12 + 26 * t
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
    frames_per_number = int(fps * (0.55 if quick else 1.2))
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
    # Camera stays close to azim=90 (where a digit at rest/identity rotation
    # faces the viewer head-on, just like the flat "Happy Birthday!" overlay
    # always does) and only bobs gently -- the wildness comes from the
    # digit's own multi-axis spin, not from the camera drifting away.
    spin_rng = random.Random(7)
    countdown_frame = 0
    for idx, num in enumerate(numbers):
        color = COUNTDOWN_COLORS[idx % len(COUNTDOWN_COLORS)]
        # Wild multi-axis tumble: whole-turn counts so the digit lands back
        # on the same camera-facing orientation as "Happy Birthday!" both
        # when it pops in (progress=0) and right before it pops out (progress=1).
        spin = (
            spin_rng.choice([0, 0, 1]) * spin_rng.choice([-1, 1]),
            spin_rng.choice([0, 0, 1]) * spin_rng.choice([-1, 1]),
            1 * spin_rng.choice([-1, 1]),
        )
        for f in range(frames_per_number):
            progress = f / frames_per_number
            ax.cla()
            setup_axes(ax)
            countdown_frame += 1
            azim = 90 + 14 * np.sin(countdown_frame * 0.05)
            elev = 15 + 6 * np.sin(countdown_frame * 0.035)
            ax.view_init(elev=elev, azim=azim)
            draw_stars(ax, stars)
            draw_countdown_number(ax, str(num), progress, color, spin)
            capture()
    azim = 90.0

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
