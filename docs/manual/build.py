#!/usr/bin/env python3
"""Assemble le manuel : couverture + sommaire paginé + corps, puis PDF (WeasyPrint).
   Usage : build.py <corps.html> <sortie.pdf> <fr|en>"""
import re, sys, subprocess, unicodedata, pathlib

body_path, out_pdf, lang = sys.argv[1], sys.argv[2], sys.argv[3]
here = pathlib.Path(body_path).parent
html = pathlib.Path(body_path).read_text(encoding='utf-8')

T = {
    'fr': dict(title="Polar Doctor", sub="Manuel d'installation et d'utilisation",
               tag="Polaires de performance d'un voilier,<br>à partir de vos données de navigation réelles",
               meta="Version 2.1.2 — septembre 2026", toc="Sommaire", foot="Polar Doctor — Manuel"),
    'en': dict(title="Polar Doctor", sub="Installation and user manual",
               tag="Sailing-boat performance polars,<br>built from your own sailing data",
               meta="Version 2.1.2 — September 2026", toc="Contents", foot="Polar Doctor — Manual"),
}[lang]

def slug(s):
    s = unicodedata.normalize('NFKD', s).encode('ascii', 'ignore').decode()
    return re.sub(r'[^a-z0-9]+', '-', s.lower()).strip('-')

# Numérote et ancre les titres, et collecte le sommaire.
toc = []
def anchor(m):
    tag, attrs, text = m.group(1), m.group(2), m.group(3)
    plain = re.sub(r'<[^>]+>', '', text)
    sid = slug(plain)
    toc.append((tag, sid, plain))
    return '<%s%s id="%s">%s</%s>' % (tag, attrs, sid, text, tag)
html = re.sub(r'<(h[12])([^>]*)>(.*?)</\1>', anchor, html, flags=re.S)

rows = []
for tag, sid, text in toc:
    rows.append('<div class="toc%s"><a href="#%s">%s</a></div>' % (tag[1], sid, text))

cover = """<div class="cover">
  <div class="band"><div class="ctitle">%(title)s</div>
  <div class="csub">%(sub)s</div></div>
  <div class="ctag">%(tag)s</div>
  <div class="cmeta">%(meta)s<br>https://github.com/ozolli/polar_doctor<br>MIT</div>
</div>
<div class="toc-page"><h1 class="tochead">%(toc)s</h1>%(rows)s</div>
""" % dict(T, rows=''.join(rows))

html = html.replace('<body>', '<body>' + cover, 1)
html = html.replace('<link rel="stylesheet" href="style.css">',
                    '<link rel="stylesheet" href="style.css"><link rel="stylesheet" href="print.css">', 1)

final = here / ('final-%s.html' % lang)
final.write_text(html, encoding='utf-8')
subprocess.run(['weasyprint', '-u', str(here) + '/', str(final), out_pdf], check=True)
print('écrit :', out_pdf)
