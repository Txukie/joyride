#!/usr/bin/env python3
"""The controller scheme's pictures, from art/controls.svg (drawn for a
light page: black on transparent).

  art/controls-light.png   as drawn: for light pages (the README in light mode)
  art/controls-dark.png    recoloured for dark pages: the launcher's home page,
                           the README in dark mode. Labels and icons light,
                           leader lines dimmer, the controller's body lifted
                           off the dark so its shape shows

Both at twice the drawing's size, trimmed to what is drawn. Needs Inkscape
and Pillow.   python3 tools/controls_art.py
"""
import re, subprocess, sys, tempfile
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SVG = ROOT / "art" / "controls.svg"

LIGHT = "#e5e7eb"      # labels, icons (the launcher's text, a touch softer)
LINE = "#9ca3af"       # leader lines and their dots (the launcher's dim)
HOLE = "#15181d"       # what was white inside an icon (a trigger's letter)
BODY = "#323843"       # the controller's black
GREY = "#687280"       # the controller's grey
BLACKS = r"#000000|#000\b|#111111|#212529|rgb\(0, ?0, ?0\)"
WHITES = r"#ffffff|#fff\b|#fdf6fd"


def recolour(svg: str) -> str:
    def element(m):
        el = m.group(0)
        cls = re.search(r'class="([^"]*)"', el)
        cls = cls.group(1) if cls else ""
        if "invizCircle" in cls:
            return el
        if "padBlack" in cls:
            return re.sub(r"#242424", BODY, el)
        if "padGrey" in cls:
            return re.sub(r"#606a6e", GREY, el)
        if "padWhite" in cls:
            return el
        tag = re.match(r"<(\w+)", el).group(1)
        # a line, or a path drawn by its stroke only: a leader line
        lead = tag == "line" or (re.search(r"(?:^|[;\s\"])fill:none", el) and re.search(r"stroke:(?:" + BLACKS + ")", el))
        el = re.sub(BLACKS, LINE if lead else LIGHT, el)
        return re.sub(WHITES, HOLE, el)
    # each drawing element's opening tag (not the document's own settings)
    return re.sub(r"<(?:path|line|circle|rect|polygon|polyline|ellipse|text|tspan)\b[^>]*>", element, svg)


def render(svg_text: str, out: Path):
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / "c.svg"
        png = Path(d) / "c.png"
        src.write_text(svg_text)
        subprocess.run(["inkscape", str(src), "--export-type=png", "--export-dpi=192",
                        f"--export-filename={png}"], check=True, capture_output=True)
        im = Image.open(png).convert("RGBA")
        im = im.crop(im.getchannel("A").getbbox())
        pad = 8
        canvas = Image.new("RGBA", (im.width + 2 * pad, im.height + 2 * pad), (0, 0, 0, 0))
        canvas.alpha_composite(im, (pad, pad))
        canvas.save(out, optimize=True)
        print(out.relative_to(ROOT), canvas.size)


def main():
    svg = SVG.read_text()
    render(svg, ROOT / "art" / "controls-light.png")
    render(recolour(svg), ROOT / "art" / "controls-dark.png")


if __name__ == "__main__":
    sys.exit(main())
