"""Reproduce the small author-owned icon examples using Pillow (host only)."""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent


def monitor(background, line):
    image = Image.new("RGBA", (192, 192))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((8, 8, 183, 183), radius=40, fill=background)
    draw.rounded_rectangle((36, 42, 155, 129), radius=10, outline=line, width=10)
    draw.line((96, 129, 96, 150), fill=line, width=10)
    draw.line((66, 151, 126, 151), fill=line, width=10)
    draw.line((52, 103, 75, 78, 95, 98, 130, 66), fill=line, width=8)
    return image.resize((48, 48), Image.Resampling.LANCZOS)


def main():
    monitor((27, 97, 180, 255), (230, 250, 255, 255)).save(ROOT / "main.png")
    monitor((228, 240, 250, 255), (20, 62, 120, 255)).save(ROOT / "light.png")
    monitor((18, 38, 65, 255), (122, 222, 244, 255)).save(ROOT / "dark.png")
    monitor((0, 0, 0, 0), (255, 255, 255, 255)).save(ROOT / "mask.png")


if __name__ == "__main__":
    main()
