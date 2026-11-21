#!/usr/bin/env python3
"""Перенос шаблонов content/ai/dialogue.json (268 строк dialogue.<template>.<variant>) в правила
выбора реплик content/ai/dialogue_rules/port.json (AL-51, DIALOGUE_GEN §7).

Каждый вариант шаблона = правило: act=<speech> + критерий шаблона + его pre. Условия pre (rel:axis>=x)
становятся критериями rel.axis>=x, вес варианта — вес правила. Строки не трогаются: правило говорит
старый ключ dialogue.*. Более конкретные новые правила (lines.json, misdeeds.json, refuse.json) побеждают
их по числу критериев.

  python tools/port_dialogue_rules.py           # пересобрать port.json
  python tools/port_dialogue_rules.py --check   # код 1, если port.json устарел
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "content" / "ai" / "dialogue.json"
OUT = ROOT / "content" / "ai" / "dialogue_rules" / "port.json"

# Критерий шаблона по его id (кроме open_<plan> и gesture_<act>, они общие).
TEMPLATE_CRIT = {
    "inform_interaction": ["topic.kind=interaction"],
    "inform_playful": ["tone=playful"],
    "inform_warm": ["tone=warm"],
    "insult_sacred": ["tone=sacred"],
    "praise_flirtatious": ["tone=flirtatious"],
    "praise_sacred": ["tone=sacred"],
    "threaten_sacred": ["tone=sacred"],
    "request_pleading": ["tone=pleading"],
    "offer_terms": ["topic.kind=terms"],
    "promise_fealty": ["commitment=fealty"],
    "promise_sworn_friendship": ["commitment=sworn_friendship"],
}
PRE = re.compile(r"^rel:([a-z_]+)(<=|>=|<|>|=|!=)(.+)$")


def crit_for(template):
    tid = template["id"]
    if tid in TEMPLATE_CRIT:
        return TEMPLATE_CRIT[tid]
    if tid.startswith("open_"):
        return ["intent=" + tid[len("open_"):]]
    if tid.startswith("gesture_"):
        return ["mode=gesture"]
    if tid != template["speech"]:
        raise SystemExit(f"шаблон {tid}: неизвестно, какой критерий ставить (добавь в TEMPLATE_CRIT)")
    return []


def pre_to_crit(p):
    m = PRE.match(p)
    if not m:
        raise SystemExit(f"условие {p}: не умею переносить")
    return f"rel.{m.group(1)}{m.group(2)}{m.group(3)}"


def build():
    d = json.loads(SRC.read_text(encoding="utf-8"))
    rules = []
    for t in d["templates"]:
        base = ["act=" + t["speech"]] + crit_for(t)
        for v in t["variants"]:
            r = {"id": f"port.{t['id']}.{v['id']}", "when": base + [pre_to_crit(p) for p in v.get("pre", [])],
                 "say": f"dialogue.{t['id']}.{v['id']}"}
            if v.get("weight", 1.0) != 1.0:
                r["weight"] = v["weight"]
            rules.append(r)
    sa = d["speech_acts"]
    acts = [a if isinstance(a, str) else a["id"] for a in (sa if isinstance(sa, list) else sa.keys()) if a != "_doc"]
    fallback = {r["when"][0][4:] for r in rules if len(r["when"]) == 1}
    missing = [a for a in acts if a not in fallback]
    return {"_doc": "СГЕНЕРИРОВАНО tools/port_dialogue_rules.py из content/ai/dialogue.json — руками не править. "
                    "Перенос старых шаблонов dialogue.<template>.<variant> в правила (AL-51).",
            "rules": rules}, missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    data, missing = build()
    text = json.dumps(data, ensure_ascii=False, indent=1) + "\n"
    if a.check:
        if not OUT.exists() or OUT.read_text(encoding="utf-8") != text:
            print("port.json устарел: перезапусти tools/port_dialogue_rules.py")
            sys.exit(1)
        return
    OUT.write_text(text, encoding="utf-8")
    print(f"{OUT.relative_to(ROOT)}: {len(data['rules'])} правил")
    if missing:
        print("акты без правила-заглушки (только act=...):", ", ".join(missing))


if __name__ == "__main__":
    main()
