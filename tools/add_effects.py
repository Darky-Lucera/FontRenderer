#!/usr/bin/env python3
"""Adds an outline, a drop shadow and a bevel to a texture saved by Font::SaveBaked.

Reads the 8-bit grayscale TGA, where each texel is the coverage of a glyph, and writes a 32-bit TGA with alpha
that FontBaked reads as a BGRA32 texture. The glyphs stay white, so the color passed to DrawText tints them,
while the outline stays dark.

The effects only fit if the glyphs were baked with enough padding (Font::SetGlyphPadding), and the shadow and the
bevel only look right if they were baked without rotation (Font::SetAllowRotation(false)).

Needs numpy and Pillow:  python -m pip install numpy pillow
Usage:                   python tools/add_effects.py [input.tga] [output.tga]
"""

import sys

import numpy as np
from PIL import Image, ImageFilter

INPUT_PATH      = 'bin/resources/effects.tga'
OUTPUT_PATH     = 'bin/resources/effectfx.tga'

OUTLINE_RADIUS  = 2                         # Pixels
OUTLINE_COLOR   = (0x1a, 0x10, 0x33)
SHADOW_OFFSET   = (0, 4)                    # Pixels to the right and down
SHADOW_BLUR     = 1.2                       # Gaussian radius, in pixels
SHADOW_OPACITY  = 0.6
BEVEL_BLUR      = 1.5                       # Softness of the height map the light falls on
BEVEL_STRENGTH  = 2.5
BEVEL_DARKEST   = 0.55                      # Brightness of the fill where it faces fully down


def blur(alpha, radius):
    image = Image.fromarray((np.clip(alpha, 0.0, 1.0) * 255.0).astype(np.uint8))
    return np.asarray(image.filter(ImageFilter.GaussianBlur(radius)), dtype=np.float32) / 255.0


# Unlike np.roll, what comes in from the edges is transparent.
def shift(alpha, dx, dy):
    height, width = alpha.shape
    moved = np.zeros_like(alpha)
    moved[max(dy, 0):height + min(dy, 0), max(dx, 0):width + min(dx, 0)] = \
        alpha[max(-dy, 0):height + min(-dy, 0), max(-dx, 0):width + min(-dx, 0)]
    return moved


# Grows the glyphs by a disk of the radius. The pixels on the rim of the disk count partially, so the outline
# keeps smooth edges.
def dilate(alpha, radius):
    grown = np.zeros_like(alpha)
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            distance = (dx * dx + dy * dy) ** 0.5
            if distance <= radius + 0.5:
                grown = np.maximum(grown, shift(alpha, dx, dy) * min(1.0, radius + 1.0 - distance))
    return grown


# With the light from above, the fill gets darker where the glyph edge faces down.
def bevel(coverage):
    heights = blur(coverage, BEVEL_BLUR)
    slope = np.zeros_like(heights)
    slope[1:-1] = (heights[2:] - heights[:-2]) * 0.5
    facing_down = np.clip(-slope * BEVEL_STRENGTH, 0.0, 1.0)
    return 1.0 - facing_down * (1.0 - BEVEL_DARKEST)


# Porter-Duff "over" with straight alpha, as the texture stores it.
def over(top_color, top_alpha, bottom_color, bottom_alpha):
    alpha = top_alpha + bottom_alpha * (1.0 - top_alpha)
    color = top_color * top_alpha[..., None] + bottom_color * (bottom_alpha * (1.0 - top_alpha))[..., None]
    return color / np.maximum(alpha, 1e-6)[..., None], alpha


def main():
    input_path  = sys.argv[1] if len(sys.argv) > 1 else INPUT_PATH
    output_path = sys.argv[2] if len(sys.argv) > 2 else OUTPUT_PATH

    coverage = np.asarray(Image.open(input_path).convert('L'), dtype=np.float32) / 255.0
    height, width = coverage.shape

    outline = dilate(coverage, OUTLINE_RADIUS)
    shadow  = blur(shift(outline, *SHADOW_OFFSET), SHADOW_BLUR) * SHADOW_OPACITY

    black   = np.zeros((height, width, 3), dtype=np.float32)
    dark    = np.broadcast_to(np.array(OUTLINE_COLOR, dtype=np.float32) / 255.0, (height, width, 3))
    fill    = np.repeat(bevel(coverage)[..., None], 3, axis=2)

    color, alpha = over(dark, outline, black, shadow)
    color, alpha = over(fill, coverage, color, alpha)

    rgba = np.dstack([color, alpha[..., None]])
    Image.fromarray((np.clip(rgba, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8), 'RGBA').save(output_path, compression='tga_rle')
    print(f'Saved {output_path}, {width} x {height}.')


if __name__ == '__main__':
    main()
