#!/usr/bin/env python3
"""Collect downloadable CC0 / CC-BY Sketchfab models for Altera -> doc/sketchfab_candidates.{json,md}"""
import json, urllib.request, urllib.parse, time
UA = {"User-Agent": "Mozilla/5.0 altera-asset-search"}
Q = {
 "architecture": ["gothic cathedral ruins", "medieval house", "medieval village", "castle tower", "fantasy tower", "ruined temple", "stone bridge medieval", "windmill", "gatehouse", "abandoned church", "crypt mausoleum", "fantasy city building", "giant ruins colossal", "monolith obelisk", "stone arch portal"],
 "surreal_landscape": ["floating island", "rock spire", "natural arch rock", "alien landscape", "giant tree fantasy", "twisted tree", "dead tree", "giant mushroom", "crystal formation", "giant bones skeleton", "ribcage giant", "strange rock formation", "canyon mesa", "stalagmite cave", "volcano", "waterfall cliff"],
 "flora": ["alien plant", "fantasy flora", "swamp reeds", "thorn bush", "vines hanging", "dark forest tree", "pine tree stylized", "bramble"],
 "creatures": ["giant monster", "demon", "dragon", "wyvern", "skeleton warrior", "undead", "troll ogre", "goblin", "eldritch tentacle", "gargoyle", "griffin", "wolf", "horse medieval", "crow raven", "giant insect", "knight armor", "peasant villager", "wizard witch", "hooded cultist"],
 "props": ["medieval cart", "tavern", "medieval weapons sword", "gallows", "tombstone", "altar ritual", "banner flag medieval", "throne", "well stone", "fountain statue", "tent camp", "boat medieval ship", "siege", "bone pile skulls", "chains cage"],
}
out, seen = [], set()
for cat, qs in Q.items():
    for q in qs:
        for lic in ("cc0", "by"):
            u = "https://api.sketchfab.com/v3/search?" + urllib.parse.urlencode({"type": "models", "downloadable": "true", "q": q, "license": lic, "sort_by": "-likeCount", "count": 12})
            try:
                r = json.load(urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=60))
            except Exception as e:
                print("ERR", q, lic, e); time.sleep(2); continue
            for m in r["results"]:
                if m["uid"] in seen or m.get("faceCount", 0) > 600000: continue
                seen.add(m["uid"])
                out.append({"uid": m["uid"], "name": m["name"], "cat": cat, "query": q, "license": lic, "faces": m.get("faceCount"), "likes": m.get("likeCount"), "author": m["user"]["displayName"], "url": m["viewerUrl"], "animated": m.get("animationCount", 0) > 0})
            time.sleep(0.4)
json.dump(out, open("doc/sketchfab_candidates.json", "w", encoding="utf8"), ensure_ascii=False, indent=1)
with open("doc/sketchfab_candidates.md", "w", encoding="utf8") as f:
    f.write("# Sketchfab: кандидаты (downloadable, CC0 / CC-BY)\n\nСкачать: `SKETCHFAB_TOKEN=... python tools/sketchfab_download.py doc/sketchfab_candidates.json`\n")
    for cat in Q:
        f.write(f"\n## {cat}\n\n| Модель | Автор | Лиц. | Фейсы | Лайки |\n|---|---|---|---|---|\n")
        for m in sorted((x for x in out if x["cat"] == cat), key=lambda x: -(x["likes"] or 0)):
            f.write(f"| [{m['name']}]({m['url']}) | {m['author']} | {m['license'].upper()} | {m['faces']} | {m['likes']} |\n")
print(len(out), "models")
