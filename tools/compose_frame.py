"""Compose a device-framed screenshot for the README.

Geometry and colours follow the Dial hardware frame from dial.html:
360x360 body, orange bezel inset 13, 240x240 screen at (57,57), notch at top.
Rendered at 2x for crisp README images.
"""
import os, sys, math
from PIL import Image, ImageDraw, ImageFilter

S = 2                     # render scale
BODY = 360 * S
C = BODY // 2
OUT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "docs")


def sc(v):
    return int(round(v * S))


def frame(screen_path, out_path):
    screen = Image.open(screen_path).convert("RGBA")
    if screen.size != (240, 240):
        screen = screen.resize((240, 240), Image.LANCZOS)

    img = Image.new("RGBA", (BODY, BODY), (0, 0, 0, 0))

    # drop shadow
    sh = Image.new("RGBA", (BODY, BODY + sc(24)), (0, 0, 0, 0))
    d = ImageDraw.Draw(sh)
    d.ellipse([sc(10), sc(26), BODY - sc(10), BODY - sc(6)], fill=(32, 35, 32, 70))
    sh = sh.filter(ImageFilter.GaussianBlur(sc(7)))
    img.alpha_composite(sh, (0, 0))

    d = ImageDraw.Draw(img)
    # body
    d.ellipse([sc(1.5), sc(1.5), BODY - sc(1.5), BODY - sc(1.5)], fill=(98, 101, 98, 255), outline=(116, 119, 114, 255), width=sc(3))

    # rotor ticks
    for i in range(60):
        a = math.radians(i * 6)
        x0 = C + math.cos(a) * sc(168)
        y0 = C + math.sin(a) * sc(168)
        x1 = C + math.cos(a) * sc(177)
        y1 = C + math.sin(a) * sc(177)
        d.line([x0, y0, x1, y1], fill=(155, 158, 152, 255), width=sc(1.6))

    # orange bezel
    d.ellipse([sc(13), sc(13), BODY - sc(13), BODY - sc(13)], fill=(244, 141, 19, 255), outline=(229, 121, 10, 255), width=sc(2))
    # inner highlight
    d.arc([sc(15), sc(15), BODY - sc(15), BODY - sc(15)], 200, 340, fill=(255, 224, 160, 120), width=sc(2))

    # notch
    notch = Image.new("RGBA", (sc(24), sc(24)), (0, 0, 0, 0))
    nd = ImageDraw.Draw(notch)
    nd.rectangle([0, 0, sc(20), sc(20)], outline=(198, 102, 8, 255), width=sc(2))
    notch = notch.rotate(45, expand=False)
    img.alpha_composite(notch, (C - sc(10), sc(12)))

    # screen
    ring = ImageDraw.Draw(img)
    ring.ellipse([sc(54), sc(54), sc(54) + sc(240), sc(54) + sc(240)], fill=(52, 55, 49, 255))
    scr = screen.resize((sc(240), sc(240)), Image.LANCZOS)
    mask = Image.new("L", (sc(240), sc(240)), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, sc(240) - 1, sc(240) - 1], fill=255)
    img.paste(scr, (sc(57), sc(57)), mask)

    # button under the screen
    btn = [sc(140), sc(300), sc(140) + sc(74), sc(300) + sc(44)]
    d.rounded_rectangle(btn, radius=sc(18), fill=(154, 81, 11, 255), outline=(229, 121, 10, 255), width=sc(1.5))

    img.save(out_path)
    print("wrote", out_path)


if __name__ == "__main__":
    pairs = [
        ("work/screen_center.png", "dial-idle.png"),
        ("work/screen_work_center.png", "dial-working.png"),
    ]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for src, dst in pairs:
        frame(os.path.join(root, src), os.path.join(OUT_DIR, dst))
