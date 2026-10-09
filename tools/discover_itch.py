#!/usr/bin/env python3
"""List itch.io game-asset pages from free listings: discover_itch.py tag-3d/tag-fantasy [pages]"""
import re, sys, urllib.request
UA = {"User-Agent": "Mozilla/5.0 campfire-asset-fetch"}
def run(path, pages=3):
    seen = {}
    for p in range(1, pages + 1):
        u = f"https://itch.io/game-assets/free/{path}?page={p}"
        try:
            h = urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=60).read().decode("utf8", "replace")
        except Exception as e:
            print("ERR", u, e, file=sys.stderr); break
        for m in re.finditer(r'<div class="game_cell[^>]*>.*?</div>\s*</div>\s*</div>', h, re.S):
            c = m.group(0)
            link = re.search(r'href="(https://[a-z0-9_-]+\.itch\.io/[a-z0-9_-]+)"', c)
            t = re.search(r'class="title game_link"[^>]*>([^<]+)', c)
            if link and link.group(1) not in seen:
                seen[link.group(1)] = (t.group(1) if t else "").strip()
    for k, v in seen.items():
        print(k, "|", v)
if __name__ == "__main__":
    run(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 3)
