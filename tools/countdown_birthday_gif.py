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


def ease_in_cubic(t):
    t = min(max(t, 0.0), 1.0)
    return t ** 3


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
    raw_path = TextPath((0, 0), text, size=400, prop=_DIGIT_FONT)
    ext = raw_path.get_extents()
    cx = ext.x0 + ext.width / 2.0
    cy = ext.y0 + ext.height / 2.0
    scale = target_height / ext.height if ext.height > 0 else 1.0
    path = raw_path.transformed(Affine2D().translate(-cx, -cy).scale(scale))

    def resample(poly, max_edge_len):
        """Insert extra points along long edges for a denser boundary/wall mesh."""
        out = []
        n = len(poly)
        for i in range(n):
            p0 = poly[i]
            p1 = poly[(i + 1) % n]
            seg_len = float(np.hypot(*(p1 - p0)))
            steps = max(1, int(np.ceil(seg_len / max_edge_len)))
            for s in range(steps):
                out.append(p0 + (p1 - p0) * (s / steps))
        return np.array(out)

    max_edge = target_height * 0.06
    polygons = [p[:-1] if np.allclose(p[0], p[-1]) else p
                for p in path.to_polygons() if len(p) >= 3]
    polygons = [resample(p, max_edge) for p in polygons]

    # matplotlib's compound-path nonzero-winding contains_points() does not
    # reliably detect counters/holes for every glyph (e.g. "0"), so decide
    # inside/outside ourselves with an even-odd count across each subpath
    # treated as its own simple polygon.
    from matplotlib.path import Path as MplPath
    subpaths = [MplPath(poly) for poly in polygons]

    def inside_shape(points):
        hit_counts = np.zeros(len(points), dtype=int)
        for sp in subpaths:
            hit_counts += sp.contains_points(points).astype(int)
        return (hit_counts % 2) == 1

    boundary_pts = np.vstack(polygons)

    # Add an interior grid of Steiner points so the cap faces are made of
    # many small triangles instead of a few large ones spanning the outline.
    xmin, ymin = boundary_pts.min(axis=0)
    xmax, ymax = boundary_pts.max(axis=0)
    grid_step = target_height * 0.06
    gx = np.arange(xmin + grid_step / 2, xmax, grid_step)
    gy = np.arange(ymin + grid_step / 2, ymax, grid_step)
    if len(gx) and len(gy):
        gxx, gyy = np.meshgrid(gx, gy)
        grid_pts = np.column_stack([gxx.ravel(), gyy.ravel()])
        grid_pts = grid_pts[inside_shape(grid_pts)]
    else:
        grid_pts = np.empty((0, 2))

    pts = np.vstack([boundary_pts, grid_pts]) if len(grid_pts) else boundary_pts
    x, y = pts[:, 0], pts[:, 1]

    tri = Triangulation(x, y)
    centroids = np.column_stack([x[tri.triangles].mean(axis=1), y[tri.triangles].mean(axis=1)])
    inside = inside_shape(centroids)
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

    face_arrays = [np.array(f) for f in faces]
    centroids = np.array([f.mean(axis=0) for f in face_arrays])

    # A fixed per-face random outward "shatter direction" (mostly along the
    # face's own direction from the digit's center, with some jitter so the
    # shards don't fly out in a perfectly uniform starburst).
    rng = np.random.default_rng(abs(hash(text)) % (2 ** 32))
    jitter = rng.normal(scale=0.4, size=centroids.shape)
    shatter_dir = centroids + jitter
    norms = np.linalg.norm(shatter_dir, axis=1, keepdims=True)
    norms[norms < 1e-6] = 1.0
    shatter_dir /= norms

    return {
        "faces": face_arrays,
        "shades": np.array(shades),
        "centroids": centroids,
        "shatter_dir": shatter_dir,
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


GATHER_IN_FRAC = 0.4    # fraction of a digit's hold time spent assembling from particles
SHATTER_OUT_FRAC = 0.4   # fraction spent shattering back into particles
SHATTER_DISTANCE = 8.0    # how far each shard/particle scatters radially
PARTICLE_SCALE = 0.12     # how small a face shrinks to when it's "just a particle"
DRIFT_DISTANCE_X = 17.0   # sideways drift while gathering/shattering -- past the frame edge


def draw_countdown_number(ax, text, progress, color, spin):
    """Draw a real, extruded 3D digit that assembles out of a swarm of tiny
    particles drifting in from off-screen left, which snap into full polygon
    shards and merge into the digit; it tumbles at rest in the same
    camera-facing orientation as "Happy Birthday!"; then shatters back into
    shards, shrinks into particles and scatters away to off-screen right."""
    if progress < GATHER_IN_FRAC:
        t = progress / GATHER_IN_FRAC
        ease = ease_out_cubic(t)   # 0 -> 1 as the digit finishes assembling
        travel = 1.0 - ease
        shard_scale = PARTICLE_SCALE + (1.0 - PARTICLE_SCALE) * ease
        alpha = 0.35 + 0.65 * ease
        drift_x = travel * -DRIFT_DISTANCE_X
    elif progress > 1 - SHATTER_OUT_FRAC:
        t = (progress - (1 - SHATTER_OUT_FRAC)) / SHATTER_OUT_FRAC
        ease = ease_in_cubic(t)     # 0 -> 1 as the digit finishes shattering away
        travel = ease
        shard_scale = 1.0 - (1.0 - PARTICLE_SCALE) * ease
        alpha = 1.0 - 0.65 * ease
        drift_x = travel * DRIFT_DISTANCE_X
    else:
        travel = 0.0
        shard_scale = 1.0
        alpha = 1.0
        drift_x = 0.0

    if alpha <= 0.01:
        return

    mesh = DIGIT_MESHES[text]
    spin_t = smoothstep(progress)
    turns_x, turns_y, turns_z = spin
    rot = rotation_matrix(turns_x * 360 * spin_t, turns_y * 360 * spin_t, turns_z * 360 * spin_t)
    drift = np.array([drift_x, 0.0, 0.0])

    base_rgb = np.array(hex_to_rgb(color))
    poly_verts = []
    poly_colors = []
    centroids = mesh["centroids"]
    shatter_dir = mesh["shatter_dir"]
    for local_verts, shade, centroid, direction in zip(
            mesh["faces"], mesh["shades"], centroids, shatter_dir):
        shard_offset = direction * (travel * SHATTER_DISTANCE)
        # scale each shard about its own centroid (not the digit's origin) so
        # it reads as a solid little chip flying around, not a sliver being
        # stretched from the center.
        local_shard = centroid + (local_verts - centroid) * shard_scale + shard_offset
        world = (rot @ local_shard.T).T + drift
        poly_verts.append(world)
        poly_colors.append(tuple(base_rgb * shade))

    coll = Poly3DCollection(poly_verts, facecolors=poly_colors, edgecolors="none",
                             alpha=alpha, antialiased=False)
    ax.add_collection3d(coll)


# --------------------------------------------------------------------------
# Fireworks
# --------------------------------------------------------------------------

class Firework:
    """One firework burst: a cloud of particles exploding from a center.

    Bigger bursts have a chance to "crackle" partway through -- spawning a
    handful of smaller secondary bursts from their own particles, like a
    willow/crackle firework -- for a more opulent, layered finale.
    """

    def __init__(self, center, n_particles=130, speed=6.0, color=None, can_crackle=True):
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
        self.can_crackle = can_crackle and n_particles >= 60
        self.crackled = False

    def alive(self):
        return self.age < self.lifetime

    def update(self, dt):
        self.age += dt
        gravity = np.array([0.0, 0.0, -3.2])
        self.vel += gravity * dt
        self.vel *= 0.98  # air drag
        self.pos += self.vel * dt

        children = []
        if self.can_crackle and not self.crackled and self.age > self.lifetime * 0.35:
            self.crackled = True
            if random.random() < 0.7:
                n = len(self.pos)
                for i in random.sample(range(n), min(3, n)):
                    children.append(Firework(self.pos[i], n_particles=26, speed=2.6,
                                              can_crackle=False))
        return children

    def draw(self, ax):
        life_frac = max(0.0, 1.0 - self.age / self.lifetime)
        alpha = life_frac ** 0.7
        size = 14 * life_frac + 2
        ax.scatter(self.pos[:, 0], self.pos[:, 1], self.pos[:, 2],
                   s=size, c=self.color, alpha=alpha, linewidths=0,
                   depthshade=False)


class Confetti:
    """A continuous flutter of colorful confetti squares, for party flair."""

    def __init__(self, n=170):
        self.rng = np.random.default_rng()
        self.n = n
        self.pos = np.column_stack([
            self.rng.uniform(-CUBE, CUBE, n),
            self.rng.uniform(-CUBE, CUBE, n),
            self.rng.uniform(-CUBE * 0.5, CUBE * 1.8, n),
        ])
        self.vel = np.column_stack([
            self.rng.uniform(-1.2, 1.2, n),
            self.rng.uniform(-1.2, 1.2, n),
            self.rng.uniform(-2.6, -1.0, n),
        ])
        self.flutter_phase = self.rng.uniform(0, 2 * np.pi, n)
        self.colors = np.array([hex_to_rgb(c) for c in self.rng.choice(FIREWORK_COLORS, n)])
        self.size_base = self.rng.uniform(8, 20, n)

    def update(self, dt):
        self.pos += self.vel * dt
        self.flutter_phase += dt * 3.0
        below = self.pos[:, 2] < -CUBE * 1.3
        n_reset = int(below.sum())
        if n_reset:
            self.pos[below, 2] = CUBE * 1.8
            self.pos[below, 0] = self.rng.uniform(-CUBE, CUBE, n_reset)
            self.pos[below, 1] = self.rng.uniform(-CUBE, CUBE, n_reset)

    def draw(self, ax):
        flutter = 0.5 + 0.5 * np.sin(self.flutter_phase)
        sizes = self.size_base * (0.5 + 0.7 * flutter)
        ax.scatter(self.pos[:, 0], self.pos[:, 1], self.pos[:, 2],
                   s=sizes, c=self.colors, alpha=0.85, linewidths=0,
                   depthshade=False, marker="s")


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
    frames_per_number = int(fps * (0.6 if quick else 1.4))
    numbers = list(range(10, -1, -1))
    firework_seconds = 2.0 if quick else 6.0
    firework_frames = int(fps * firework_seconds)
    end_hold_frames = int(fps * (0.8 if quick else 3.0))

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
    # Camera stays close to azim=-90 (where a digit at rest/identity rotation
    # faces the viewer head-on and reads correctly, just like the flat
    # "Happy Birthday!" overlay always does) and only bobs gently -- the
    # wildness comes from the digit's own multi-axis spin, not from the
    # camera drifting away.
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
            azim = -90 + 14 * np.sin(countdown_frame * 0.05)
            elev = 15 + 6 * np.sin(countdown_frame * 0.035)
            ax.view_init(elev=elev, azim=azim)
            draw_stars(ax, stars)
            draw_countdown_number(ax, str(num), progress, color, spin)
            capture()
    azim = -90.0

    # --- Firework + "Happy Birthday" phase ---
    fireworks = []
    confetti = Confetti()
    spawn_every = max(1, int(fps * 0.18))
    text_fly_start = int(fps * 0.4)
    text_fly_duration = int(fps * 1.2)
    grand_finale_fired = False
    dt = 0.06

    def random_burst_center(spread=0.5):
        return (
            random.uniform(-CUBE * spread, CUBE * spread),
            random.uniform(-CUBE * spread, CUBE * spread),
            random.uniform(-1.0, CUBE * 0.6),
        )

    def update_fireworks(dt):
        children = []
        for fw in fireworks:
            children.extend(fw.update(dt))
        fireworks.extend(children)
        fireworks[:] = [fw for fw in fireworks if fw.alive()]

    for f in range(firework_frames):
        ax.cla()
        setup_axes(ax)
        clear_birthday()
        azim += 0.8
        ax.view_init(elev=16, azim=azim)
        draw_stars(ax, stars)

        if f % spawn_every == 0:
            fireworks.append(Firework(random_burst_center()))
        if f % (spawn_every * 3) == 0:
            # a second, simultaneous burst for a fuller sky
            fireworks.append(Firework(random_burst_center()))

        if f >= text_fly_start:
            text_progress = min(1.0, (f - text_fly_start) / text_fly_duration)
            birthday_artists.append(draw_happy_birthday(fig, text_progress))
            if text_progress >= 1.0 and not grand_finale_fired:
                grand_finale_fired = True
                for _ in range(7):
                    fireworks.append(Firework(random_burst_center(spread=0.75), n_particles=160, speed=7.5))

        update_fireworks(dt)
        for fw in fireworks:
            fw.draw(ax)

        confetti.update(dt)
        confetti.draw(ax)

        capture()

    # --- Final hold: full-on party finale, confetti and fireworks keep going ---
    for f in range(end_hold_frames):
        ax.cla()
        setup_axes(ax)
        clear_birthday()
        azim += 0.5
        ax.view_init(elev=16, azim=azim)
        draw_stars(ax, stars)

        if f % spawn_every == 0:
            fireworks.append(Firework(random_burst_center(spread=0.45), n_particles=100))

        update_fireworks(dt)
        for fw in fireworks:
            fw.draw(ax)

        confetti.update(dt)
        confetti.draw(ax)

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
