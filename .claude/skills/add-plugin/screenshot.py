"""A plugin card's screenshot.png for co-op's plugin list.

  python -I screenshot.py <image> <out.png>          convert any image Pillow reads (png, jpg, gif, webp)
  python -I screenshot.py --card "<Name>" <out.png>  a plain name card, for a plugin with no screen image

The result is an RGB PNG of at most 1280 px on its long side and under 1 MB (co-op's limits are 2048 px and
1 MB). Needs Pillow.
"""
import io
import sys

from PIL import Image, ImageDraw, ImageFont

MAX_SIDE = 1280
MAX_BYTES = 1024 * 1024


def card(name):
    im = Image.new('RGB', (640, 320), (20, 20, 20))
    d = ImageDraw.Draw(im)
    font = ImageFont.load_default(size=56)
    box = d.textbbox((0, 0), name, font=font)
    d.text(((640 - (box[2] - box[0])) / 2, (320 - (box[3] - box[1])) / 2 - box[1]), name, fill=(235, 235, 235), font=font)
    return im


def png(im):
    im = im.convert('RGB')
    side = MAX_SIDE
    while True:
        im.thumbnail((side, side))
        buf = io.BytesIO()
        im.save(buf, 'PNG', optimize=True)
        if buf.tell() <= MAX_BYTES:
            return buf.getvalue()
        side = int(side * 0.8)


def main(argv):
    if len(argv) == 3 and argv[0] == '--card':
        data, out = png(card(argv[1])), argv[2]
    elif len(argv) == 2:
        data, out = png(Image.open(argv[0])), argv[1]
    else:
        sys.exit(__doc__)
    with open(out, 'wb') as f:
        f.write(data)
    print(f'{out}: {len(data)} bytes')


if __name__ == '__main__':
    main(sys.argv[1:])
