#!/usr/bin/env python3
"""Render an animated GIF: a 3D countdown from 10 to 0 that ends in a
party finale with cute dancing ponies and falling confetti in the
background, and a "Happy Birthday!" banner flying into the foreground.

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
from matplotlib.lines import Line2D
from matplotlib.patches import Polygon as MplPolygon
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
COUNTDOWN_COLORS = ["#ff477e", "#00d9ff", "#ffb703", "#8ac926", "#b388ff"]
CONFETTI_COLORS = ["#ff5f6d", "#ffd93f", "#6dffb0", "#3fb6ff", "#ff9de2", "#c86dff", "#ffffff"]


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


SHATTER_DISTANCE = 8.0    # how far each shard/particle scatters radially
PARTICLE_SCALE = 0.12     # how small a face shrinks to when it's "just a particle"
DRIFT_DISTANCE_X = 17.0   # sideways drift while materializing/dissolving -- past the frame edge
TRANSIT_RIGHT_X = 13.0    # how far a decaying digit's particles sweep right
TRANSIT_LEFT_X = 10.0     # how far they then swing past center to the left before settling
BURST_RADIUS = 3.0        # extra outward kick applied at the mid-transition burst


def _shard_polys(mesh, shard_scale, radial_travel, drift, rot):
    """Build the rotated/offset/scaled world-space polygons for one mesh's
    faces. `radial_travel` scatters each shard outward along its own
    precomputed shatter direction; `drift` is a uniform world-space offset
    (e.g. sideways travel) added after rotation."""
    poly_verts = []
    for local_verts, centroid, direction in zip(
            mesh["faces"], mesh["centroids"], mesh["shatter_dir"]):
        shard_offset = direction * radial_travel
        local_shard = centroid + (local_verts - centroid) * shard_scale + shard_offset
        poly_verts.append((rot @ local_shard.T).T + drift)
    return poly_verts


def draw_countdown_number(ax, text, progress, color, spin):
    """Draw a real, extruded 3D digit fully assembled, tumbling in the same
    camera-facing orientation as "Happy Birthday!" at rest (progress=0/1)."""
    mesh = DIGIT_MESHES[text]
    spin_t = smoothstep(progress)
    turns_x, turns_y, turns_z = spin
    rot = rotation_matrix(turns_x * 360 * spin_t, turns_y * 360 * spin_t, turns_z * 360 * spin_t)

    base_rgb = np.array(hex_to_rgb(color))
    poly_verts = _shard_polys(mesh, 1.0, 0.0, np.zeros(3), rot)
    poly_colors = [tuple(base_rgb * shade) for shade in mesh["shades"]]

    coll = Poly3DCollection(poly_verts, facecolors=poly_colors, edgecolors="none",
                             alpha=1.0, antialiased=False)
    ax.add_collection3d(coll)


def draw_materialize(ax, text, t, color):
    """The very first digit has no predecessor to emerge from, so it just
    condenses out of a swarm of tiny particles drifting in from the left."""
    ease = ease_out_cubic(t)
    shard_scale = PARTICLE_SCALE + (1.0 - PARTICLE_SCALE) * ease
    radial_travel = (1.0 - ease) * SHATTER_DISTANCE
    alpha = 0.35 + 0.65 * ease
    drift = np.array([(1.0 - ease) * -DRIFT_DISTANCE_X, 0.0, 0.0])

    mesh = DIGIT_MESHES[text]
    base_rgb = np.array(hex_to_rgb(color))
    poly_verts = _shard_polys(mesh, shard_scale, radial_travel, drift, np.eye(3))
    poly_colors = [tuple(base_rgb * shade) for shade in mesh["shades"]]
    coll = Poly3DCollection(poly_verts, facecolors=poly_colors, edgecolors="none",
                             alpha=alpha, antialiased=False)
    ax.add_collection3d(coll)


def draw_dissolve(ax, text, t, color):
    """The final digit ("0") has no successor -- it just shatters into
    particles and scatters away to the right, handing off to the fireworks."""
    ease = ease_in_cubic(t)
    shard_scale = 1.0 - (1.0 - PARTICLE_SCALE) * ease
    radial_travel = ease * SHATTER_DISTANCE
    alpha = 1.0 - 0.65 * ease
    drift = np.array([ease * DRIFT_DISTANCE_X, 0.0, 0.0])

    mesh = DIGIT_MESHES[text]
    base_rgb = np.array(hex_to_rgb(color))
    poly_verts = _shard_polys(mesh, shard_scale, radial_travel, drift, np.eye(3))
    poly_colors = [tuple(base_rgb * shade) for shade in mesh["shades"]]
    coll = Poly3DCollection(poly_verts, facecolors=poly_colors, edgecolors="none",
                             alpha=alpha, antialiased=False)
    ax.add_collection3d(coll)


def draw_transition(ax, old_text, new_text, t, old_color, new_color):
    """The particles of the decaying digit sweep right, burst apart in a
    flurry of tiny drifting motes while their colour bleeds continuously
    from the old digit's colour to the new one's, then sweep back past
    center to the left and re-condense into the next digit's polygon shards."""
    # One continuous sideways sweep: right, then past center to the left,
    # settling back at x=0 exactly as the next digit's hold phase begins.
    if t <= 0.6:
        local_t = t / 0.6
        drift_x = TRANSIT_RIGHT_X * np.sin(np.pi * local_t)
    else:
        local_t = (t - 0.6) / 0.4
        drift_x = -TRANSIT_LEFT_X * np.sin(np.pi * local_t)
    drift = np.array([drift_x, 0.0, 0.0])

    # Shards are largest/most solid at the endpoints (still-forming old
    # digit, freshly-formed new digit) and shrink to tiny bursting particles
    # around the midpoint.
    shard_scale = 1.0 - (1.0 - PARTICLE_SCALE) * np.sin(np.pi * t)
    burst = BURST_RADIUS * np.sin(np.pi * t)
    alpha = 0.5 + 0.5 * (1.0 - abs(2.0 * t - 1.0))

    # A colour that bleeds continuously from the old digit's to the new
    # digit's across the whole sweep, independent of which mesh is active.
    blended_rgb = np.array(hex_to_rgb(old_color)) * (1.0 - t) + np.array(hex_to_rgb(new_color)) * t

    # Which digit's shape the shards borrow is switched at the midpoint,
    # where they are smallest/most particle-like, so the swap is disguised.
    mesh = DIGIT_MESHES[old_text] if t < 0.5 else DIGIT_MESHES[new_text]
    rot = rotation_matrix(0.0, 0.0, 90.0 * smoothstep(t))

    poly_verts = _shard_polys(mesh, shard_scale, burst, drift, rot)
    poly_colors = [tuple(blended_rgb * shade) for shade in mesh["shades"]]
    coll = Poly3DCollection(poly_verts, facecolors=poly_colors, edgecolors="none",
                             alpha=alpha, antialiased=False)
    ax.add_collection3d(coll)


# --------------------------------------------------------------------------
# Dancing horses
# --------------------------------------------------------------------------

HORSE_PALETTE = [
    ("#c9814f", "#6b4226", "#f4e6d7", "#ff5f9e"),   # chestnut, mane, snout, bow
    ("#dba463", "#7a4a2b", "#fbeedd", "#3fb6ff"),    # caramel, mane, snout, bow
    ("#8d7466", "#4a382f", "#efe2d3", "#ffd93f"),    # dove grey-brown, mane, snout, bow
    ("#5c4a42", "#241a16", "#e9dccb", "#6dffb0"),    # deep bay, mane, snout, bow
    ("#e8c39e", "#a9713f", "#fff6ea", "#c86dff"),    # palomino, mane, snout, bow
]
_HORSE_DARK = "#2e2018"
_HORSE_WHITE = "#ffffff"
_HORSE_BLUSH = "#ffb3c6"


def _ellipse_poly(cx, cy, rx, ry, rot_deg=0.0, n=14):
    t = np.linspace(0, 2 * np.pi, n, endpoint=False)
    pts = np.column_stack([rx * np.cos(t), ry * np.sin(t)])
    if rot_deg:
        r = np.radians(rot_deg)
        c, s = np.cos(r), np.sin(r)
        pts = pts @ np.array([[c, -s], [s, c]]).T
    return pts + np.array([cx, cy])


def _tri_poly(p1, p2, p3):
    return np.array([p1, p2, p3])


def _extrude_part(poly2d, thickness, w_layer=0.0):
    """Extrude a 2D (u, v) side-view polygon into a real 3D volume: a front
    cap, a back cap and a ring of side-wall quads, in local (u, w, v)
    coordinates -- u=forward/back, w=sideways thickness, v=height. This is
    what turns the horse from a flat cutout into an actual 3D shape that
    can be viewed, and rotated to face any direction, without ever going
    edge-on/invisible or needing a mirror-flip."""
    n = len(poly2d)
    half = thickness / 2.0
    u, v = poly2d[:, 0], poly2d[:, 1]
    front = np.column_stack([u, np.full(n, w_layer + half), v])
    back = np.column_stack([u, np.full(n, w_layer - half), v])
    faces = [(front, 1.0), (back, 0.45)]
    for i in range(n):
        j = (i + 1) % n
        quad = np.array([
            [u[i], w_layer + half, v[i]],
            [u[j], w_layer + half, v[j]],
            [u[j], w_layer - half, v[j]],
            [u[i], w_layer - half, v[i]],
        ])
        faces.append((quad, 0.72))
    return faces


_HORSE_CENTER_U = 5.0


def build_horse_template():
    """A cute, chibi-proportioned pony built from simple rounded shapes (a
    big soft body, a friendly head with a blush and a bow, poofy mane/tail
    tufts), each one a real extruded 3D volume rather than a flat cutout.
    Standing with feet at local v=0, roughly 10 units tall, centered on its
    own forward axis. Colors are given as palette keys, resolved per horse
    instance so each pony can wear its own coat/mane/bow colors.
    """
    part_defs = [
        (_ellipse_poly(4.6, 4.2, 2.5, 1.55), "body", 2.6, 0.0),
        (_ellipse_poly(6.6, 5.6, 1.05, 1.7, rot_deg=32), "body", 1.7, 0.0),
        (_ellipse_poly(7.75, 7.0, 1.0, 0.85), "body", 1.5, 0.0),
        (_ellipse_poly(8.65, 6.75, 0.55, 0.4), "snout", 1.0, 0.0),
        (_tri_poly((7.2, 7.7), (7.0, 8.75), (7.55, 7.85)), "body", 0.35, 0.0),
        (_tri_poly((7.75, 7.75), (7.95, 8.85), (8.15, 7.85)), "body", 0.35, 0.0),
        (_tri_poly((7.28, 7.75), (7.12, 8.5), (7.46, 7.9)), "blush", 0.2, 0.7),
        (_tri_poly((7.82, 7.8), (7.98, 8.55), (8.1, 7.9)), "blush", 0.2, 0.7),
        (_ellipse_poly(7.95, 7.15, 0.13, 0.13), "dark", 0.25, 0.75),
        (_ellipse_poly(8.0, 7.2, 0.045, 0.045), "white", 0.28, 0.85),
        (_ellipse_poly(8.95, 6.65, 0.08, 0.06), "dark", 0.25, 0.5),
        (_ellipse_poly(8.35, 6.55, 0.22, 0.16), "blush", 0.2, 0.7),
        (_tri_poly((7.35, 8.15), (6.98, 8.42), (7.35, 8.28)), "bow", 0.4, 0.0),
        (_tri_poly((7.55, 8.15), (7.92, 8.42), (7.55, 8.28)), "bow", 0.4, 0.0),
        (_ellipse_poly(7.45, 8.22, 0.1, 0.1), "bow", 0.42, 0.0),
    ]
    for mx, my, mr in [(6.05, 6.9, 0.42), (6.45, 7.25, 0.4), (6.9, 7.55, 0.36), (7.25, 7.75, 0.3)]:
        part_defs.append((_ellipse_poly(mx, my, mr, mr), "mane", 1.0, 0.0))

    rigid = []
    for poly, color_key, thickness, w_layer in part_defs:
        centered = poly - np.array([_HORSE_CENTER_U, 0.0])
        for face, shade in _extrude_part(centered, thickness, w_layer):
            rigid.append((face, color_key, shade))

    leg_faces = _extrude_part(_ellipse_poly(0, -1.3, 0.34, 1.3), 0.85)
    hoof_faces = _extrude_part(_ellipse_poly(0, -2.45, 0.4, 0.22), 0.9)

    def leg(pivot, side):
        pu, pv = pivot[0] - _HORSE_CENTER_U, pivot[1]
        parts = [(face, "body", shade) for face, shade in leg_faces]
        parts += [(face, "dark", shade) for face, shade in hoof_faces]
        return {"pivot": (pu, pv), "side": side, "parts": parts}

    legs = [
        leg((3.3, 2.75), "back"),
        leg((3.9, 2.7), "back"),
        leg((5.7, 2.85), "front"),
        leg((6.3, 2.8), "front"),
    ]

    tail_pivot = (2.2 - _HORSE_CENTER_U, 4.6)
    tail_parts = []
    for cx, cy, r, thick in [(0, 0, 0.5, 0.9), (-0.5, -0.7, 0.42, 0.85), (-0.85, -1.5, 0.34, 0.8)]:
        for face, shade in _extrude_part(_ellipse_poly(cx, cy, r, r), thick):
            tail_parts.append((face, "mane", shade))
    tail = {"pivot": tail_pivot, "parts": tail_parts}

    return {"rigid": rigid, "legs": legs, "tail": tail}


HORSE_TEMPLATE = build_horse_template()


def _rotate_uv(pts_xyz, angle_deg, pivot_uv):
    """Rotate the (u, v) = (x, z) components of local (u, w, v) points
    around pivot_uv, leaving the sideways w (y) component untouched --
    used for the local leg-swing/tail-swish animation."""
    r = np.radians(angle_deg)
    c, s = np.cos(r), np.sin(r)
    pu, pv = pivot_uv
    u = pts_xyz[:, 0] - pu
    v = pts_xyz[:, 2] - pv
    out = pts_xyz.copy()
    out[:, 0] = u * c - v * s + pu
    out[:, 2] = u * s + v * c + pv
    return out


class DancingHorse:
    """A cute background pony, built as a real extruded 3D model, that
    rides around a circular track which recedes into depth (a circle in
    the ground/depth plane, not one lying flat against the screen). It
    turns to face its actual direction of travel with a real yaw rotation
    -- never mirror-flipping -- while bouncing, swinging its legs in a
    little trot and swishing its tail. Purely decorative background flair,
    drawn behind the confetti and "Happy Birthday!" text."""

    def __init__(self, theta0, orbit_center, orbit_radius, orbit_speed,
                 z_base, scale, phase, palette, dance_speed=2.2):
        self.theta0 = theta0
        self.orbit_cx, self.orbit_cy = orbit_center
        self.orbit_radius = orbit_radius
        self.orbit_speed = orbit_speed
        self.z_base = z_base
        self.scale = scale
        self.phase = phase
        self.dance_speed = dance_speed
        body, mane, snout, bow = palette
        self.colors = {
            "body": body, "mane": mane, "snout": snout, "bow": bow,
            "dark": _HORSE_DARK, "white": _HORSE_WHITE, "blush": _HORSE_BLUSH,
        }

    def collect(self, t, near_y, far_y):
        theta = self.theta0 + t * self.orbit_speed
        x_center = self.orbit_cx + self.orbit_radius * np.cos(theta)
        y_center = self.orbit_cy + self.orbit_radius * np.sin(theta)

        # Nearer horses (closer to the camera) are drawn bigger, farther
        # ones smaller, so the circular track visibly recedes into depth.
        depth_t = np.clip((y_center - far_y) / (near_y - far_y), 0.0, 1.0)
        eff_scale = self.scale * (0.5 + 0.75 * depth_t)

        # A real yaw rotation to face the current direction of travel
        # around the circle -- the horse is an actual 3D volume now, so it
        # turns smoothly through every heading instead of mirror-flipping.
        vx = -np.sin(theta) * self.orbit_speed
        vy = np.cos(theta) * self.orbit_speed
        heading_deg = np.degrees(np.arctan2(vy, vx))
        rot = rotation_matrix(0.0, 0.0, heading_deg)

        bounce = abs(np.sin(t * self.dance_speed + self.phase)) * 0.9
        swing_deg = np.sin(t * self.dance_speed * 2.0 + self.phase) * 26.0
        tail_deg = np.sin(t * self.dance_speed * 1.6 + self.phase + 1.0) * 20.0
        center = np.array([x_center, y_center, self.z_base])

        def place(local_xyz):
            world = (rot @ (local_xyz * eff_scale).T).T + center
            world[:, 2] += bounce * eff_scale
            return world

        verts, colors = [], []
        for local_face, color_key, shade in HORSE_TEMPLATE["rigid"]:
            verts.append(place(local_face))
            colors.append(tuple(np.array(hex_to_rgb(self.colors[color_key])) * shade))

        for leg in HORSE_TEMPLATE["legs"]:
            angle = swing_deg if leg["side"] == "front" else -swing_deg
            pivot_offset = np.array([leg["pivot"][0], 0.0, leg["pivot"][1]])
            for local_face, color_key, shade in leg["parts"]:
                # Parts are stored pivot-relative (hip/shoulder at local
                # origin), so swing around (0, 0) first, then move the
                # whole swung leg to its attachment point on the body.
                swung = _rotate_uv(local_face, angle, (0.0, 0.0)) + pivot_offset
                verts.append(place(swung))
                colors.append(tuple(np.array(hex_to_rgb(self.colors[color_key])) * shade))

        tail = HORSE_TEMPLATE["tail"]
        tail_offset = np.array([tail["pivot"][0], 0.0, tail["pivot"][1]])
        for local_face, color_key, shade in tail["parts"]:
            swung = _rotate_uv(local_face, tail_deg, (0.0, 0.0)) + tail_offset
            verts.append(place(swung))
            colors.append(tuple(np.array(hex_to_rgb(self.colors[color_key])) * shade))

        return verts, colors


def draw_horses(ax, horses, t, near_y, far_y):
    # Draw farthest-first so nearer horses correctly overlap farther ones.
    def depth_of(h):
        theta = h.theta0 + t * h.orbit_speed
        y_center = h.orbit_cy + h.orbit_radius * np.sin(theta)
        return (y_center - far_y) / (near_y - far_y)

    ordered = sorted(horses, key=depth_of)
    verts, colors = [], []
    for horse in ordered:
        hv, hc = horse.collect(t, near_y, far_y)
        verts.extend(hv)
        colors.extend(hc)
    coll = Poly3DCollection(verts, facecolors=colors, edgecolors="none", antialiased=False)
    ax.add_collection3d(coll)


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
        self.colors = np.array([hex_to_rgb(c) for c in self.rng.choice(CONFETTI_COLORS, n)])
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
# Ivy vines growing in above the banner
# --------------------------------------------------------------------------

IVY_STEM_COLOR = "#3f7d3f"
IVY_LEAF_COLORS = ["#2e5e2e", "#4a8f4a", "#6fbf6f"]


def _vine_path(start_x, start_y, direction, height, drift, seed, n=50, wobble=0.018):
    """A wandering, slightly curling tendril path in figure coordinates,
    parameterized by s in [0, 1] from root to tip."""
    rng = np.random.default_rng(seed)
    s = np.linspace(0.0, 1.0, n)
    freq1, freq2 = rng.uniform(2.2, 3.4), rng.uniform(5.0, 7.5)
    ph1, ph2 = rng.uniform(0, 2 * np.pi, 2)
    curl = rng.uniform(0.7, 1.3)
    x = (start_x + direction * s * drift
         + wobble * np.sin(freq1 * np.pi * s + ph1)
         + 0.4 * wobble * np.sin(freq2 * np.pi * s + ph2)
         + direction * 0.05 * curl * s ** 2 * np.sin(3 * np.pi * s + ph1))
    y = start_y + s * height + 0.03 * s * np.sin(freq1 * np.pi * s * 0.5 + ph2)
    return x, y


def build_ivy_vines():
    """A handful of ivy tendrils creeping in from the left and right, up
    and over the top of the "Happy Birthday!" banner."""
    configs = [
        dict(start=(0.055, 0.565), direction=1, height=0.27, drift=0.34, seed=1),
        dict(start=(0.10, 0.545), direction=1, height=0.20, drift=0.22, seed=2),
        dict(start=(0.945, 0.565), direction=-1, height=0.27, drift=0.34, seed=3),
        dict(start=(0.90, 0.545), direction=-1, height=0.20, drift=0.22, seed=4),
    ]
    vines = []
    for cfg in configs:
        x, y = _vine_path(cfg["start"][0], cfg["start"][1], cfg["direction"],
                           cfg["height"], cfg["drift"], cfg["seed"])
        n = len(x)
        rng = np.random.default_rng(cfg["seed"] + 100)
        leaves = [(i, 1 if k % 2 == 0 else -1, rng.uniform(0.85, 1.15))
                  for k, i in enumerate(range(4, n - 2, 5))]
        vines.append({"x": x, "y": y, "leaves": leaves})
    return vines


IVY_VINES = build_ivy_vines()
_IVY_LEAF_LOCAL = np.array([[0.0, 0.0], [0.32, 0.55], [0.0, 1.15], [-0.32, 0.55]])


def draw_ivy(fig, vines, growth_by_vine):
    """Draw each vine up to its current growth fraction (0..1), with small
    leaves popping in as the tip grows past them -- an organic "growing in"
    reveal rather than a static overlay."""
    artists = []
    for vine, g in zip(vines, growth_by_vine):
        g = max(0.0, min(1.0, g))
        n = len(vine["x"])
        visible = int(g * (n - 1)) + 1
        if visible >= 2:
            line = Line2D(vine["x"][:visible], vine["y"][:visible], color=IVY_STEM_COLOR,
                          linewidth=2.2, solid_capstyle="round", transform=fig.transFigure,
                          zorder=5, alpha=0.95)
            fig.add_artist(line)
            artists.append(line)

        for i, side, size_mult in vine["leaves"]:
            s_leaf = i / (n - 1)
            if s_leaf > g:
                continue
            pop = ease_out_back(min(1.0, (g - s_leaf) / 0.05), overshoot=1.6)
            if pop <= 0.02:
                continue
            i0, i1 = max(0, i - 1), min(n - 1, i + 1)
            dx, dy = vine["x"][i1] - vine["x"][i0], vine["y"][i1] - vine["y"][i0]
            angle = np.degrees(np.arctan2(dy, dx)) + side * 62 - 90
            r = np.radians(angle)
            c, s_ = np.cos(r), np.sin(r)
            rot = _IVY_LEAF_LOCAL @ np.array([[c, -s_], [s_, c]]).T
            leaf_scale = 0.03 * size_mult * pop
            pts = rot * leaf_scale + np.array([vine["x"][i], vine["y"][i]])
            color = IVY_LEAF_COLORS[i % len(IVY_LEAF_COLORS)]
            poly = MplPolygon(pts, closed=True, facecolor=color, edgecolor="none",
                               transform=fig.transFigure, zorder=5, alpha=0.95)
            fig.add_artist(poly)
            artists.append(poly)

    return artists


# --------------------------------------------------------------------------
# Main render loop
# --------------------------------------------------------------------------

def generate(output="countdown_birthday.gif", fps=20, quick=False):
    hold_frames = int(fps * (0.5 if quick else 1.0))
    transition_frames = int(fps * (0.45 if quick else 1.0))
    edge_frames = int(fps * (0.3 if quick else 0.6))
    numbers = list(range(10, -1, -1))
    finale_seconds = 2.0 if quick else 6.0
    finale_frames = int(fps * finale_seconds)
    end_hold_frames = int(fps * (0.8 if quick else 3.0))

    fig = plt.figure(figsize=(6, 4.5), dpi=70 if quick else 90)
    fig.patch.set_facecolor("black")
    ax = fig.add_subplot(111, projection="3d")

    stars = make_stars()
    frames = []
    azim = 0.0
    birthday_artists = []
    vine_artists = []

    def clear_birthday():
        for artist in birthday_artists:
            artist.remove()
        birthday_artists.clear()

    def clear_vines():
        for artist in vine_artists:
            artist.remove()
        vine_artists.clear()

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

    def next_camera():
        nonlocal countdown_frame
        countdown_frame += 1
        azim = -90 + 14 * np.sin(countdown_frame * 0.05)
        elev = 15 + 6 * np.sin(countdown_frame * 0.035)
        ax.cla()
        setup_axes(ax)
        ax.view_init(elev=elev, azim=azim)
        draw_stars(ax, stars)

    for idx, num in enumerate(numbers):
        text = str(num)
        color = COUNTDOWN_COLORS[idx % len(COUNTDOWN_COLORS)]
        # Wild multi-axis tumble: whole-turn counts so the digit lands back
        # on the same camera-facing orientation as "Happy Birthday!" both
        # when it pops in (progress=0) and right before it pops out (progress=1).
        spin = (
            spin_rng.choice([0, 0, 1]) * spin_rng.choice([-1, 1]),
            spin_rng.choice([0, 0, 1]) * spin_rng.choice([-1, 1]),
            1 * spin_rng.choice([-1, 1]),
        )

        if idx == 0:
            # The first digit has no predecessor -- it condenses out of
            # drifting particles instead of taking part in a transition.
            for f in range(edge_frames):
                next_camera()
                draw_materialize(ax, text, f / edge_frames, color)
                capture()

        for f in range(hold_frames):
            next_camera()
            draw_countdown_number(ax, text, f / hold_frames, color, spin)
            capture()

        if idx == len(numbers) - 1:
            # The last digit ("0") has no successor -- it shatters away on
            # its own, handing off to the fireworks.
            for f in range(edge_frames):
                next_camera()
                draw_dissolve(ax, text, f / edge_frames, color)
                capture()
        else:
            next_color = COUNTDOWN_COLORS[(idx + 1) % len(COUNTDOWN_COLORS)]
            next_text = str(numbers[idx + 1])
            for f in range(transition_frames):
                next_camera()
                draw_transition(ax, text, next_text, f / transition_frames, color, next_color)
                capture()
    azim = -90.0

    # --- Dancing horses + "Happy Birthday" finale ---
    confetti = Confetti()
    text_fly_start = int(fps * 0.4)
    text_fly_duration = int(fps * 1.2)
    dt = 0.06

    # The horses ride a circular track laid out in the ground/depth plane
    # (not flat against the screen), so the circle visibly recedes into
    # depth: half the ride is out front near the camera, half recedes into
    # the background, looping continuously.
    ORBIT_CENTER = (0.0, -CUBE * 0.9)
    ORBIT_RADIUS = CUBE * 0.55
    # Higher world-Y is closer to the camera at azim=-90 (the same
    # convention the digits use), so the near point of the orbit is the
    # +radius side and the far point is the -radius side.
    ORBIT_NEAR_Y = ORBIT_CENTER[1] + ORBIT_RADIUS
    ORBIT_FAR_Y = ORBIT_CENTER[1] - ORBIT_RADIUS
    ORBIT_SPEED = 0.7

    horse_rng = random.Random(11)
    n_horses = 6
    horses = [
        DancingHorse(
            theta0=i * (2 * np.pi / n_horses) + horse_rng.uniform(-0.15, 0.15),
            orbit_center=ORBIT_CENTER,
            orbit_radius=ORBIT_RADIUS,
            orbit_speed=ORBIT_SPEED,
            z_base=-CUBE * 1.35 + horse_rng.uniform(-0.3, 0.3),
            scale=horse_rng.uniform(0.68, 0.82),
            phase=horse_rng.uniform(0, 2 * np.pi),
            palette=HORSE_PALETTE[i % len(HORSE_PALETTE)],
            dance_speed=horse_rng.uniform(2.4, 3.0),
        )
        for i in range(n_horses)
    ]

    finale_time = 0.0

    # Ivy grows in above the banner, staggered per vine, starting a bit
    # after the text arrives and then staying put once fully grown.
    IVY_START = text_fly_start + int(fps * 0.5)
    IVY_STAGGER = int(fps * 0.22)
    IVY_DURATION = int(fps * 1.5)

    def ivy_growth(global_frame):
        return [max(0.0, min(1.0, (global_frame - (IVY_START + k * IVY_STAGGER)) / IVY_DURATION))
                for k in range(len(IVY_VINES))]

    for f in range(finale_frames):
        ax.cla()
        setup_axes(ax)
        clear_birthday()
        clear_vines()
        azim = -90 + 10 * np.sin(f * 0.03)
        elev = 15 + 5 * np.sin(f * 0.02)
        ax.view_init(elev=elev, azim=azim)
        draw_stars(ax, stars)

        finale_time += dt
        draw_horses(ax, horses, finale_time, ORBIT_NEAR_Y, ORBIT_FAR_Y)

        if f >= text_fly_start:
            text_progress = min(1.0, (f - text_fly_start) / text_fly_duration)
            birthday_artists.append(draw_happy_birthday(fig, text_progress))
            vine_artists.extend(draw_ivy(fig, IVY_VINES, ivy_growth(f)))

        confetti.update(dt)
        confetti.draw(ax)

        capture()

    # --- Final hold: the ponies keep dancing, confetti keeps falling ---
    for f in range(end_hold_frames):
        ax.cla()
        setup_axes(ax)
        clear_birthday()
        clear_vines()
        azim = -90 + 10 * np.sin((finale_frames + f) * 0.03)
        elev = 15 + 5 * np.sin((finale_frames + f) * 0.02)
        ax.view_init(elev=elev, azim=azim)
        draw_stars(ax, stars)

        finale_time += dt
        draw_horses(ax, horses, finale_time, ORBIT_NEAR_Y, ORBIT_FAR_Y)

        confetti.update(dt)
        confetti.draw(ax)

        birthday_artists.append(draw_happy_birthday(fig, 1.0))
        vine_artists.extend(draw_ivy(fig, IVY_VINES, ivy_growth(finale_frames + f)))
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
