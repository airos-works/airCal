#!/usr/bin/env python3
"""Generate airCal's 64-unit HVIF icon and optional PNG previews.

Run: python3 tools/make-icon.py [--no-preview]
Outputs go to this checkout's resources directory, independent of cwd.
Only previews require Pillow. All lettering is drawn as vector paths.
"""
from pathlib import Path
import argparse
import sys

# Keep regeneration from leaving bytecode outside the artwork outputs.
sys.dont_write_bytecode = True
import hvif

C = hvif.hex_color
K = 0.55228475


class Icon:
    def __init__(self):
        self.styles, self.paths, self.shapes = [], [], []

    def solid(self, color, alpha=255):
        self.styles.append({'color': C(color, alpha)})
        return len(self.styles) - 1

    def linear(self, start, end, colors):
        self.styles.append(hvif.linear_gradient(start, end, [
            (i / (len(colors) - 1), C(c)) for i, c in enumerate(colors)]))
        return len(self.styles) - 1

    def radial(self, center, radius, stops, ratio=1):
        self.styles.append(hvif.radial_gradient(center, radius, [
            (offset, C(color, alpha)) for offset, color, alpha in stops], ratio))
        return len(self.styles) - 1

    def shape(self, style, points, detail=False, face=False):
        if face:
            # One affine plane for the paper, header, rings and numerals.
            points = [tuple((x + 7, y + 7 + x * .20)
                            for x, y in hvif.normalize_point(p)) for p in points]
        self.paths.append({'closed': True, 'points': points})
        shape = {'style': style, 'paths': [len(self.paths) - 1]}
        if detail:
            # Encodes exactly as LOD byte 32: absent at 16/32, present at 64+.
            shape['lod'] = (32 / 63.75, 4.0)
        self.shapes.append(shape)

    def ellipse(self, style, cx, cy, rx, ry, **kw):
        self.shape(style, [
            ((cx + rx, cy), (cx + rx, cy - K * ry), (cx + rx, cy + K * ry)),
            ((cx, cy + ry), (cx + K * rx, cy + ry), (cx - K * rx, cy + ry)),
            ((cx - rx, cy), (cx - rx, cy + K * ry), (cx - rx, cy - K * ry)),
            ((cx, cy - ry), (cx - K * rx, cy - ry), (cx + K * rx, cy - ry)),
        ], **kw)

    def rounded(self, style, left, top, right, bottom, r, **kw):
        k = r * (1 - K)
        self.shape(style, [
            ((left + r, top), (left + k, top), (left + r, top)),
            ((right - r, top), (right - r, top), (right - k, top)),
            ((right, top + r), (right, top + k), (right, top + r)),
            ((right, bottom - r), (right, bottom - r), (right, bottom - k)),
            ((right - r, bottom), (right - k, bottom), (right - r, bottom)),
            ((left + r, bottom), (left + r, bottom), (left + k, bottom)),
            ((left, bottom - r), (left, bottom - k), (left, bottom - r)),
            ((left, top + r), (left, top + r), (left, top + k)),
        ], **kw)

    def data(self):
        return dict(styles=self.styles, paths=self.paths, shapes=self.shapes)


def app_icon():
    icon = Icon()
    ink = icon.solid('29323b')
    paper = icon.linear((11, 22), (46, 54), ['fffef7', 'f4f3eb', 'd8dce0'])
    edge = icon.linear((45, 19), (52, 52), ['e6e9e9', '9da8b2'])
    backing = icon.linear((13, 5), (51, 49), ['db7864', '8a3935'])
    coral = icon.linear((12, 9), (42, 27), ['ffae88', 'f26753', 'cc3e3b'])
    red_edge = icon.solid('a43938')
    header_light = icon.solid('ffdfb8', 215)
    white = icon.solid('ffffff', 220)
    line = icon.solid('8a939b', 145)
    perforation = icon.solid('976f65', 135)
    numbers = icon.linear((15, 25), (35, 47), ['354653', '202c39'])
    metal = icon.linear((14, 5), (20, 13), ['ffffff', 'c8d6df', '657884'])
    contact = icon.solid('392b2e', 120)
    shadow = icon.radial((35, 53), 28, [
        (0, '1c2735', 65), (.55, '1c2735', 36), (1, '1c2735', 0)], .28)
    icon.ellipse(shadow, 35, 53, 28, 7.84)

    # A thin dark silhouette surrounds a rigid back and a layered paper pad.
    icon.shape(ink, [(7, 8), (11, 4), (50, 11.8), (52, 14),
                     (52, 50.8), (47, 56), (7, 48)])
    icon.shape(backing, [(8.3, 8.3), (11.4, 5.2), (49.4, 12.8),
                         (50.8, 14.5), (47, 18), (8.3, 10)])
    icon.shape(edge, [(47, 17), (50.8, 14), (50.8, 50.3),
                     (46.8, 54.7), (8.2, 47), (8.2, 44.8)])
    for y in (47.5, 49.5):
        icon.shape(line, [(47.2, y + 3), (50.7, y - .6),
                          (50.7, y), (47.2, y + 3.6)], detail=True)
    icon.shape(ink, [(7, 8), (47.8, 16.1), (47.8, 53.8),
                     (46.4, 55), (7, 47.2)])
    icon.rounded(paper, .9, 1.5, 39.8, 39.7, 1, face=True)
    icon.shape(white, [(1.3, 14), (1.9, 14), (1.9, 38.7),
                       (35, 38.7), (35, 39.2), (1.3, 39.2)], detail=True, face=True)

    # Coral binding with a fine raised upper lip and a tear-off edge.
    icon.shape(red_edge, [(.9, 1.8), (39.8, 1.8), (39.8, 13.4), (.9, 13.4)], face=True)
    icon.shape(coral, [(.9, 1.8), (39.8, 1.8), (39.8, 12.4), (.9, 12.4)], face=True)
    icon.shape(header_light, [(1.4, 1.9), (39.2, 1.9), (39.2, 2.7),
                              (1.4, 2.7)], detail=True, face=True)
    for x in range(4, 38, 4):
        icon.rounded(perforation, x, 14.6, x + 1.6, 15.1, .2, detail=True, face=True)

    # Two substantial steel loops; inset shapes give a controlled thin contour.
    for x in (9, 30):
        icon.ellipse(contact, x + 1.2, 7.1, 3.1, 1.6, face=True)
        icon.rounded(ink, x - 2, -3.1, x + 2.1, 7.7, 2, face=True)
        # Gradient repeats at each ring, in canvas space.
        ring_metal = metal if x == 9 else icon.linear(
            (35, 9.2), (41, 17.2), ['ffffff', 'c8d6df', '657884'])
        icon.rounded(ring_metal, x - 1.05, -2.2, x + 1.15, 6.7, 1.05, face=True)
        icon.rounded(white, x - .85, -1.5, x - .25, 5.5, .3, detail=True, face=True)

    # Hand-built bold '31': gently curved 3 and an unambiguous flagged 1.
    icon.shape(numbers, [
        (6.8, 19.5), ((12.2, 17.7), (8, 17.7), (16.6, 17.7)),
        ((18.8, 22), (18.8, 18.7), (18.8, 24)),
        ((16.5, 25.5), (18.8, 24.4), (19.4, 26.1)),
        ((19.4, 29.2), (19.4, 27), (19.4, 33.2)),
        ((12.3, 34.4), (17.3, 34.4), (9.3, 34.4)),
        (6.6, 32.8), (7.7, 29.6),
        ((12.1, 31), (9.5, 31), (14.9, 31)),
        ((15.4, 28.9), (15.4, 30.5), (15.4, 27.4)),
        (12.2, 27), (10.5, 27), (10.5, 23.9), (12.2, 23.9),
        ((14.9, 22.4), (14.9, 23.9), (14.9, 21)),
        ((12, 20.9), (14.1, 20.9), (10.6, 20.9)),
        (8.1, 22.1),
    ], face=True)
    icon.shape(numbers, [(22, 20.6), (27, 17.9), (30.3, 17.9),
                         (30.3, 30.8), (33.3, 30.8), (33.3, 34.1),
                         (22.6, 34.1), (22.6, 30.8), (26.3, 30.8),
                         (26.3, 22.1), (23.3, 23.7)], face=True)

    # A small enamel-blue task badge floats over the lower-right paper corner.
    badge_shadow = icon.radial((49.5, 49.5), 12, [
        (0, '172536', 100), (.75, '172536', 80), (1, '172536', 0)])
    icon.ellipse(badge_shadow, 49.5, 49.5, 12, 12)
    badge_ink = icon.solid('17364e')
    blue_rim = icon.linear((40, 39), (57, 58), ['a9e7ff', '3e9bd4', '12528c'])
    blue = icon.linear((42, 39), (55, 57), ['58c9f2', '258ed4', '1765b4'])
    icon.ellipse(badge_ink, 50, 49, 10.6, 10.6)
    icon.ellipse(blue_rim, 50, 48.8, 9.6, 9.6)
    icon.ellipse(blue, 50, 48.9, 8.7, 8.7)
    check_shadow = icon.solid('104d85', 160)
    check = [(43.6, 48.5), (45.8, 46.4), (48.9, 49.5),
             (54.6, 43.5), (56.8, 45.6), (48.9, 54.1)]
    icon.shape(check_shadow, [(x, y + .9) for x, y in check], detail=True)
    icon.shape(icon.linear((44, 43), (52, 54), ['ffffff', 'e2f5ff']), check)
    return icon.data()


def render(icon, size):
    # hvif.preview currently ignores shape LOD; match Haiku's scale filtering
    # here without modifying the shared reader/renderer.
    scale = size / 64
    visible = dict(icon, shapes=[s for s in icon['shapes']
                   if s.get('lod', (0, 4))[0] <= scale <= s.get('lod', (0, 4))[1]])
    return hvif.preview(visible, size)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--no-preview', action='store_true', help='write HVIF without Pillow')
    args = parser.parse_args()
    out = Path(__file__).resolve().parent.parent / 'resources'
    out.mkdir(exist_ok=True)
    # Quantization can turn fractional coordinates into compact integers.
    # A second encoding canonicalizes those paths before verifying stability.
    encoded = hvif.encode(hvif.decode(hvif.encode(app_icon())))
    if len(encoded) >= 6000:
        raise ValueError('icon exceeds the 6 KB budget')
    decoded = hvif.decode(encoded)
    if hvif.encode(decoded) != encoded:
        raise ValueError('HVIF round-trip changed the icon')
    target = out / 'airCal.hvif'
    target.write_bytes(encoded)
    outputs = [target]
    if not args.no_preview:
        for size in (256, 32, 16):
            suffix = '' if size == 256 else f'-{size}'
            target = out / f'airCal{suffix}-preview.png'
            render(decoded, size).save(target)
            outputs.append(target)
    for target in outputs:
        print(f'{target.relative_to(out.parent)}: {target.stat().st_size:,} bytes')


if __name__ == '__main__':
    main()
