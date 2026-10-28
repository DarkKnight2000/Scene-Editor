"""Draws the editor's icon: res/SceneEditor.ico (embedded in the Windows executable) and
res/icon.png (a 256 px preview).

A flat-colour owl face in the editor's blue, on the editor's dark tile.

    python tools/make_icon.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

SIZE = 256  # design units: everything below is drawn on a 256 x 256 grid
SCALE = 4   # drawn at 4x, then scaled down for smooth edges

TILE_TOP = (40, 43, 51)
TILE_BOTTOM = (20, 21, 24)  # theme::kBackground
TILE_EDGE = (66, 71, 84)
HEAD = (63, 110, 222)
EARS = (44, 80, 176)
FOREHEAD = (36, 66, 150)
FACE = (205, 222, 255)
EYE = (250, 251, 253)
PUPIL = (24, 26, 33)
BEAK = (255, 177, 41)


def draw(size):
    s = SCALE * size / SIZE  # pixels per design unit
    big = SCALE * size
    # Small icons: bigger pupils and less margin, so they stay readable.
    bold = 1.5 if size <= 24 else 1.2 if size <= 40 else 1.0
    margin = 8 if size > 40 else 2
    image = Image.new("RGBA", (big, big), (0, 0, 0, 0))

    def box(x0, y0, x1, y1):
        return [round(x0 * s), round(y0 * s), round(x1 * s), round(y1 * s)]

    def points(*xy):
        return [(round(x * s), round(y * s)) for x, y in xy]

    # Tile: a rounded square with a vertical gradient and a thin lighter edge.
    gradient = Image.new("RGBA", (big, big))
    pixels = gradient.load()
    for y in range(big):
        t = y / (big - 1)
        colour = tuple(round(a + (b - a) * t) for a, b in zip(TILE_TOP, TILE_BOTTOM)) + (255,)
        for x in range(big):
            pixels[x, y] = colour
    mask = Image.new("L", (big, big), 0)
    tile = box(margin, margin, SIZE - margin, SIZE - margin)
    ImageDraw.Draw(mask).rounded_rectangle(tile, radius=round(52 * s), fill=255)
    image.paste(gradient, (0, 0), mask)
    d = ImageDraw.Draw(image)
    d.rounded_rectangle(tile, radius=round(52 * s), outline=TILE_EDGE, width=round(5 * s * bold))

    # Ear tufts, then the head: a wide rounded shape.
    d.polygon(points((42, 112), (50, 36), (110, 78)), fill=EARS)
    d.polygon(points((214, 112), (206, 36), (146, 78)), fill=EARS)
    d.rounded_rectangle(box(36, 64, 220, 222), radius=round(78 * s), fill=HEAD)

    # Forehead feathers: a small chevron.
    if size >= 24:
        d.polygon(points((108, 80), (128, 94), (148, 80), (148, 90), (128, 104), (108, 90)), fill=FOREHEAD)

    # Face discs and eyes, looking a little to the right.
    for cx in (88, 168):
        d.ellipse(box(cx - 46, 104, cx + 46, 196), fill=FACE)
        d.ellipse(box(cx - 34, 116, cx + 34, 184), fill=EYE)
        p = 17 * bold
        d.ellipse(box(cx + 4 - p, 152 - p, cx + 4 + p, 152 + p), fill=PUPIL)
        if size >= 32:
            d.ellipse(box(cx + 7, 136, cx + 17, 146), fill=EYE)

    # Beak.
    d.polygon(points((114, 172), (142, 172), (128, 204)), fill=BEAK)

    return image.resize((size, size), Image.LANCZOS)


def main():
    out = Path(__file__).resolve().parent.parent / "res"
    out.mkdir(exist_ok=True)
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = {size: draw(size) for size in sizes}
    images[256].save(out / "icon.png")
    images[256].save(out / "SceneEditor.ico", sizes=[(n, n) for n in sizes],
                     append_images=[images[n] for n in sizes if n != 256])
    print("wrote", out / "SceneEditor.ico", "and", out / "icon.png")


if __name__ == "__main__":
    main()
