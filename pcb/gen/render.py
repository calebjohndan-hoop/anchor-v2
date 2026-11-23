"""Render KiCad SVG exports to PNG on a white or dark background."""
import sys
from playwright.sync_api import sync_playwright


def svg_to_png(svg_path, png_path, width=900, bg="#ffffff"):
    svg = open(svg_path, encoding="utf8").read()
    svg = svg.replace("<svg", '<svg style="width:100%;height:auto;display:block"', 1)
    html = '<html><body style="margin:0;background:' + bg + '"><div style="width:' + str(width) + 'px">' + svg + "</div></body></html>"
    with sync_playwright() as p:
        b = p.chromium.launch()
        pg = b.new_page(viewport={"width": width, "height": 400}, device_scale_factor=1)
        pg.set_content(html)
        pg.locator("div").first.screenshot(path=png_path)
        b.close()


if __name__ == "__main__":
    svg_to_png(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 900)
