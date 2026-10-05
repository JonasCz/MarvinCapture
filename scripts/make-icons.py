#!/usr/bin/env python3
"""Regenerates the app icons from docs/images/logo.svg (transparent background, no tile):

  gui/windows/MarvinCaptureGUI/Assets/AppIcon.ico   16..256 px, PNG-compressed frames
  gui/macos/Resources/AppIcon.iconset/*.png         the ten iconset sizes (scripts/build.sh runs iconutil)

The SVG is rasterised at every size with headless Edge (present on every Windows 10/11 machine), so
nothing has to be installed except Pillow (pip install pillow), which writes the .ico.
Usage: python scripts/make-icons.py
"""
import io, os, subprocess, sys, tempfile
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SVG = ROOT / "docs" / "images" / "logo.svg"
ICO = ROOT / "gui" / "windows" / "MarvinCaptureGUI" / "Assets" / "AppIcon.ico"
ICONSET = ROOT / "gui" / "macos" / "Resources" / "AppIcon.iconset"
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
ICONSET_FILES = {  # file name -> pixels
    "icon_16x16.png": 16, "icon_16x16@2x.png": 32, "icon_32x32.png": 32, "icon_32x32@2x.png": 64,
    "icon_128x128.png": 128, "icon_128x128@2x.png": 256, "icon_256x256.png": 256,
    "icon_256x256@2x.png": 512, "icon_512x512.png": 512, "icon_512x512@2x.png": 1024,
}

def find_edge():
    for base in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")):
        if base:
            p = Path(base) / "Microsoft" / "Edge" / "Application" / "msedge.exe"
            if p.exists():
                return str(p)
    sys.exit("msedge.exe not found")

def render(edge, svg_text, px, tmp):
    """The SVG at px x px on a fully transparent page."""
    html = tmp / f"logo{px}.html"
    png = tmp / f"logo{px}.png"
    html.write_text(f'<!doctype html><meta charset="utf-8"><style>html,body{{margin:0;background:transparent;overflow:hidden}}'
                    f'svg{{display:block;width:{px}px;height:{px}px}}</style>{svg_text}', encoding="utf-8")
    subprocess.run([edge, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                    "--default-background-color=00000000", f"--window-size={px},{px}",
                    f"--user-data-dir={tmp / 'profile'}", f"--screenshot={png}", html.as_uri()],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    img = Image.open(png).convert("RGBA")
    if img.size != (px, px):
        img = img.crop((0, 0, px, px))
    return img

def main():
    edge = find_edge()
    svg_text = SVG.read_text(encoding="utf-8")
    svg_text = svg_text[svg_text.index("<svg"):].replace(' width="64" height="64"', "", 1)
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        sizes = sorted(set(ICO_SIZES) | set(ICONSET_FILES.values()))
        imgs = {px: render(edge, svg_text, px, tmp) for px in sizes}
    for px, img in imgs.items():
        if img.getchannel("A").getextrema()[0] != 0:
            sys.exit(f"{px} px render has no transparent pixel: Edge ignored the transparent background")
    ICO.parent.mkdir(parents=True, exist_ok=True)
    big = imgs[256]
    # Pillow resamples `big` unless append_images supplies each size: give it the exact renders.
    big.save(ICO, format="ICO", sizes=[(s, s) for s in ICO_SIZES], append_images=[imgs[s] for s in ICO_SIZES if s != 256])
    ICONSET.mkdir(parents=True, exist_ok=True)
    for name, px in ICONSET_FILES.items():
        imgs[px].save(ICONSET / name, optimize=True)
    print(f"wrote {ICO.relative_to(ROOT)} and {len(ICONSET_FILES)} PNGs in {ICONSET.relative_to(ROOT)}")

if __name__ == "__main__":
    main()
