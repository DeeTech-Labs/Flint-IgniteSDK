"""Host-only FLIM v2 preparation. Pillow; optional CairoSVG for self-contained SVG."""
import argparse
import io
import math
import re
import struct
import sys
from pathlib import Path
from PIL import Image

FORMATS = {"A4": 1, "INDEX8": 2, "RGB565+A8": 3}


def rgb565(r, g, b):
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def rgba(color, alpha):
    r, g, b = color >> 11, (color >> 5) & 63, color & 31
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31, alpha)


def load_image(path, size=None):
    if size is not None and (len(size) != 2 or any(type(n) is not int or not 1 <= n <= 320 for n in size)):
        raise ValueError("image size must contain two integers in 1..320")
    if path.suffix.lower() == ".svg":
        # No file/network fetches, scripts, CSS imports, or external entity expansion.
        import xml.etree.ElementTree as ET
        data = path.read_bytes()
        if len(data) > 1024 * 1024 or b"<!" in data:
            raise ValueError("SVG declarations and files over 1 MiB are unsupported")
        tree = ET.fromstring(data)
        for node in tree.iter():
            if node.tag.split("}")[-1] in ("script", "foreignObject", "style", "image"):
                raise ValueError("SVG must use self-contained vector shapes")
            for key, value in node.attrib.items():
                if key.split("}")[-1] == "href" and not value.startswith("#"):
                    raise ValueError("external SVG references are unsupported")
                if key.split("}")[-1] == "base" or "@import" in value.lower():
                    raise ValueError("external SVG base/import is unsupported")
                for target in re.findall(r"url\s*\(\s*([^)]*)", value, flags=re.I):
                    if not re.fullmatch(r"[\"']?#[a-zA-Z0-9_.:-]+[\"']?", target.strip()):
                        raise ValueError("external SVG URLs are unsupported")
        if size is None:
            raise ValueError("SVG requires an explicit output size")
        try:
            from cairosvg.surface import PNGSurface
        except (ImportError, OSError) as error:
            raise ValueError("SVG requires host CairoSVG and Cairo; alternatively export PNG") from error
        def deny_fetch(*args, **kwargs):
            raise ValueError("external SVG resource fetching is disabled")
        image = Image.open(io.BytesIO(PNGSurface.convert(bytestring=data,
            output_width=size[0], output_height=size[1], unsafe=False,
            url_fetcher=deny_fetch)))
    else:
        image = Image.open(path)
        if image.width * image.height > 16 * 1024 * 1024:
            raise ValueError("source raster exceeds 16 megapixels")
    image = image.convert("RGBA")
    if size and image.size != tuple(size):
        # Pillow uses premultiplied RGBA for filtered resizing, avoiding color fringes.
        image = image.resize(tuple(size), Image.Resampling.LANCZOS)
    if not 1 <= image.width <= 320 or not 1 <= image.height <= 320:
        raise ValueError("prepared image dimensions must be 1..320")
    return image


def encode(image, representation="RGB565+A8", mask=None, colors=256):
    if representation not in FORMATS:
        raise ValueError("unknown FLIM representation")
    if type(colors) is not int or not 1 <= colors <= 256:
        raise ValueError("palette size must be 1..256")
    image = image.convert("RGBA")
    w, h = image.size
    if not 1 <= w <= 320 or not 1 <= h <= 320:
        raise ValueError("FLIM dimensions must be 1..320")
    pixels = list(image.getdata())
    palette = []
    payload = bytearray()
    preview = Image.new("RGBA", (w, h))
    decoded = []
    if representation == "A4":
        if mask not in ("alpha", "luminance"):
            raise ValueError("A4 requires explicit mask=alpha or luminance")
        levels = [((a if mask == "alpha" else
                    ((r * 299 + g * 587 + b * 114 + 500) // 1000) * a // 255) * 15 + 127) // 255
                  for r, g, b, a in pixels]
        for y in range(h):
            for x in range(0, w, 2):
                payload.append(levels[y*w+x] << 4 | (levels[y*w+x+1] if x+1 < w else 0))
        decoded = [(255, 255, 255, a * 17) for a in levels]
    else:
        if mask is not None:
            raise ValueError("mask is valid only with A4")
        pairs = [(rgb565(r, g, b) if a else 0, a) for r, g, b, a in pixels]
        if representation == "INDEX8":
            # Exact palette first; combinations of RGB565 and A8 are distinct entries.
            palette = list(dict.fromkeys(pairs))
            if len(palette) > colors:
                quantized = image.quantize(colors=colors, method=Image.Quantize.FASTOCTREE,
                    dither=Image.Dither.NONE).convert("RGBA")
                pairs = [(rgb565(r, g, b) if a else 0, a) for r, g, b, a in quantized.getdata()]
                palette = list(dict.fromkeys(pairs))
            lookup = {p: i for i, p in enumerate(palette)}
            payload.extend(lookup[p] for p in pairs)
        else:
            for c, a in pairs:
                payload.extend(struct.pack("<HB", c, a))
        decoded = [rgba(c, a) for c, a in pairs]
    preview.putdata(decoded)
    data = struct.pack("<4sBBHHHI", b"FLIM", 2, FORMATS[representation], w, h, len(palette), 0)
    data += b"".join(struct.pack("<HB", c, a) for c, a in palette) + payload
    return data, preview


def convert(path, options, root):
    representation = options.get("representation", "RGB565+A8")
    image = load_image(path, options.get("size"))
    data, preview = encode(image, representation, options.get("mask"), options.get("colors", 256))
    # Quality is measured on black and white to include alpha errors, not hidden RGB.
    error = 0
    for original, result in zip(image.getdata(), preview.getdata()):
        for bg in (0, 255):
            for channel in range(3):
                a = (original[channel] * original[3] + bg * (255-original[3])) / 255
                b = (result[channel] * result[3] + bg * (255-result[3])) / 255
                error += (a-b)**2
    rmse = math.sqrt(error / (image.width*image.height*6)) if representation != "A4" else None
    print(f"FLIM {path.name}: {image.width}x{image.height} {representation}, {len(data)} bytes, "
          + (f"composite RMSE={rmse:.3f}/255" if rmse is not None else "explicit thematic mask"), file=sys.stderr)
    if options.get("preview"):
        destination = (root / options["preview"]).resolve()
        if not destination.is_relative_to(root.resolve()):
            raise ValueError("preview escapes project")
        preview.save(destination)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--representation", choices=FORMATS, default="RGB565+A8")
    parser.add_argument("--size", type=int, nargs=2)
    parser.add_argument("--mask", choices=("alpha", "luminance"))
    parser.add_argument("--colors", type=int, default=256)
    parser.add_argument("--preview")
    args = parser.parse_args()
    args.output.write_bytes(convert(args.source, vars(args), Path.cwd()))


if __name__ == "__main__":
    main()
