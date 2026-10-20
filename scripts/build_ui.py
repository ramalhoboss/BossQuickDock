#!/usr/bin/env python3
"""BossQuickDock: build src/web/index.html from src/web/ui.html, inlining the
brand images (assets/brand/*.jpg) as data URIs. Run before `make webpage`."""
import base64
def uri(p): return "data:image/jpeg;base64," + base64.b64encode(open(p, "rb").read()).decode()
s = open("src/web/ui.html", encoding="utf-8").read()
s = s.replace("{{HERO}}", uri("assets/brand/hero.jpg")).replace("{{MINI}}", uri("assets/brand/mini.jpg"))
open("src/web/index.html", "w", encoding="utf-8", newline="\n").write(s)
print("src/web/index.html:", len(s), "bytes")
