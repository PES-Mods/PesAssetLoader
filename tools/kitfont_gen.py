"""
kitfont_gen.py -- make 4-bit kit number and name font textures (PNG) for PES5/PES6.

    python kitfont_gen.py

Numbers: 10 digit pictures (PNG, any size; e.g. 0.png .. 9.png or number_0.png .. number_9.png) are placed into
the game's digit cells and reduced to 16 colors (index 0 is transparent: it's the background of the player's
number texture). Three results share the same indices: the number font (shirt numbers) and the two shorts
number palettes - recoloring changes only the palette, never the indices, as the game needs.
Digit pictures can be templates: red / green / blue = the share of the primary / secondary / third color.

Letters (names): A-Z and '.' from a .ttf/.otf font, with a fill color and an optional border. The game draws
names from this texture as shading levels, so its indices have a fixed meaning: 0 - transparent, 1..11 - fill
with increasing opacity, 12..15 - opaque fill blended into the border (25%, 50%, 75%, 100% border).

The game takes every character from a fixed rectangle (its own glyph table: number font 256x128, name font
256x64); the textures are made in a multiple of that size (4x by default) with the same layout.

Saved files: the texture as a 4-bit PNG (palette with alpha 0..255), the palette as an 8x2 32-bit PNG.
"""

import os
import re
import sys

import numpy as np
from scipy import ndimage
from PIL import Image, ImageDraw, ImageFont
from PyQt5.QtCore import QObject, QPointF, QRectF, QSettings, QSize, Qt, QTimer, pyqtSignal
from PyQt5.QtGui import QBrush, QColor, QIcon, QImage, QPainter, QPainterPath, QPalette, QPen, QPixmap
from PyQt5.QtWidgets import (QAbstractItemView, QApplication, QCheckBox, QColorDialog, QComboBox,
                             QDoubleSpinBox, QFileDialog, QFormLayout, QGridLayout, QGroupBox, QHBoxLayout,
                             QLabel, QLineEdit, QListWidget, QListWidgetItem, QMainWindow, QMessageBox,
                             QPushButton, QScrollArea, QSizePolicy, QSpinBox, QSplitter, QStyleFactory, QTabWidget,
                             QToolButton, QVBoxLayout, QWidget)

# --- the game's glyph table (the same in PES5 and PES6) ---
# index: A-Z = 0..25, 26 = space, 27 = '_', 28 = '.', 29..38 = '0'..'9'; (x, y, width, height)
GLYPHS = [
    (2, 1, 18, 30), (24, 1, 14, 30), (42, 1, 16, 30), (62, 1, 16, 30), (82, 1, 12, 30), (98, 1, 12, 30),
    (114, 1, 16, 30), (134, 1, 14, 30), (152, 1, 6, 30), (162, 1, 12, 30), (178, 1, 14, 30), (196, 1, 14, 30),
    (214, 1, 18, 30), (236, 1, 16, 30), (2, 33, 18, 30), (24, 33, 14, 30), (42, 33, 18, 30), (64, 33, 16, 30),
    (84, 33, 14, 30), (102, 33, 16, 30), (122, 33, 14, 30), (140, 33, 18, 30), (162, 33, 20, 30),
    (186, 33, 16, 30), (206, 33, 16, 30), (226, 33, 16, 30), (0, 0, 3, 30), (0, 0, 1, 30), (246, 33, 4, 30),
    (0, 0, 32, 64), (34, 0, 20, 64), (64, 0, 32, 64), (96, 0, 32, 64), (128, 0, 32, 64),
    (0, 64, 32, 64), (32, 64, 32, 64), (64, 64, 32, 64), (96, 64, 32, 64), (128, 64, 32, 64),
]
# kerning: (first, second, advance)
KERNING = {
    ('A', 'T'): 14, ('A', 'V'): 14, ('A', 'W'): 16, ('A', 'Y'): 13, ('F', 'A'): 8, ('F', 'J'): 6, ('F', '.'): 12,
    ('L', 'T'): 10, ('L', 'V'): 10, ('L', 'W'): 10, ('L', 'Y'): 8, ('P', 'A'): 12, ('P', 'J'): 8, ('P', '.'): 14,
    ('T', 'A'): 12, ('T', 'J'): 10, ('T', '.'): 16, ('V', 'A'): 12, ('V', 'J'): 12, ('V', '.'): 16,
    ('W', 'A'): 18, ('W', 'J'): 18, ('W', '.'): 22, ('Y', 'A'): 10, ('Y', 'J'): 10, ('Y', '.'): 14,
    ('.', 'T'): 2, ('.', 'V'): 2, ('.', 'W'): 2, ('.', 'Y'): 2,
}
NUMBER_TEXTURE = (256, 128)
NAME_TEXTURE = (256, 64)
LETTERS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ.'
LETTER_GAP = 4      # pixels between letters of a name (the game uses 1..4, less for long names)


def glyph_index(ch):
    if 'A' <= ch <= 'Z':
        return ord(ch) - ord('A')
    if '0' <= ch <= '9':
        return 29 + ord(ch) - ord('0')
    return {'.': 28, '_': 27}.get(ch, 26)


# --- images ---

def load_rgba(path):
    return np.asarray(Image.open(path).convert('RGBA'), dtype=np.float32) / 255.0


def trim(img, threshold):
    """Crop transparent edges (alpha <= threshold)."""
    mask = img[:, :, 3] > threshold
    if not mask.any():
        return img[:1, :1] * 0
    ys, xs = np.nonzero(mask)
    return img[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


_RESAMPLING = getattr(Image, 'Resampling', Image)
RESAMPLE_FILTERS = [('Lanczos', _RESAMPLING.LANCZOS), ('Bicubic', _RESAMPLING.BICUBIC),
                    ('Hamming', _RESAMPLING.HAMMING), ('Bilinear', _RESAMPLING.BILINEAR),
                    ('Box', _RESAMPLING.BOX), ('Nearest', _RESAMPLING.NEAREST)]


def resize_rgba(img, width, height, resample=_RESAMPLING.LANCZOS):
    """Resize with premultiplied alpha (no dark/colored fringes)."""
    width, height = max(1, int(round(width))), max(1, int(round(height)))
    if (height, width) == img.shape[:2]:
        return img
    pil = Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGBA').convert('RGBa')
    pil = pil.resize((width, height), resample).convert('RGBA')
    return np.asarray(pil, dtype=np.float32) / 255.0


def paste(canvas, img, x, y):
    """Copy 'img' into 'canvas' at (x, y), clipped."""
    h, w = img.shape[:2]
    ch, cw = canvas.shape[:2]
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(cw, x + w), min(ch, y + h)
    if x1 > x0 and y1 > y0:
        canvas[y0:y1, x0:x1] = img[y0 - y:y1 - y, x0 - x:x1 - x]


def is_template(images):
    """Red / green / blue templates: visible pixels are mostly a single pure channel; the rest mix at most two
    channels (anti-aliased edges between the color parts, e.g. red -> green) or are black."""
    total = mixed = pure = 0
    for img in images:
        if img is None:
            continue
        px = img[img[:, :, 3] > 0.5][:, :3]
        if not len(px):
            continue
        s = np.sort(px, axis=1)
        mn, mid, mx = s[:, 0], s[:, 1], s[:, 2]
        dark = mx < 0.08
        total += len(px)
        mixed += int(((mn <= mx * 0.12) | dark).sum())
        pure += int(((mid <= mx * 0.12) | dark).sum())
    return total > 0 and mixed >= total * 0.97 and pure >= total * 0.6


# --- quantization ---

def kmeans(points, weights, k, iterations=25, seed=1):
    """Weighted k-means (k-means++ start). Returns centroids."""
    n = len(points)
    if n <= k:
        return points.copy()
    rng = np.random.default_rng(seed)
    prob = weights / weights.sum()
    centroids = [points[rng.choice(n, p=prob)]]
    dist = ((points - centroids[0]) ** 2).sum(axis=1)
    for _ in range(1, k):
        p = dist * weights
        if p.sum() <= 0:
            break
        c = points[rng.choice(n, p=p / p.sum())]
        centroids.append(c)
        dist = np.minimum(dist, ((points - c) ** 2).sum(axis=1))
    centroids = np.array(centroids)
    for _ in range(iterations):
        d = ((points[:, None, :] - centroids[None, :, :]) ** 2).sum(axis=2)
        label = d.argmin(axis=1)
        moved = False
        for j in range(len(centroids)):
            m = label == j
            if m.any():
                c = (points[m] * weights[m, None]).sum(axis=0) / weights[m].sum()
                if not np.allclose(c, centroids[j]):
                    moved = True
                centroids[j] = c
        if not moved:
            break
    return centroids


def bleed(features, visible):
    """Every invisible pixel gets the color of the nearest visible one (what filtering and mipmaps blend in
    at the edges, instead of black)."""
    if not visible.any() or visible.all():
        return features
    _, (iy, ix) = ndimage.distance_transform_edt(~visible, return_indices=True)
    return features[iy, ix]


def fit_transparent(features, visible, count):
    """Colors of the transparent palette entries: clusters of the bled colors right around the visible pixels
    (the ones that are blended in), the heaviest one first."""
    ring = ndimage.binary_dilation(visible, structure=np.ones((3, 3), bool)) & ~visible
    if not ring.any():
        ring = ~visible
    keys = np.round(features[ring] * 63).astype(np.int32)
    uniq, counts = np.unique(keys, axis=0, return_counts=True)
    points = uniq.astype(np.float64) / 63.0
    # at least 3 clusters, so even a single entry gets a real color of the edge rather than a blend of them
    centroids = kmeans(points, counts.astype(np.float64), max(count, 3))
    label = ((points[:, None, :] - centroids[None, :, :]) ** 2).sum(axis=2).argmin(axis=1)
    weight = np.bincount(label, counts, len(centroids))
    order = np.argsort(-weight)[:count]
    return [np.clip(centroids[j], 0, 1) for j in order if weight[j] > 0]


def quantize(features, alpha, colors, alpha_threshold, transparent=1):
    """features: HxWx3 (0..1), alpha: HxW.
    Index 0 and 'transparent' - 1 more entries are transparent (alpha 0); their colors are the bled colors of
    the edges. 1.. 'colors' visible entries (the clusters of the visible pixels) follow index 0.
    Returns (indices HxW uint8, entries: list of (feature[3], alpha) for every index from 0)."""
    h, w = alpha.shape
    indices = np.zeros((h, w), np.uint8)
    visible = alpha > alpha_threshold
    if not visible.any():
        return indices, [(np.zeros(3), 0.0)]
    bled = bleed(features, visible)
    hidden_colors = fit_transparent(bled, visible, max(1, transparent))
    shown, entries = quantize_visible(features, alpha, visible, colors)
    indices[visible] = shown
    # the invisible pixels: the transparent entry nearest to their bled color
    if (~visible).any():
        bled = bled[~visible]
        d = ((bled[:, None, :] - np.array(hidden_colors)[None, :, :]) ** 2).sum(axis=2)
        nearest = d.argmin(axis=1)
        extra = len(entries) + 1        # the 2nd, 3rd, ... transparent entries follow the visible ones
        indices[~visible] = np.where(nearest == 0, 0, extra + nearest - 1).astype(np.uint8)
    return indices, ([(hidden_colors[0], 0.0)] + entries + [(c, 0.0) for c in hidden_colors[1:]])


def quantize_visible(features, alpha, visible, colors):
    """The visible pixels: k-means into 'colors' entries. Returns (indices 1.. of the visible pixels, entries)."""
    f = features[visible]
    a = alpha[visible]
    # premultiplied: edge pixels are matched by what they really add
    pts = np.concatenate([f * a[:, None], a[:, None]], axis=1)
    keys = np.round(pts * 63).astype(np.int32)
    uniq, inverse, counts = np.unique(keys, axis=0, return_inverse=True, return_counts=True)
    inverse = inverse.reshape(-1)
    centroids = kmeans(uniq.astype(np.float64) / 63.0, counts.astype(np.float64), colors)
    d = ((uniq[:, None, :] / 63.0 - centroids[None, :, :]) ** 2).sum(axis=2)
    label = d.argmin(axis=1)
    # order the entries by opacity (and brightness) so the palette looks tidy
    order = np.lexsort((-centroids[:, :3].sum(axis=1), -centroids[:, 3]))
    rank = np.empty(len(order), np.int64)
    rank[order] = np.arange(len(order))
    shown = (rank[label[inverse]] + 1).astype(np.uint8)
    entries = []
    for j in order:
        ca = max(1e-6, centroids[j, 3])
        entries.append((np.clip(centroids[j, :3] / ca, 0, 1), float(np.clip(centroids[j, 3], 0, 1))))
    return shown, entries


# --- output ---

def to_rgba_image(indices, palette):
    """indices HxW + palette [(r,g,b,a) 0..255] -> RGBA array uint8."""
    pal = np.zeros((256, 4), np.uint8)
    pal[:len(palette)] = palette
    return pal[indices]


def save_png4(path, indices, palette):
    img = Image.fromarray(indices, 'P')
    flat = []
    for r, g, b, _ in palette:
        flat += [r, g, b]
    flat += [0, 0, 0] * (16 - len(palette))
    img.putpalette(flat)
    alphas = bytes(p[3] for p in palette) + bytes(16 - len(palette))
    img.save(path, bits=4, transparency=alphas)


def save_palette_png(path, palette):
    pal = list(palette) + [(0, 0, 0, 0)] * (16 - len(palette))
    arr = np.array(pal[:16], np.uint8).reshape(2, 8, 4)
    Image.fromarray(arr, 'RGBA').save(path)


def qcolor_rgb(c):
    return np.array([c.red(), c.green(), c.blue()], np.float64) / 255.0


# --- numbers ---

class NumberParams(object):
    def __init__(self):
        self.scale = 4
        self.resample = _RESAMPLING.LANCZOS
        self.trim = False
        self.alpha_threshold = 8
        self.width = 100.0        # % of the common scale
        self.height = 100.0
        self.padding = (0, 0, 0, 0)  # left, top, right, bottom: output pixels
        self.halign = 'center'    # 'left' / 'center' / 'right'
        self.valign = 'center'    # 'top' / 'center' / 'bottom'
        self.colors = 15
        self.transparent = 1      # palette entries for the invisible pixels (index 0 + more)


def layout_numbers(images, p):
    """Place the digits into their cells. All digits use the same scale (the pictures are assumed to be drawn
    at the same scale): at 100% the tallest picture fills the cell height inside the padding. Each digit is
    aligned inside its cell (minus the padding) and clipped to it. Returns HxWx4 float (0..1)."""
    s = p.scale
    canvas = np.zeros((NUMBER_TEXTURE[1] * s, NUMBER_TEXTURE[0] * s, 4), np.float32)
    digits = []
    for img in images:
        if img is not None and p.trim:
            img = trim(img, p.alpha_threshold / 255.0)
        digits.append(img)
    heights = [d.shape[0] for d in digits if d is not None]
    if not heights:
        return canvas
    left, top, right, bottom = p.padding
    k = max(1, GLYPHS[29][3] * s - top - bottom) / float(max(heights))
    kx, ky = k * p.width / 100.0, k * p.height / 100.0
    for d, img in enumerate(digits):
        if img is None:
            continue
        gx, gy, gw, gh = (v * s for v in GLYPHS[29 + d])
        ix, iy, iw, ih = gx + left, gy + top, gw - left - right, gh - top - bottom
        if iw <= 0 or ih <= 0:
            continue
        part = resize_rgba(img, img.shape[1] * kx, img.shape[0] * ky, p.resample)
        th, tw = part.shape[:2]
        x = {'left': 0, 'right': iw - tw}.get(p.halign, (iw - tw) // 2)
        y = {'top': 0, 'bottom': ih - th}.get(p.valign, (ih - th) // 2)
        inner = np.zeros((ih, iw, 4), np.float32)
        paste(inner, part, x, y)
        canvas[iy:iy + ih, ix:ix + iw] = inner
    return canvas


def template_intensity(canvas):
    """The strongest channel value of the visible template pixels (templates drawn with e.g. 224 red)."""
    visible = canvas[:, :, 3] > 0.5
    mx = float(canvas[:, :, :3][visible].max()) if visible.any() else 1.0
    return mx if mx > 0 else 1.0


def number_palette(entries, mode, colors, intensity=1.0):
    """Palette (16 x RGBA 0..255) for the entries (from index 0). mode: 'original' / 'single' / 'template'.
    single: every entry gets the color (only the opacity is kept);
    template: red / green / blue of the entry = the shares of the 1st / 2nd / 3rd color.
    The transparent entries are recolored the same way: their color is what blends in at the edges."""
    pal = []
    c1, c2, c3 = colors
    for f, a in entries:
        if mode == 'template':
            w = np.clip(f / intensity, 0, 1)
            rgb = w[0] * c1 + w[1] * c2 + w[2] * c3
        elif mode == 'single':
            rgb = c1
        else:
            rgb = f
        rgb = np.clip(rgb, 0, 1)
        pal.append((int(rgb[0] * 255 + 0.5), int(rgb[1] * 255 + 0.5), int(rgb[2] * 255 + 0.5), int(a * 255 + 0.5)))
    return pal


def number_texture(indices, text, scale):
    """The player number texture (64x64) the way the game draws it (DrawKitNumber): the digits are placed
    side by side by their cell widths (no gap; '1' is 20 pixels wide, the others 32), centered; the game
    copies every texel with an index other than 0 (index 0 is the background).
    Returns (indices, cell rects (x, y, w, h))."""
    size = 64 * scale
    out = np.zeros((size, size), np.uint8)
    cells = [GLYPHS[29 + ord(ch) - ord('0')] for ch in text if ch.isdigit()]
    x = (size - sum(c[2] for c in cells) * scale) // 2
    rects = []
    for gx, gy, gw, gh in cells:
        gx, gy, gw, gh = gx * scale, gy * scale, gw * scale, gh * scale
        rects.append((x, 0, gw, min(gh, size)))
        x0, x1 = max(0, x), min(size, x + gw)
        if x1 > x0:
            part = indices[gy:gy + min(gh, size), gx + x0 - x:gx + x1 - x]
            region = out[:part.shape[0], x0:x1]
            m = part != 0
            region[m] = part[m]
        x += gw
    return out, rects


def number_cells(p):
    """Digit cells and their inner rects (minus the padding) in the number font texture: [(cell, inner)]."""
    left, top, right, bottom = p.padding
    out = []
    for d in range(10):
        gx, gy, gw, gh = (v * p.scale for v in GLYPHS[29 + d])
        out.append(((gx, gy, gw, gh), (gx + left, gy + top, gw - left - right, gh - top - bottom)))
    return out


# --- letters ---

class LetterParams(object):
    def __init__(self):
        self.font = ''
        self.scale = 4
        self.lowercase = False
        self.size = 100.0          # % of the cell height for capital letters
        self.width = 100.0         # % horizontal stretch
        self.weight = 0            # variable fonts: 'wght' axis value (0 = font default)
        self.embolden = 0.0        # pixels of the original size
        self.padding = 0.5
        self.offset = 0.0          # vertical offset, pixels of the original size
        self.wide = 'shrink'       # a letter wider than its cell: 'shrink' / 'squeeze' / 'clip'
        self.fill = QColor(255, 255, 255)
        self.border = True
        self.border_color = QColor(0, 0, 0)
        self.border_width = 1.0    # pixels of the original size


SUPERSAMPLE = 4


def font_axes(path):
    try:
        font = ImageFont.truetype(path, 32)
        return font.get_variation_axes()
    except Exception:
        return []


def make_font(path, size, weight):
    font = ImageFont.truetype(path, max(1, int(round(size))))
    if weight:
        try:
            axes = font.get_variation_axes()
            values = []
            for ax in axes:
                name = ax.get('name', b'')
                name = name.decode('latin-1') if isinstance(name, bytes) else str(name)
                values.append(weight if name.lower() in ('weight', 'wght') else ax.get('default', ax['minimum']))
            if axes:
                font.set_variation_by_axes(values)
        except Exception:
            pass
    return font


def render_glyph(font, ch, fill_stroke, border_stroke):
    """Coverage masks (0..1) of the fill and of fill+border, cropped to the ink; baseline at row 'base'."""
    size = font.size
    pad = int(border_stroke + fill_stroke) + 4
    w = int(size * 3) + 2 * pad
    h = int(size * 2) + 2 * pad
    base = int(size * 1.4) + pad
    fill = Image.new('L', (w, h), 0)
    ImageDraw.Draw(fill).text((pad, base), ch, font=font, fill=255, anchor='ls', stroke_width=int(round(fill_stroke)),
                              stroke_fill=255)
    outer = Image.new('L', (w, h), 0)
    ImageDraw.Draw(outer).text((pad, base), ch, font=font, fill=255, anchor='ls',
                               stroke_width=int(round(fill_stroke + border_stroke)), stroke_fill=255)
    bbox = outer.getbbox()
    if not bbox:
        return None
    x0, _, x1, _ = bbox
    f = np.asarray(fill, np.float32)[:, x0:x1] / 255.0
    o = np.asarray(outer, np.float32)[:, x0:x1] / 255.0
    return f, np.maximum(o, f), base


def layout_letters(p):
    """Fill and outer (fill+border) coverage of the name font texture, HxW floats."""
    s = p.scale
    ss = SUPERSAMPLE
    k = s * ss                                  # pixels per original pixel while drawing
    W, H = NAME_TEXTURE[0] * s, NAME_TEXTURE[1] * s
    fill_cov = np.zeros((H * ss, W * ss), np.float32)
    outer_cov = np.zeros((H * ss, W * ss), np.float32)
    cell_h = 30 * k
    pad = p.padding * k
    # font size: the capital H as high as the cell (minus padding)
    probe = make_font(p.font, 200, p.weight)
    hb = probe.getbbox('H', anchor='ls')
    cap = max(1, hb[3] - hb[1])
    size = 200.0 * (cell_h - 2 * pad) * (p.size / 100.0) / cap
    font = make_font(p.font, size, p.weight)
    fill_stroke = p.embolden * k
    border_stroke = p.border_width * k if p.border else 0
    for ch in LETTERS:
        gx, gy, gw, gh = GLYPHS[glyph_index(ch)]
        cx, cy, cw, chh = gx * k, gy * k, gw * k, gh * k
        draw_ch = ch.lower() if (p.lowercase and ch != '.') else ch
        r = render_glyph(font, draw_ch, fill_stroke, border_stroke)
        if r is None:
            continue
        f, o, base = r
        # horizontal stretch
        if p.width != 100:
            nw = max(1, int(round(o.shape[1] * p.width / 100.0)))
            f = np.asarray(Image.fromarray(f).resize((nw, f.shape[0]), Image.BILINEAR))
            o = np.asarray(Image.fromarray(o).resize((nw, o.shape[0]), Image.BILINEAR))
        inner_w = cw - 2 * pad
        if o.shape[1] > inner_w and p.wide != 'clip':
            if p.wide == 'shrink':
                factor = inner_w / o.shape[1]
                nw, nh = max(1, int(inner_w)), max(1, int(round(o.shape[0] * factor)))
                f = np.asarray(Image.fromarray(f).resize((nw, nh), Image.BILINEAR))
                o = np.asarray(Image.fromarray(o).resize((nw, nh), Image.BILINEAR))
                base = base * factor
            else:
                nw = max(1, int(inner_w))
                f = np.asarray(Image.fromarray(f).resize((nw, f.shape[0]), Image.BILINEAR))
                o = np.asarray(Image.fromarray(o).resize((nw, o.shape[0]), Image.BILINEAR))
        # baseline at the bottom of the cell (inside the padding)
        x = int(round(cx + (cw - o.shape[1]) / 2))
        y = int(round(cy + chh - pad - base + p.offset * k))
        cell_f = np.zeros((chh, cw), np.float32)
        cell_o = np.zeros((chh, cw), np.float32)
        for src, dst in ((f, cell_f), (o, cell_o)):
            h, w = src.shape
            x0, y0 = max(0, x - cx), max(0, y - cy)
            x1, y1 = min(cw, x - cx + w), min(chh, y - cy + h)
            if x1 > x0 and y1 > y0:
                dst[y0:y1, x0:x1] = src[y0 - (y - cy):y1 - (y - cy), x0 - (x - cx):x1 - (x - cx)]
        fill_cov[cy:cy + chh, cx:cx + cw] = np.maximum(fill_cov[cy:cy + chh, cx:cx + cw], cell_f)
        outer_cov[cy:cy + chh, cx:cx + cw] = np.maximum(outer_cov[cy:cy + chh, cx:cx + cw], cell_o)
    # down to the texture size (box filter)
    fill_cov = fill_cov.reshape(H, ss, W, ss).mean(axis=(1, 3))
    outer_cov = outer_cov.reshape(H, ss, W, ss).mean(axis=(1, 3))
    return fill_cov, outer_cov


def letter_indices(fill_cov, outer_cov, border):
    """The game's meaning: 0..11 - fill opacity, 12..15 - opaque, 25%..100% border."""
    a = np.clip(outer_cov, 0, 1)
    idx = np.round(a * 11).astype(np.int32)
    if border:
        # a border can't be semi-transparent in the game: its outer edge is either the border or nothing
        # (not the fill color, which would make a light halo around a dark border)
        bfrac = np.where(a > 0, np.clip((a - fill_cov) / np.maximum(a, 1e-6), 0, 1), 0)
        is_border = bfrac >= 0.125
        idx = np.where(is_border, np.where(a >= 0.35, 11 + np.clip(np.round(bfrac * 4), 1, 4), 0), idx)
    return idx.astype(np.uint8)


def letter_palette(fill, border):
    pal = []
    f = qcolor_rgb(fill)
    b = qcolor_rgb(border)
    for i in range(12):
        pal.append((int(f[0] * 255 + 0.5), int(f[1] * 255 + 0.5), int(f[2] * 255 + 0.5), int(round(255 * i / 11))))
    for i in range(1, 5):
        c = f + (b - f) * (i / 4.0)
        pal.append((int(c[0] * 255 + 0.5), int(c[1] * 255 + 0.5), int(c[2] * 255 + 0.5), 255))
    return pal


def sample_text(rgba, text, scale, gap):
    """Draw 'text' from the texture the way the game places characters (straight, no arc)."""
    text = text.upper()
    advances = []
    for i, ch in enumerate(text):
        g = GLYPHS[glyph_index(ch)]
        adv = KERNING.get((ch, text[i + 1]) if i + 1 < len(text) else None, g[2])
        if i + 1 < len(text) and not ch.isdigit():
            adv += gap
        advances.append(adv)
    width = max(1, sum(advances)) * scale
    height = max([GLYPHS[glyph_index(ch)][3] for ch in text] or [1]) * scale
    out = np.zeros((height, width, 4), np.uint8)
    x = 0
    for ch, adv in zip(text, advances):
        gx, gy, gw, gh = (v * scale for v in GLYPHS[glyph_index(ch)])
        if ch != ' ' and gw > 0:
            part = rgba[gy:gy + gh, gx:gx + gw]
            region = out[0:part.shape[0], x:x + part.shape[1]]
            m = part[:, :, 3:4].astype(np.float32) / 255.0
            region[:] = (part * m + region * (1 - m)).astype(np.uint8)
            region[:, :, 3] = np.maximum(region[:, :, 3], part[:, :, 3])
        x += adv * scale
    return out


# --- Qt helpers ---

def checker_pixmap(rgba, max_w, max_h):
    h, w = rgba.shape[:2]
    img = QImage(rgba.tobytes(), w, h, w * 4, QImage.Format_RGBA8888).copy()
    k = min(max_w / w, max_h / h) if w and h else 1
    k = max(k, 0.05)
    tw, th = max(1, int(w * k)), max(1, int(h * k))
    pm = QPixmap(tw, th)
    p = QPainter(pm)
    cell = 8
    for y in range(0, th, cell):
        for x in range(0, tw, cell):
            p.fillRect(x, y, cell, cell, QColor(204, 204, 204) if (x // cell + y // cell) % 2 else QColor(240, 240, 240))
    p.drawImage(pm.rect(), img)
    p.end()
    return pm


_CHECKER = {}
DARK_THEME = [False]


def checker_brush():
    dark = DARK_THEME[0]
    if dark not in _CHECKER:
        a, b = (QColor(58, 58, 58), QColor(78, 78, 78)) if dark else (QColor(240, 240, 240), QColor(204, 204, 204))
        pm = QPixmap(16, 16)
        pm.fill(a)
        p = QPainter(pm)
        p.fillRect(8, 0, 8, 8, b)
        p.fillRect(0, 8, 8, 8, b)
        p.end()
        _CHECKER[dark] = QBrush(pm)
    return _CHECKER[dark]


class ColorButton(QPushButton):
    """A color swatch; click to pick. Painted (not a style sheet: the color dialog would inherit it)."""
    changed = pyqtSignal()

    def __init__(self, color, parent=None):
        super(ColorButton, self).__init__(parent)
        self.setFixedSize(40, 22)
        self._color = QColor(color)
        self.clicked.connect(self._pick)

    def color(self):
        return QColor(self._color)

    def setColor(self, c):
        self._color = QColor(c)
        self.update()

    def paintEvent(self, e):
        super(ColorButton, self).paintEvent(e)
        p = QPainter(self)
        r = self.rect().adjusted(4, 4, -4, -4)
        p.fillRect(r, self._color)
        p.setPen(QColor(80, 80, 80))
        p.drawRect(r.adjusted(0, 0, -1, -1))
        p.end()

    def _pick(self):
        c = QColorDialog.getColor(self._color, self.window(), 'Color')
        if c.isValid():
            self.setColor(c)
            self.changed.emit()


class ViewState(QObject):
    """What several image views share: the scale and whether the cell bounds are drawn."""
    changed = pyqtSignal()
    STEPS = (0.125, 0.25, 0.333, 0.5, 0.667, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0)

    def __init__(self):
        super(ViewState, self).__init__()
        self.zoom = 1.0
        self.bounds = False

    def setZoom(self, value):
        if value != self.zoom:
            self.zoom = value
            self.changed.emit()

    def step(self, direction):
        if direction > 0:
            self.setZoom(next((z for z in self.STEPS if z > self.zoom + 1e-6), self.STEPS[-1]))
        else:
            self.setZoom(next((z for z in reversed(self.STEPS) if z < self.zoom - 1e-6), self.STEPS[0]))

    def setBounds(self, on):
        self.bounds = on
        self.changed.emit()


class ImageView(QWidget):
    """An RGBA image over a checkerboard, drawn at the shared zoom (the view scales, the image is not
    resampled), optionally with the cell bounds. Mouse wheel: zoom."""

    def __init__(self, state):
        super(ImageView, self).__init__()
        self.state = state
        self._image = None
        self._data = None
        self._cells = []     # solid rects (x, y, w, h) in image pixels
        self._inner = []     # dashed rects
        state.changed.connect(self._resize)
        self.setFixedSize(2, 2)

    def setRgba(self, rgba):
        if rgba is None or not rgba.size:
            self._image = self._data = None
        else:
            h, w = rgba.shape[:2]
            self._data = np.ascontiguousarray(rgba, np.uint8).tobytes()
            self._image = QImage(self._data, w, h, w * 4, QImage.Format_RGBA8888)
        self._resize()

    def setCells(self, cells, inner=()):
        self._cells = list(cells)
        self._inner = [r for r in inner if r not in self._cells]
        self.update()

    def _resize(self):
        z = self.state.zoom
        if self._image is None:
            self.setFixedSize(2, 2)
        else:
            self.setFixedSize(int(round(self._image.width() * z)) + 2, int(round(self._image.height() * z)) + 2)
        self.update()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setPen(self.palette().color(QPalette.Mid))
        p.drawRect(self.rect().adjusted(0, 0, -1, -1))
        if self._image is not None:
            z = self.state.zoom
            target = QRectF(1, 1, self.width() - 2, self.height() - 2)
            p.fillRect(target, checker_brush())
            if z < 1:
                p.setRenderHint(QPainter.SmoothPixmapTransform)
            p.drawImage(target, self._image)
            if self.state.bounds:
                p.setRenderHint(QPainter.SmoothPixmapTransform, False)
                for rects, style in ((self._cells, Qt.SolidLine), (self._inner, Qt.DashLine)):
                    pen = QPen(QColor(255, 0, 255), 1, style)
                    pen.setCosmetic(True)
                    p.setPen(pen)
                    for x, y, w, h in rects:
                        if w > 0 and h > 0:
                            p.drawRect(QRectF(1 + x * z, 1 + y * z, w * z - 1, h * z - 1))
        p.end()

    def wheelEvent(self, e):
        self.state.step(e.angleDelta().y())
        e.accept()


class PaletteView(QWidget):
    """The 16 palette entries as 8x2 swatches."""
    CELL = 14

    def __init__(self):
        super(PaletteView, self).__init__()
        self.setFixedSize(8 * self.CELL + 1, 2 * self.CELL + 1)
        self.palette = []

    def setPalette16(self, palette):
        self.palette = list(palette)
        self.update()

    def paintEvent(self, e):
        p = QPainter(self)
        c = self.CELL
        p.fillRect(self.rect(), checker_brush())
        for i, (r, g, b, a) in enumerate(self.palette[:16]):
            p.fillRect(QRectF((i % 8) * c, (i // 8) * c, c, c), QColor(r, g, b, a))
        p.setPen(QColor(120, 120, 120))
        for i in range(9):
            p.drawLine(i * c, 0, i * c, 2 * c)
        for i in range(3):
            p.drawLine(0, i * c, 8 * c, i * c)
        p.end()


class PreviewLabel(QLabel):
    def __init__(self, min_h=120):
        super(PreviewLabel, self).__init__()
        self.setAlignment(Qt.AlignCenter)
        self.setMinimumSize(160, min_h)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.setStyleSheet('border: 1px solid #999;')
        self._rgba = None

    def setRgba(self, rgba):
        self._rgba = rgba
        self._refresh()

    def resizeEvent(self, e):
        super(PreviewLabel, self).resizeEvent(e)
        self._refresh()

    def _refresh(self):
        if self._rgba is None or not self._rgba.size:
            self.clear()
            return
        self.setPixmap(checker_pixmap(self._rgba, self.width() - 4, self.height() - 4))


class DigitList(QListWidget):
    """File list: drop PNG files here, drag items to reorder (position = digit)."""
    filesDropped = pyqtSignal(list)

    def __init__(self):
        super(DigitList, self).__init__()
        self.setDragDropMode(QAbstractItemView.InternalMove)
        self.setAcceptDrops(True)
        self.setSelectionMode(QAbstractItemView.ExtendedSelection)

    def dragEnterEvent(self, e):
        if e.mimeData().hasUrls():
            e.acceptProposedAction()
        else:
            super(DigitList, self).dragEnterEvent(e)

    def dragMoveEvent(self, e):
        if e.mimeData().hasUrls():
            e.acceptProposedAction()
        else:
            super(DigitList, self).dragMoveEvent(e)

    def dropEvent(self, e):
        if e.mimeData().hasUrls():
            files = [u.toLocalFile() for u in e.mimeData().urls() if u.toLocalFile().lower().endswith('.png')]
            e.acceptProposedAction()
            self.filesDropped.emit(files)
        else:
            super(DigitList, self).dropEvent(e)


def digit_of(path):
    """The digit a file name stands for: the last single digit number in it (number_7.png, numbers_12_7_color.png)."""
    stem = os.path.splitext(os.path.basename(path))[0]
    nums = [n for n in re.findall(r'\d+', stem) if len(n) == 1]
    return int(nums[-1]) if nums else None


def spin(minimum, maximum, value, step=1.0, decimals=1, suffix=''):
    w = QDoubleSpinBox()
    w.setRange(minimum, maximum)
    w.setDecimals(decimals)
    w.setSingleStep(step)
    w.setValue(value)
    if suffix:
        w.setSuffix(suffix)
    return w


def scale_combo(default=4):
    c = QComboBox()
    for k in (1, 2, 3, 4, 6, 8):
        c.addItem('%dx' % k, k)
    c.setCurrentIndex(c.findData(default))
    return c


# --- Numbers tab ---

def grid_row(grid, row, label, *widgets, **kw):
    """A compact settings row: label, then the widgets packed to the left."""
    lbl = QLabel(label)
    grid.addWidget(lbl, row, 0)
    h = QHBoxLayout()
    h.setSpacing(4)
    for w in widgets:
        h.addWidget(w)
    h.addStretch(1)
    grid.addLayout(h, row, 1)
    tip = kw.get('tooltip')
    if tip:
        lbl.setToolTip(tip)
        for w in widgets:
            w.setToolTip(tip)
    return lbl


def int_spin(minimum, maximum, value, suffix=''):
    w = QSpinBox()
    w.setRange(minimum, maximum)
    w.setValue(value)
    if suffix:
        w.setSuffix(suffix)
    return w


def combo(items, default=None):
    c = QComboBox()
    c.setSizeAdjustPolicy(QComboBox.AdjustToContents)
    for text, data in items:
        c.addItem(text, data)
    if default is not None:
        c.setCurrentIndex(c.findData(default))
    return c


PANEL_WIDTH = 215


class NumberOutput(QGroupBox):
    """The settings of one result (number font or a shorts palette): the same indices, own colors."""
    changed = pyqtSignal()
    LABELS = {'single': ('Color:',), 'template': ('Red:', 'Green:', 'Blue:')}

    def __init__(self, title, colors):
        super(NumberOutput, self).__init__(title)
        self.setSizePolicy(QSizePolicy.Fixed, QSizePolicy.Maximum)
        self.setFixedWidth(PANEL_WIDTH)
        lay = QVBoxLayout(self)
        self.mode = combo((('Original colors', 'original'), ('Single color', 'single'), ('Template', 'template')))
        self.mode.setToolTip('Original colors: the colors of the pictures.\n'
                             'Single color: every visible pixel gets the color (only the opacity is kept).\n'
                             'Template: red / green / blue in the pictures = the share of the 1st / 2nd / 3rd '
                             'color.')
        lay.addWidget(self.mode)
        g = QGridLayout()
        g.setSpacing(4)
        self.labels = [QLabel() for _ in colors]
        self.c = [ColorButton(c) for c in colors]
        for i, (l, b) in enumerate(zip(self.labels, self.c)):
            g.addWidget(l, i, 0)
            g.addWidget(b, i, 1)
            b.changed.connect(self.changed)
        g.setColumnStretch(2, 1)
        lay.addLayout(g)
        self.palette_view = PaletteView()
        lay.addWidget(self.palette_view)
        self.save_image = QPushButton('Save Image')
        self.save_palette = QPushButton('Save Palette')
        lay.addWidget(self.save_image)
        lay.addWidget(self.save_palette)
        self.mode.currentIndexChanged.connect(self._mode_changed)
        self.indices = None
        self.palette = None
        self._update_colors()

    def currentMode(self):
        return self.mode.currentData()

    def setMode(self, mode):
        self.mode.setCurrentIndex(self.mode.findData(mode))

    def _mode_changed(self):
        self._update_colors()
        self.changed.emit()

    def _update_colors(self):
        labels = self.LABELS.get(self.currentMode(), ())
        for i, (l, b) in enumerate(zip(self.labels, self.c)):
            l.setVisible(i < len(labels))
            b.setVisible(i < len(labels))
            if i < len(labels):
                l.setText(labels[i])

    def colors(self):
        return [qcolor_rgb(b.color()) for b in self.c]


class NumbersTab(QWidget):
    def __init__(self, settings):
        super(NumbersTab, self).__init__()
        self.settings = settings
        self.images = {}
        self.view = ViewState()
        self.timer = QTimer(self)
        self.timer.setSingleShot(True)
        self.timer.setInterval(200)
        self.timer.timeout.connect(self.generate)

        split = QSplitter(Qt.Horizontal)
        outer = QHBoxLayout(self)
        outer.addWidget(split)

        # left: files and the shared settings
        left = QWidget()
        ll = QVBoxLayout(left)
        ll.setContentsMargins(0, 0, 0, 0)
        ll.addWidget(QLabel('Digit pictures (drop PNG files; drag to reorder, 1st = 0):'))
        self.list = DigitList()
        self.list.filesDropped.connect(self.add_files)
        self.list.model().rowsMoved.connect(self._order_changed)
        ll.addWidget(self.list, 1)
        b = QHBoxLayout()
        for text, slot in (('Add...', self._browse), ('Sort by Name', self.sort_by_name), ('Remove', self._remove),
                           ('Clear', self._clear)):
            w = QPushButton(text)
            w.clicked.connect(slot)
            b.addWidget(w)
        ll.addLayout(b)

        box = QGroupBox('Settings')
        g = QGridLayout(box)
        g.setVerticalSpacing(4)
        g.setColumnStretch(1, 1)
        self.scale = scale_combo()
        self.scale.setSizeAdjustPolicy(QComboBox.AdjustToContents)
        self.size_label = QLabel()
        grid_row(g, 0, 'Output size:', self.scale, self.size_label)
        self.resample = combo([(name, f) for name, f in RESAMPLE_FILTERS])
        grid_row(g, 1, 'Resampling:', self.resample)
        self.dw = int_spin(10, 400, 100, ' %')
        self.dh = int_spin(10, 400, 100, ' %')
        self.lock = QCheckBox('Lock')
        self.lock.setChecked(True)
        grid_row(g, 2, 'Digit size:', QLabel('W'), self.dw, QLabel('H'), self.dh, self.lock,
                 tooltip='The same scale for all digits. 100%: the tallest picture fills the cell height '
                         '(inside the padding); smaller pictures stay smaller. Lock: keep the width / height '
                         'ratio.')
        self.halign = combo((('Left', 'left'), ('Center', 'center'), ('Right', 'right')), 'center')
        self.valign = combo((('Top', 'top'), ('Center', 'center'), ('Bottom', 'bottom')), 'center')
        grid_row(g, 3, 'Alignment:', self.halign, self.valign)
        self.pad = [int_spin(0, 128, 0) for _ in range(4)]
        self.pad_same = QCheckBox('All')
        self.pad_same.setChecked(True)
        grid_row(g, 4, 'Padding:', self.pad_same, *self.pad,
                 tooltip='Left, top, right, bottom: output pixels kept transparent inside every digit cell. '
                         'All: the same padding on all 4 sides.')
        self.trim = QCheckBox('Trim transparent edges')
        self.trim.setToolTip('Crop every picture to its visible pixels before placing it '
                             '(then the digits are aligned by their own bounds)')
        grid_row(g, 5, '', self.trim)
        self.threshold = int_spin(0, 254, 8)
        grid_row(g, 6, 'Alpha threshold:', self.threshold,
                 tooltip='Pixels with this opacity (0..255) or lower are transparent (index 0)')
        self.ncolors = int_spin(2, 15, 15)
        self.ntransparent = int_spin(1, 8, 1)
        grid_row(g, 7, 'Colors:', self.ncolors, QLabel('+ transparent'), self.ntransparent,
                 tooltip='Palette entries for the visible pixels + the entries reserved for the transparent ones '
                         '(16 in total). The transparent pixels keep the color of the nearest visible pixel '
                         '(bleeding): the game filters the texture, so that color blends in at the edges. '
                         'Index 0 gets the most common edge color; more entries help with multicolored edges.')
        self.full_intensity = QCheckBox('Template: brightest = 100%')
        self.full_intensity.setChecked(True)
        self.full_intensity.setToolTip('Template coloring: the brightest red / green / blue of the pictures counts '
                                       'as 100% of the color (for templates drawn with e.g. 224 instead of 255). '
                                       'Off: the template values are used as they are (224 = 88%).')
        grid_row(g, 8, '', self.full_intensity)
        self.auto = QCheckBox('Auto-Generate')
        self.auto.setChecked(True)
        gen = QPushButton('Generate')
        gen.clicked.connect(self.generate)
        reset = QPushButton('Reset')
        reset.setToolTip('Reset the settings to the defaults')
        reset.clicked.connect(self.reset_settings)
        grid_row(g, 9, '', self.auto, gen, reset)
        ll.addWidget(box)
        split.addWidget(left)

        # right: the results, stacked
        right = QWidget()
        rl = QVBoxLayout(right)
        rl.setContentsMargins(0, 0, 0, 0)
        bar = QHBoxLayout()
        zout = QPushButton('-')
        zin = QPushButton('+')
        z100 = QPushButton('100%')
        for w in (zout, zin):
            w.setFixedWidth(28)
        zout.clicked.connect(lambda: self.view.step(-1))
        zin.clicked.connect(lambda: self.view.step(1))
        z100.clicked.connect(lambda: self.view.setZoom(1.0))
        for w in (zout, zin, z100):
            w.setToolTip('Zoom (or the mouse wheel over a picture)')
        self.zoom_label = QLabel()
        self.zoom_label.setMinimumWidth(40)
        self.zoom_label.setAlignment(Qt.AlignCenter)
        self.bounds = QCheckBox('Cell bounds')
        self.bounds.setToolTip('Draw the digit cells (solid) and the padding (dashed)')
        self.bounds.toggled.connect(self.view.setBounds)
        self.hidden = QCheckBox('Transparent colors')
        self.hidden.setToolTip('Show the colors of the transparent pixels (alpha ignored): what blends in at the '
                               'edges')
        self.hidden.toggled.connect(self.schedule)
        self.status = QLabel()
        for w in (zout, self.zoom_label, zin, z100):
            bar.addWidget(w)
        bar.addSpacing(16)
        bar.addWidget(self.bounds)
        bar.addWidget(self.hidden)
        bar.addSpacing(16)
        bar.addWidget(self.status, 1)
        rl.addLayout(bar)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        content = QWidget()
        cl = QVBoxLayout(content)
        self.outputs = [
            NumberOutput('Number Font (shirt numbers)', ('#ffffff', '#000000', '#808080')),
            NumberOutput('Palette 1 (shorts numbers)', ('#ffffff', '#000000', '#808080')),
            NumberOutput('Palette 2 (shorts numbers)', ('#000000', '#ffffff', '#808080')),
        ]
        self.views = []
        names = ('number_font', 'number_palette1', 'number_palette2')
        for out, name in zip(self.outputs, names):
            view = ImageView(self.view)
            self.views.append(view)
            cl.addLayout(self._row(view, out))
            out.changed.connect(self.schedule)
            out.save_image.clicked.connect(lambda _=False, o=out, n=name: self._save(o, n, False))
            out.save_palette.clicked.connect(lambda _=False, o=out, n=name: self._save(o, n, True))

        # the player number texture as the game makes it
        sample_box = QGroupBox('In-game number')
        sample_box.setSizePolicy(QSizePolicy.Fixed, QSizePolicy.Maximum)
        sample_box.setFixedWidth(PANEL_WIDTH)
        sl = QVBoxLayout(sample_box)
        self.sample_text = QLineEdit('10')
        self.sample_text.setMaxLength(3)
        self.sample_text.setFixedWidth(60)
        sl.addWidget(self.sample_text)
        note = QLabel("The player's 64x64 number texture of each result, made like the game does: the digits "
                      "side by side by their cell widths (1: 20, others: 32 pixels), no extra gap, centered.")
        note.setWordWrap(True)
        sl.addWidget(note)
        samples = QHBoxLayout()
        samples.setContentsMargins(0, 0, 0, 0)
        self.sample_views = [ImageView(self.view) for _ in self.outputs]
        for v in self.sample_views:
            samples.addWidget(v, 0, Qt.AlignTop)
        sw = QWidget()
        sw.setLayout(samples)
        cl.addLayout(self._row(sw, sample_box))
        cl.addStretch(1)
        scroll.setWidget(content)
        rl.addWidget(scroll, 1)
        split.addWidget(right)
        split.setStretchFactor(0, 0)
        split.setStretchFactor(1, 1)

        for w in (self.scale, self.resample, self.halign, self.valign):
            w.currentIndexChanged.connect(self.schedule)
        for w in (self.trim, self.full_intensity):
            w.toggled.connect(self.schedule)
        for w in (self.threshold, self.ncolors):
            w.valueChanged.connect(self.schedule)
        self.ntransparent.valueChanged.connect(self._transparent_changed)
        self._ratio = 1.0
        self.dw.valueChanged.connect(lambda v: self._size_changed(self.dw, self.dh))
        self.dh.valueChanged.connect(lambda v: self._size_changed(self.dh, self.dw))
        self.lock.toggled.connect(self._lock_toggled)
        for w in self.pad:
            w.valueChanged.connect(lambda v, w=w: self._padding_changed(w))
        self.pad_same.toggled.connect(lambda on: self._padding_changed(self.pad[0]))
        self.sample_text.textChanged.connect(self.schedule)
        self.auto.toggled.connect(lambda on: self.schedule() if on else None)
        self.view.changed.connect(self._update_zoom)
        self._update_size()
        self._update_zoom()

    @staticmethod
    def _row(view, panel):
        row = QHBoxLayout()
        row.addWidget(view, 0, Qt.AlignTop)
        row.addWidget(panel, 0, Qt.AlignTop)
        row.addStretch(1)
        return row

    def _update_zoom(self):
        self.zoom_label.setText('%d%%' % round(self.view.zoom * 100))

    def _transparent_changed(self, value):
        self.ncolors.setMaximum(16 - value)
        self.schedule()

    def reset_settings(self):
        """The shared settings back to the defaults (the colors of the results are kept)."""
        p = NumberParams()
        widgets = (self.scale, self.resample, self.dw, self.dh, self.lock, self.halign, self.valign, self.pad_same,
                   self.trim, self.threshold, self.ntransparent, self.ncolors, self.full_intensity) + tuple(self.pad)
        for w in widgets:
            w.blockSignals(True)
        self.scale.setCurrentIndex(self.scale.findData(p.scale))
        self.resample.setCurrentIndex(self.resample.findData(p.resample))
        self.dw.setValue(int(p.width))
        self.dh.setValue(int(p.height))
        self.lock.setChecked(True)
        self._ratio = 1.0
        self.halign.setCurrentIndex(self.halign.findData(p.halign))
        self.valign.setCurrentIndex(self.valign.findData(p.valign))
        self.pad_same.setChecked(True)
        for w, v in zip(self.pad, p.padding):
            w.setValue(v)
        self.trim.setChecked(p.trim)
        self.threshold.setValue(p.alpha_threshold)
        self.ntransparent.setValue(p.transparent)
        self.ncolors.setMaximum(16 - p.transparent)
        self.ncolors.setValue(p.colors)
        self.full_intensity.setChecked(True)
        for w in widgets:
            w.blockSignals(False)
        self.schedule()

    def _update_size(self):
        k = self.scale.currentData()
        self.size_label.setText('%d x %d' % (NUMBER_TEXTURE[0] * k, NUMBER_TEXTURE[1] * k))

    def _lock_toggled(self, on):
        if on:
            self._ratio = self.dh.value() / float(self.dw.value())

    def _size_changed(self, changed, other):
        if self.lock.isChecked():
            ratio = self._ratio if changed is self.dw else 1.0 / self._ratio
            other.blockSignals(True)
            other.setValue(int(round(changed.value() * ratio)))
            other.blockSignals(False)
        self.schedule()

    def _padding_changed(self, changed):
        if self.pad_same.isChecked():
            for w in self.pad:
                if w is not changed:
                    w.blockSignals(True)
                    w.setValue(changed.value())
                    w.blockSignals(False)
        self.schedule()

    def schedule(self):
        self._update_size()
        if self.auto.isChecked():
            self.timer.start()

    # files
    def _browse(self):
        files, _ = QFileDialog.getOpenFileNames(self, 'Digit Pictures', self.settings.value('numbers_dir', ''),
                                                'PNG images (*.png)')
        if files:
            self.settings.setValue('numbers_dir', os.path.dirname(files[0]))
            self.add_files(files)

    def add_files(self, files):
        for f in files:
            if f in self.images:
                continue
            try:
                self.images[f] = load_rgba(f)
            except Exception as e:
                QMessageBox.warning(self, 'Digit Pictures', 'Unable to read %s:\n%s' % (f, e))
                continue
            item = QListWidgetItem(os.path.basename(f))
            item.setData(Qt.UserRole, f)
            self.list.addItem(item)
        self.sort_by_name(silent=True)
        if self.list.count() > 10:
            QMessageBox.information(self, 'Digit Pictures', 'Only the first 10 pictures are used.')
        imgs = [self.images[self.list.item(i).data(Qt.UserRole)] for i in range(min(10, self.list.count()))]
        mode = 'template' if is_template(imgs) else 'original'
        for out in self.outputs:
            out.blockSignals(True)
            out.setMode(mode)
            out.blockSignals(False)
        self._relabel()
        self.schedule()

    def sort_by_name(self, silent=False):
        items = [self.list.item(i).data(Qt.UserRole) for i in range(self.list.count())]
        known = sorted([f for f in items if digit_of(f) is not None], key=digit_of)
        rest = [f for f in items if digit_of(f) is None]
        # put files named by a digit into their position, the others into the free ones
        slots = [None] * 10
        extra = []
        for f in known:
            d = digit_of(f)
            if slots[d] is None:
                slots[d] = f
            else:
                extra.append(f)
        free = iter(rest + extra)
        for d in range(10):
            if slots[d] is None:
                slots[d] = next(free, None)
        order = [f for f in slots if f] + list(free)
        self.list.clear()
        for f in order:
            item = QListWidgetItem(os.path.basename(f))
            item.setData(Qt.UserRole, f)
            self.list.addItem(item)
        self._relabel()
        if not silent:
            self.schedule()

    def _remove(self):
        for item in self.list.selectedItems():
            self.images.pop(item.data(Qt.UserRole), None)
            self.list.takeItem(self.list.row(item))
        self._relabel()
        self.schedule()

    def _clear(self):
        self.list.clear()
        self.images.clear()
        self.schedule()

    def _order_changed(self, *args):
        self._relabel()
        self.schedule()

    def _relabel(self):
        for i in range(self.list.count()):
            item = self.list.item(i)
            name = os.path.basename(item.data(Qt.UserRole))
            item.setText(('%d:  %s' % (i, name)) if i < 10 else ('(unused)  %s' % name))

    # generation
    def params(self):
        p = NumberParams()
        p.scale = self.scale.currentData()
        p.resample = self.resample.currentData()
        p.trim = self.trim.isChecked()
        p.alpha_threshold = self.threshold.value()
        p.width = self.dw.value()
        p.height = self.dh.value()
        p.padding = tuple(w.value() for w in self.pad)
        p.halign = self.halign.currentData()
        p.valign = self.valign.currentData()
        p.colors = self.ncolors.value()
        p.transparent = self.ntransparent.value()
        return p

    def generate(self):
        files = [self.list.item(i).data(Qt.UserRole) for i in range(min(10, self.list.count()))]
        if not files:
            self.status.setText('Add the digit pictures.')
            return
        QApplication.setOverrideCursor(Qt.WaitCursor)
        try:
            p = self.params()
            images = [self.images.get(f) for f in files] + [None] * (10 - len(files))
            canvas = layout_numbers(images, p)
            indices, entries = quantize(canvas[:, :, :3], canvas[:, :, 3], p.colors, p.alpha_threshold / 255.0,
                                        p.transparent)
            intensity = template_intensity(canvas) if self.full_intensity.isChecked() else 1.0
            text = ''.join(ch for ch in self.sample_text.text() if ch.isdigit())
            sample_indices, sample_cells = number_texture(indices, text, p.scale)
            cells = number_cells(p)
            for out, view, sample in zip(self.outputs, self.views, self.sample_views):
                pal = number_palette(entries, out.currentMode(), out.colors(), intensity)
                out.indices = indices
                out.palette = pal
                shown = [(r, g, b, 255) for r, g, b, _ in pal] if self.hidden.isChecked() else pal
                view.setRgba(to_rgba_image(indices, shown))
                view.setCells([c for c, _ in cells], [i for _, i in cells])
                sample.setRgba(to_rgba_image(sample_indices, shown) if text else None)
                sample.setCells(sample_cells)
                out.palette_view.setPalette16(pal + [(0, 0, 0, 0)] * (16 - len(pal)))
            hidden = sum(1 for _, a in entries if a == 0)
            self.status.setText('%d x %d, %d colors + %d transparent' % (indices.shape[1], indices.shape[0],
                                                                       len(entries) - hidden, hidden))
        except Exception as e:
            self.status.setText('Error: %s' % e)
        finally:
            QApplication.restoreOverrideCursor()

    def _save(self, out, name, palette_only):
        if out.palette is None:
            self.generate()
        if out.palette is None:
            return
        suffix = '_palette' if palette_only else ''
        path, _ = QFileDialog.getSaveFileName(self, 'Save', os.path.join(self.settings.value('save_dir', ''),
                                              name + suffix + '.png'), 'PNG image (*.png)')
        if not path:
            return
        self.settings.setValue('save_dir', os.path.dirname(path))
        if palette_only:
            save_palette_png(path, out.palette)
        else:
            save_png4(path, out.indices, out.palette)


# --- Letters tab ---

class LettersTab(QWidget):
    def __init__(self, settings):
        super(LettersTab, self).__init__()
        self.settings = settings
        self.indices = None
        self.palette = None
        self.timer = QTimer(self)
        self.timer.setSingleShot(True)
        self.timer.setInterval(250)
        self.timer.timeout.connect(self.generate)

        split = QSplitter(Qt.Horizontal)
        outer = QHBoxLayout(self)
        outer.addWidget(split)

        left = QWidget()
        form = QFormLayout(left)
        fr = QHBoxLayout()
        self.font_path = QLineEdit()
        self.font_path.setReadOnly(True)
        browse = QPushButton('Font...')
        browse.clicked.connect(self._browse)
        fr.addWidget(self.font_path, 1)
        fr.addWidget(browse)
        form.addRow('Font file:', fr)
        self.scale = scale_combo()
        self.size_label = QLabel()
        sr = QHBoxLayout()
        sr.addWidget(self.scale)
        sr.addWidget(self.size_label, 1)
        form.addRow('Output size:', sr)
        self.case = QComboBox()
        self.case.addItem('Capital letters', False)
        self.case.addItem('Small letters', True)
        form.addRow('Letters:', self.case)
        self.size = spin(20, 200, 100, 1, 0, ' %')
        form.addRow('Size (capital height):', self.size)
        self.width = spin(30, 200, 100, 1, 0, ' %')
        form.addRow('Width:', self.width)
        self.weight = QSpinBox()
        self.weight.setRange(0, 1000)
        self.weight.setSingleStep(50)
        self.weight.setSpecialValueText('font default')
        form.addRow('Weight (variable fonts):', self.weight)
        self.embolden = spin(0, 4, 0, 0.25, 2, ' px')
        form.addRow('Embolden (original px):', self.embolden)
        self.padding = spin(0, 6, 0.5, 0.25, 2, ' px')
        form.addRow('Padding (original px):', self.padding)
        self.offset = spin(-10, 10, 0, 0.25, 2, ' px')
        form.addRow('Vertical offset (original px):', self.offset)
        self.wide = QComboBox()
        self.wide.addItem('Shrink', 'shrink')
        self.wide.addItem('Squeeze', 'squeeze')
        self.wide.addItem('Clip', 'clip')
        form.addRow('Too wide for its cell:', self.wide)
        self.fill = ColorButton('#ffffff')
        form.addRow('Fill color:', self.fill)
        br = QHBoxLayout()
        self.border = QCheckBox('Border')
        self.border.setChecked(True)
        self.border_color = ColorButton('#000000')
        self.border_width = spin(0.25, 6, 1, 0.25, 2, ' px')
        br.addWidget(self.border)
        br.addWidget(self.border_color)
        br.addWidget(self.border_width)
        form.addRow('Border:', br)
        self.sample_text = QLineEdit('PLAYER NAME')
        form.addRow('Sample:', self.sample_text)
        gr = QHBoxLayout()
        self.auto = QCheckBox('Auto-Generate')
        self.auto.setChecked(True)
        gen = QPushButton('Generate')
        gen.clicked.connect(self.generate)
        gr.addWidget(self.auto)
        gr.addWidget(gen)
        form.addRow(gr)
        note = QLabel('Every letter has a fixed cell in the game (e.g. I: 6, W: 20 original pixels wide); '
                      'letters are fitted into their cells. Indices: 0..11 fill opacity, 12..15 border.')
        note.setWordWrap(True)
        form.addRow(note)
        self.status = QLabel()
        self.status.setWordWrap(True)
        form.addRow(self.status)
        split.addWidget(left)

        right = QWidget()
        rl = QVBoxLayout(right)
        self.preview = PreviewLabel(160)
        self.sample = PreviewLabel(60)
        self.palette_view = PreviewLabel(24)
        self.palette_view.setMaximumHeight(28)
        rl.addWidget(self.preview, 3)
        rl.addWidget(self.sample, 1)
        rl.addWidget(self.palette_view)
        b = QHBoxLayout()
        si = QPushButton('Save Image')
        si.clicked.connect(lambda: self._save(False))
        sp = QPushButton('Save Palette')
        sp.clicked.connect(lambda: self._save(True))
        b.addStretch(1)
        b.addWidget(si)
        b.addWidget(sp)
        rl.addLayout(b)
        split.addWidget(right)
        split.setStretchFactor(1, 3)

        for w in (self.scale, self.case, self.wide):
            w.currentIndexChanged.connect(self.schedule)
        for w in (self.size, self.width, self.embolden, self.padding, self.offset, self.border_width, self.weight):
            w.valueChanged.connect(self.schedule)
        for w in (self.fill, self.border_color):
            w.changed.connect(self.schedule)
        self.border.toggled.connect(self.schedule)
        self.sample_text.textChanged.connect(self.schedule)
        self.auto.toggled.connect(lambda on: self.schedule() if on else None)
        self._update_size()

    def _update_size(self):
        k = self.scale.currentData()
        self.size_label.setText('%d x %d' % (NAME_TEXTURE[0] * k, NAME_TEXTURE[1] * k))

    def schedule(self):
        self._update_size()
        if self.auto.isChecked():
            self.timer.start()

    def _browse(self):
        path, _ = QFileDialog.getOpenFileName(self, 'Font', self.settings.value('font_dir', ''),
                                              'Fonts (*.ttf *.otf *.ttc)')
        if not path:
            return
        self.settings.setValue('font_dir', os.path.dirname(path))
        self.font_path.setText(path)
        axes = font_axes(path)
        wght = [a for a in axes if str(a.get('name', '')).lower().strip("b'") in ('weight', 'wght')]
        self.weight.setEnabled(bool(wght))
        if wght:
            self.weight.setRange(int(wght[0]['minimum']), int(wght[0]['maximum']))
            self.weight.setValue(int(wght[0].get('default', wght[0]['minimum'])))
        else:
            self.weight.setValue(0)
        self.schedule()

    def params(self):
        p = LetterParams()
        p.font = self.font_path.text()
        p.scale = self.scale.currentData()
        p.lowercase = self.case.currentData()
        p.size = self.size.value()
        p.width = self.width.value()
        p.weight = self.weight.value() if self.weight.isEnabled() else 0
        p.embolden = self.embolden.value()
        p.padding = self.padding.value()
        p.offset = self.offset.value()
        p.wide = self.wide.currentData()
        p.fill = self.fill.color()
        p.border = self.border.isChecked()
        p.border_color = self.border_color.color()
        p.border_width = self.border_width.value()
        return p

    def generate(self):
        p = self.params()
        if not p.font or not os.path.isfile(p.font):
            self.status.setText('Select a font file.')
            return
        QApplication.setOverrideCursor(Qt.WaitCursor)
        try:
            fill_cov, outer_cov = layout_letters(p)
            self.indices = letter_indices(fill_cov, outer_cov, p.border)
            self.palette = letter_palette(p.fill, p.border_color if p.border else p.fill)
            rgba = to_rgba_image(self.indices, self.palette)
            self.preview.setRgba(rgba)
            self.sample.setRgba(sample_text(rgba, self.sample_text.text() or 'A', p.scale, LETTER_GAP))
            self.palette_view.setRgba(np.array(self.palette, np.uint8).reshape(1, 16, 4))
            self.status.setText('%d x %d' % (self.indices.shape[1], self.indices.shape[0]))
        except Exception as e:
            self.status.setText('Error: %s' % e)
        finally:
            QApplication.restoreOverrideCursor()

    def _save(self, palette_only):
        if self.palette is None:
            self.generate()
        if self.palette is None:
            return
        name = 'name_font_palette.png' if palette_only else 'name_font.png'
        path, _ = QFileDialog.getSaveFileName(self, 'Save', os.path.join(self.settings.value('save_dir', ''), name),
                                              'PNG image (*.png)')
        if not path:
            return
        self.settings.setValue('save_dir', os.path.dirname(path))
        if palette_only:
            save_palette_png(path, self.palette)
        else:
            save_png4(path, self.indices, self.palette)


# --- theme ---

def dark_palette():
    p = QPalette()
    text, disabled = QColor(220, 220, 220), QColor(120, 120, 120)
    for role, color in ((QPalette.Window, QColor(45, 45, 45)), (QPalette.WindowText, text),
                        (QPalette.Base, QColor(30, 30, 30)), (QPalette.AlternateBase, QColor(40, 40, 40)),
                        (QPalette.ToolTipBase, QColor(50, 50, 50)), (QPalette.ToolTipText, text),
                        (QPalette.Text, text), (QPalette.Button, QColor(55, 55, 55)), (QPalette.ButtonText, text),
                        (QPalette.BrightText, QColor(255, 80, 80)), (QPalette.Link, QColor(80, 150, 230)),
                        (QPalette.Highlight, QColor(42, 110, 190)), (QPalette.HighlightedText, QColor(255, 255, 255)),
                        (QPalette.Light, QColor(70, 70, 70)), (QPalette.Midlight, QColor(60, 60, 60)),
                        (QPalette.Mid, QColor(90, 90, 90)), (QPalette.Dark, QColor(25, 25, 25)),
                        (QPalette.Shadow, QColor(10, 10, 10)), (QPalette.PlaceholderText, disabled)):
        p.setColor(role, color)
    for role in (QPalette.WindowText, QPalette.Text, QPalette.ButtonText):
        p.setColor(QPalette.Disabled, role, disabled)
    return p


def apply_theme(app, dark):
    DARK_THEME[0] = dark
    app.setStyle(QStyleFactory.create('Fusion'))
    app.setPalette(dark_palette() if dark else app.style().standardPalette())
    for w in app.allWidgets():
        w.update()


def theme_icon(dark):
    """Light theme: a dark-grey sun; dark theme: a white moon."""
    size, ratio = 20, 2
    pm = QPixmap(size * ratio, size * ratio)
    pm.setDevicePixelRatio(ratio)
    pm.fill(Qt.transparent)
    p = QPainter(pm)
    p.setRenderHint(QPainter.Antialiasing)
    c = size / 2.0
    if dark:
        moon = QPainterPath()
        moon.addEllipse(QRectF(c - 7.5, c - 7.5, 15, 15))
        cut = QPainterPath()
        cut.addEllipse(QRectF(c - 3.5, c - 10.5, 14, 14))
        p.fillPath(moon.subtracted(cut), QColor(255, 255, 255))
    else:
        color = QColor(80, 80, 80)
        p.setPen(Qt.NoPen)
        p.setBrush(color)
        p.drawEllipse(QRectF(c - 4, c - 4, 8, 8))
        pen = QPen(color, 1.8)
        pen.setCapStyle(Qt.RoundCap)
        p.setPen(pen)
        for i in range(8):
            a = i * np.pi / 4
            p.drawLine(QPointF(c + 6.3 * np.cos(a), c + 6.3 * np.sin(a)),
                       QPointF(c + 8.8 * np.cos(a), c + 8.8 * np.sin(a)))
    p.end()
    return QIcon(pm)


class MainWindow(QMainWindow):
    def __init__(self):
        super(MainWindow, self).__init__()
        self.setWindowTitle('Kit Number / Name Fonts')
        self.settings = QSettings('PesAssetLoader', 'kitfont_gen')
        tabs = QTabWidget()
        tabs.addTab(NumbersTab(self.settings), 'Numbers')
        tabs.addTab(LettersTab(self.settings), 'Letters')
        self.theme = QToolButton()
        self.theme.setAutoRaise(True)
        self.theme.setIconSize(QSize(20, 20))
        self.theme.clicked.connect(lambda: self.setDark(not DARK_THEME[0]))
        tabs.setCornerWidget(self.theme, Qt.TopRightCorner)
        self.setCentralWidget(tabs)
        self.resize(1300, 800)
        self.setDark(self.settings.value('dark_theme', False, type=bool))

    def setDark(self, dark):
        apply_theme(QApplication.instance(), dark)
        self.settings.setValue('dark_theme', dark)
        self.theme.setIcon(theme_icon(dark))
        self.theme.setToolTip('Switch to the light theme' if dark else 'Switch to the dark theme')


def main():
    app = QApplication(sys.argv)
    w = MainWindow()
    w.show()
    sys.exit(app.exec_())


if __name__ == '__main__':
    main()
