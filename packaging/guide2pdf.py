#!/usr/bin/env python3
"""A guide in docs/*.md -> a print-ready HTML page, for `make guide`.

    guide2pdf.py docs/<name>.md out.html <version>

The Markdown is the only source: diagrams are SVG files it references as
ordinary images (docs/img/), so GitHub shows the same pictures the PDF
prints. This script knows nothing about any one guide. Chrome does the
printing (the Makefile runs it headless); needs python3-markdown.

A title written "Kicker: headline" is split into a small line and a big one.
"""
import pathlib
import re
import sys

import markdown

REPO = "https://github.com/Hyperwindmill/v7dvs"
ACCENT, INK, MUTE, LINK = "#c2410c", "#0f172a", "#64748b", "#1d4ed8"

CSS = f"""
@page {{ size: A4; margin: 14mm 16mm 14mm; }}
* {{ box-sizing: border-box; }}
body {{ font: 10pt/1.45 "Noto Sans", "DejaVu Sans", sans-serif; color: {INK}; margin: 0; }}
header {{ border-bottom: 3px solid {ACCENT}; padding-bottom: 10px; margin-bottom: 14px; }}
header .kicker {{ font-size: 8.5pt; letter-spacing: .12em; text-transform: uppercase;
                  color: {ACCENT}; font-weight: 700; }}
h1 {{ font-size: 21pt; line-height: 1.2; margin: 4px 0 0; text-wrap: balance; }}
h2 {{ font-size: 13.5pt; margin: 17px 0 5px; padding-top: 2px; break-after: avoid; }}
h3 {{ font-size: 11pt; margin: 16px 0 4px; break-after: avoid; }}
p {{ margin: 6px 0; }}
ol, ul {{ margin: 6px 0; padding-left: 20px; }}
li {{ margin: 3px 0; }}
a {{ color: {LINK}; text-decoration: none; }}
code {{ font: 9.2pt "DejaVu Sans Mono", monospace; background: #f1f5f9;
        padding: 1px 4px; border-radius: 3px; }}
pre {{ background: #f1f5f9; padding: 8px 10px; border-radius: 4px; break-inside: avoid; }}
pre code {{ padding: 0; }}
table {{ border-collapse: collapse; width: 100%; margin: 8px 0 10px; font-size: 9.5pt; }}
th {{ text-align: left; background: #f1f5f9; border-bottom: 1.5px solid {INK}; padding: 5px 8px; }}
td {{ border-bottom: 1px solid #cbd5e1; padding: 5px 8px; vertical-align: top; }}
tr, table, ol, ul {{ break-inside: avoid; }}
/* a diagram stays with the sentence that introduces it, and that with its heading */
p:has(+ p > img) {{ break-after: avoid; }}
img {{ width: 100%; height: auto; display: block; margin: 10px 0 12px; break-inside: avoid; }}
footer {{ margin-top: 18px; border-top: 1px solid #cbd5e1; padding-top: 6px;
          font-size: 8.5pt; color: {MUTE}; }}
"""


def render(src, version):
    src = pathlib.Path(src).resolve()
    md = src.read_text(encoding="utf-8")
    title = re.match(r"# (.+)\n", md)
    if not title:
        sys.exit(f"{src}: the first line must be the '# title'")
    md = md[title.end():]
    # A link into the repo means nothing on paper: point it at GitHub.
    md = re.sub(r"\]\(\.\./([^)]+)\)", rf"]({REPO}/blob/main/\1)", md)
    kicker, sep, headline = title.group(1).partition(": ")
    if not sep:
        kicker, headline = "", kicker
    headline = headline[0].upper() + headline[1:]
    body = markdown.markdown(md, extensions=["tables", "fenced_code"])
    # <base> lets the images keep the relative paths GitHub also resolves.
    return f"""<!doctype html><html lang="en"><head><meta charset="utf-8">
<base href="{src.parent.as_uri()}/">
<title>v7dvs: {title.group(1)}</title><style>{CSS}</style></head><body>
<header><div class="kicker">v7dvs {version}{" · " + kicker if kicker else ""}</div>
<h1>{headline}</h1></header>
{body}
<footer>v7dvs {version} · GPLv3 · {REPO.removeprefix("https://")}</footer>
</body></html>"""


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    pathlib.Path(sys.argv[2]).write_text(render(sys.argv[1], sys.argv[3]), encoding="utf-8")
