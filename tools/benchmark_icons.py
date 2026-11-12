"""Prepare reproducible 48px samples and run native file-read/decoder host timings."""
import argparse
import subprocess
from pathlib import Path
from PIL import Image, ImageDraw
from flim import encode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("decoder_test", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    samples = {}
    samples["monitor"] = Image.open(Path(__file__).resolve().parents[1] / "examples/icons/main.png").convert("RGBA")
    ring = Image.new("RGBA", (192,192))
    ImageDraw.Draw(ring).ellipse((12,12,179,179), outline=(250,200,40,255),width=24)
    samples["ring"] = ring.resize((48,48),Image.Resampling.LANCZOS)
    gradient = Image.new("RGBA", (48,48))
    gradient.putdata([(x*255//47,y*255//47,(x+y)*255//94,(x*7+y*3)%256) for y in range(48) for x in range(48)])
    samples["gradient"] = gradient
    fixtures=[]
    for name, image in samples.items():
        for fmt in ("A4","INDEX8","RGB565+A8"):
            data,preview=encode(image,fmt,"alpha" if fmt=="A4" else None)
            path=args.output/(name+"-"+fmt.replace("+","_")+".flim")
            path.write_bytes(data);preview.save(path.with_suffix(".png"));fixtures.append(path.resolve())
    subprocess.run([str(args.decoder_test.resolve()),*map(str,fixtures)],check=True)


if __name__=="__main__":
    main()
