"""Generate the two authored factory icons and their explicit thematic masks."""
import argparse
import io
import sys
sys.dont_write_bytecode = True
from generation import write_if_changed
import math
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
SCALE = 4
WHITE = (250, 252, 255, 255)


def box(values):
    return tuple(round(value * SCALE) for value in values)


def clock(image, color):
    draw = ImageDraw.Draw(image)
    draw.ellipse(box((9, 9, 39, 39)), outline=color, width=3 * SCALE)
    draw.line([box((24, 15)), box((24, 24)), box((31, 28))], fill=color, width=3 * SCALE)
    for x, y in ((24, 15), (24, 24), (31, 28)):
        draw.ellipse(box((x-1.5, y-1.5, x+1.5, y+1.5)), fill=color)


def gear(image, color, hole):
    draw = ImageDraw.Draw(image)
    points = []
    for step in range(40):
        angle = 2 * math.pi * step / 40
        radius = 18 if step % 4 < 2 else 14
        points.append(box((24 + radius * math.cos(angle), 24 + radius * math.sin(angle))))
    draw.polygon(points, fill=color)
    draw.ellipse(box((17, 17, 31, 31)), fill=hole)


def write(directory, name, image):
    path = directory / name
    stream = io.BytesIO()
    image.resize((48, 48), Image.Resampling.LANCZOS).save(stream, format="PNG", optimize=True)
    write_if_changed(path, stream.getvalue())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/generated/system-icons")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, background in (('clock', (27, 129, 218, 255)), ('settings', (109, 96, 183, 255))):
        image = Image.new('RGBA', (48*SCALE, 48*SCALE))
        ImageDraw.Draw(image).rounded_rectangle(box((1, 1, 47, 47)), radius=13*SCALE, fill=background)
        mask = Image.new('RGBA', image.size)
        if name == 'clock':
            clock(image, WHITE)
            clock(mask, WHITE)
        else:
            gear(image, WHITE, background)
            gear(mask, WHITE, (0, 0, 0, 0))
        write(args.output, name + '.png', image)
        write(args.output, name + '-mask.png', mask)


if __name__ == '__main__':
    main()
