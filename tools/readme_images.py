"""Frame display screenshots (docs/images/*.png) into README images.

Take the screenshots first, with the bridge stopped:
    python bridge/claude_screen.py --demo --demo-frame 0 --shot docs/images/working.png
    python bridge/claude_screen.py --demo --demo-frame 1 --shot docs/images/needs-you.png
    python bridge/claude_screen.py --demo --demo-frame 2 --shot docs/images/done.png
then:
    python tools/readme_images.py
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

IMG = Path(__file__).resolve().parent.parent / "docs" / "images"
FONT = r"C:\Windows\Fonts\Roboto-Bold.ttf"
LABEL = (138, 133, 120, 255)


def rounded(img, r):
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, *img.size), r, fill=255)
    out = img.convert("RGBA")
    out.putalpha(mask)
    return out


def shadow(size, box, r, blur, alpha):
    s = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(s).rounded_rectangle(box, r, fill=(0, 0, 0, alpha))
    return s.filter(ImageFilter.GaussianBlur(blur))


def device(screen):
    """The screenshot inside a dark bezel, like the board in its case."""
    pad, r = 34, 36
    w, h = screen.width + pad * 2, screen.height + pad * 2
    body = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(body)
    d.rounded_rectangle((0, 0, w - 1, h - 1), r, fill=(32, 31, 29, 255), outline=(58, 56, 51, 255), width=2)
    d.rounded_rectangle((pad - 6, pad - 6, w - pad + 5, h - pad + 5), 10, fill=(8, 8, 8, 255))
    body.alpha_composite(rounded(screen, 6), (pad, pad))
    return body


def hero():
    dev = device(Image.open(IMG / "working.png"))
    m = 60
    canvas = Image.new("RGBA", (dev.width + m * 2, dev.height + m * 2), (0, 0, 0, 0))
    canvas.alpha_composite(shadow(canvas.size, (m + 10, m + 24, m + dev.width - 10, m + dev.height + 10), 40, 26, 120))
    canvas.alpha_composite(dev, (m, m))
    canvas.save(IMG / "hero.png")


def states():
    shots = [("working.png", "Working"), ("needs-you.png", "Needs you"), ("done.png", "Done")]
    sw, sh, gap, m, label_h = 480, 360, 36, 30, 56
    font = ImageFont.truetype(FONT, 26)
    canvas = Image.new("RGBA", (m * 2 + sw * 3 + gap * 2, m * 2 + sh + label_h), (0, 0, 0, 0))
    d = ImageDraw.Draw(canvas)
    for i, (file, label) in enumerate(shots):
        x = m + i * (sw + gap)
        shot = Image.open(IMG / file).resize((sw, sh), Image.LANCZOS)
        canvas.alpha_composite(shadow(canvas.size, (x + 6, m + 12, x + sw - 6, m + sh + 4), 18, 14, 90))
        canvas.alpha_composite(rounded(shot, 18), (x, m))
        d.text((x + sw / 2, m + sh + 36), label, font=font, fill=LABEL, anchor="mm")
    canvas.save(IMG / "states.png")


if __name__ == "__main__":
    hero()
    states()
    print("wrote", IMG / "hero.png", "and", IMG / "states.png")
