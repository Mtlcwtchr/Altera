#!/usr/bin/env python3
"""Генератор парадигм лексем ru для реплик (AL-51, doc/design/DIALOGUE_GEN.md §5.1).

Источник — компактный словарь tools/lexgen/ru.json (человек пишет основу и тип, исключения — явными
формами). Выход — content/loc/ru/lex_verbs.json, lex_adj.json, lex_nouns.json (ключи lex.verb.<id>,
lex.adj.<id>, lex.noun.<id>). Сгенерированные файлы руками не править: правка в ru.json и перезапуск.

  python tools/lexgen.py            # пересобрать
  python tools/lexgen.py --check    # только сравнить с файлами на диске (код 1, если устарели)

Глагол:  {"inf": "украсть", "pfv": true, "past": "укра" | ["залез", "залезл"],
          "np": ["украду", "украд", "ё", "украдут"],        # 1s, основа, гласная (е|ё|и), 3p
          "imp": ["укради", "украдите"], "syn": ["pilfer", ...]}   # syn — id других лексем
          np у совершенного вида даёт fut, у несовершенного pres. Любую форму можно задать явно:
          "past_forms": {...}, "fut": {...}, "pres": {...}.
Прилагательное: "краденый" (тип по окончанию) | {"word": ..., "type": "hard|velar|hush|soft|oy"} |
          {"forms": {"m": {...}, "f": {...}, "n": {...}, "pl": {...}}} (неправильные: весь).
Существительное: {"nom": "кража", "g": "f", "decl": "a|m|m_soft|n|f_soft|fixed", "anim": false,
          "stem": "мешк" (беглая гласная), "gpl": "краж", "only_sg": true, "forms": {...}, "pl_forms": {...}}
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "tools" / "lexgen" / "ru.json"
OUT = ROOT / "content" / "loc" / "ru"
CASES = ["nom", "gen", "dat", "acc", "ins", "pre"]
VELAR, HUSH = set("кгх"), set("жшщч")


def fix(stem, end):
    """Правила правописания: ы→и после г к х ж ш щ ч; я→а, ю→у после ж ш щ ч ц (для окончаний)."""
    if end and stem:
        c = stem[-1]
        if end[0] == "ы" and (c in VELAR or c in HUSH):
            end = "и" + end[1:]
    return stem + end


# --- глаголы -----------------------------------------------------------------
NP_END = {
    "е": ["", "ешь", "ет", "ем", "ете", ""],
    "ё": ["", "ёшь", "ёт", "ём", "ёте", ""],
    "и": ["", "ишь", "ит", "им", "ите", ""],
}
PERSONS = ["1s", "2s", "3s", "1p", "2p", "3p"]


def verb(vid, s):
    out = {"inf": s["inf"], "pfv": bool(s.get("pfv", False))}
    if "past_forms" in s:
        out["past"] = s["past_forms"]
    else:
        p = s["past"]
        m, st = (p + "л", p + "л") if isinstance(p, str) else (p[0], p[1])
        out["past"] = {"m": m, "f": st + "а", "n": st + "о", "pl": st + "и"}
    tense = "fut" if out["pfv"] else "pres"
    if tense in s:
        out[tense] = s[tense]
    elif "np" in s:
        first, stem, vowel, third = s["np"]
        forms = {}
        for i, pers in enumerate(PERSONS):
            forms[pers] = first if i == 0 else third if i == 5 else stem + NP_END[vowel][i]
        out[tense] = forms
    if "imp" in s:
        out["imp"] = {"2s": s["imp"][0], "2p": s["imp"][1]}
    if s.get("syn"):
        out["syn"] = s["syn"]
    if s.get("refl"):
        out["refl"] = True
    return out


# --- прилагательные ----------------------------------------------------------
def adj_type(word):
    if word.endswith("ой"):
        return "oy"
    if word.endswith("ый"):
        return "hard"
    if word.endswith("ий"):
        c = word[-3]
        if c in VELAR:
            return "velar"
        if c in HUSH:
            return "hush"
        return "soft"
    raise ValueError(f"прилагательное {word}: тип не определить, задай type")


ADJ = {
    # m: nom gen dat acc(inan) ins pre; acc_an = gen
    "hard":  {"m": ["ый", "ого", "ому", "ый", "ым", "ом"], "n": ["ое", "ого", "ому", "ое", "ым", "ом"],
              "f": ["ая", "ой", "ой", "ую", "ой", "ой"], "pl": ["ые", "ых", "ым", "ые", "ыми", "ых"]},
    "oy":    {"m": ["ой", "ого", "ому", "ой", "ым", "ом"], "n": ["ое", "ого", "ому", "ое", "ым", "ом"],
              "f": ["ая", "ой", "ой", "ую", "ой", "ой"], "pl": ["ые", "ых", "ым", "ые", "ыми", "ых"]},
    "velar": {"m": ["ий", "ого", "ому", "ий", "им", "ом"], "n": ["ое", "ого", "ому", "ое", "им", "ом"],
              "f": ["ая", "ой", "ой", "ую", "ой", "ой"], "pl": ["ие", "их", "им", "ие", "ими", "их"]},
    "hush":  {"m": ["ий", "его", "ему", "ий", "им", "ем"], "n": ["ее", "его", "ему", "ее", "им", "ем"],
              "f": ["ая", "ей", "ей", "ую", "ей", "ей"], "pl": ["ие", "их", "им", "ие", "ими", "их"]},
    "soft":  {"m": ["ий", "его", "ему", "ий", "им", "ем"], "n": ["ее", "его", "ему", "ее", "им", "ем"],
              "f": ["яя", "ей", "ей", "юю", "ей", "ей"], "pl": ["ие", "их", "им", "ие", "ими", "их"]},
}


def adj(aid, s):
    if isinstance(s, dict) and "forms" in s:
        return s["forms"]
    word = s if isinstance(s, str) else s["word"]
    t = adj_type(word) if isinstance(s, str) or "type" not in s else s["type"]
    stem = word[:-2]
    out = {}
    for g, ends in ADJ[t].items():
        f = {c: fix(stem, e) for c, e in zip(CASES, ends)}
        if g in ("m", "pl"):
            f["acc_an"] = f["gen"]
        out[g] = f
    return out


# --- существительные ---------------------------------------------------------
def noun(nid, s):
    nom, decl = s["nom"], s.get("decl", "fixed")
    anim = bool(s.get("anim", False))
    sg, pl = {}, {}
    if decl == "fixed":
        sg = {c: nom for c in CASES}
        pl = dict(s.get("pl", {}))
    elif decl == "a":
        soft = nom.endswith("я")
        st = nom[:-2] if nom.endswith("ия") else s.get("stem", nom[:-1])
        if nom.endswith("ия"):
            sg = dict(zip(CASES, [nom, st + "ии", st + "ии", st + "ию", st + "ией", st + "ии"]))
            pl = dict(zip(CASES, [st + "ии", st + "ий", st + "иям", st + "ии", st + "иями", st + "иях"]))
        elif soft:
            sg = dict(zip(CASES, [nom, st + "и", st + "е", st + "ю", st + "ей", st + "е"]))
            pl = dict(zip(CASES, [st + "и", s.get("gpl", st + "ь"), st + "ям", st + "и", st + "ями", st + "ях"]))
        else:
            ins = "ей" if st[-1] in HUSH | {"ц"} and not s.get("stressed") else "ой"
            sg = {"nom": nom, "gen": fix(st, "ы"), "dat": st + "е", "acc": st + "у", "ins": st + ins, "pre": st + "е"}
            pl = {"nom": fix(st, "ы"), "gen": s.get("gpl", st), "dat": st + "ам", "acc": fix(st, "ы"),
                  "ins": st + "ами", "pre": st + "ах"}
    elif decl == "m":
        st = s.get("stem", nom)
        ins = "ем" if st[-1] in HUSH | {"ц"} and not s.get("stressed") else "ом"
        sg = {"nom": nom, "gen": st + "а", "dat": st + "у", "acc": nom, "ins": st + ins, "pre": st + "е"}
        gpl = "ей" if st[-1] in HUSH else "ев" if st[-1] == "ц" and not s.get("stressed") else "ов"
        pl = {"nom": fix(st, "ы"), "gen": s.get("gpl", st + gpl), "dat": st + "ам", "acc": fix(st, "ы"),
              "ins": st + "ами", "pre": st + "ах"}
    elif decl == "m_soft":
        st = nom[:-1]
        y = nom.endswith("й")
        sg = {"nom": nom, "gen": st + "я", "dat": st + "ю", "acc": nom, "ins": st + ("ем" if y else s.get("ins_end", "ем")),
              "pre": st + "е"}
        pl = {"nom": st + "и", "gen": s.get("gpl", st + ("ев" if y else "ей")), "dat": st + "ям", "acc": st + "и",
              "ins": st + "ями", "pre": st + "ях"}
    elif decl == "n":
        st = nom[:-1]
        if nom.endswith("ие"):
            sg = dict(zip(CASES, [nom, st + "я", st + "ю", nom, st + "ем", st + "и"]))
            pl = dict(zip(CASES, [st + "я", st[:-1] + "ий", st + "ям", st + "я", st + "ями", st + "ях"]))
        elif nom.endswith("е"):
            sg = dict(zip(CASES, [nom, st + "я", st + "ю", nom, st + "ем", st + "е"]))
            pl = dict(zip(CASES, [st + "я", s.get("gpl", st + "ей"), st + "ям", st + "я", st + "ями", st + "ях"]))
        else:
            sg = dict(zip(CASES, [nom, st + "а", st + "у", nom, st + "ом", st + "е"]))
            pl = dict(zip(CASES, [st + "а", s.get("gpl", st), st + "ам", st + "а", st + "ами", st + "ах"]))
    elif decl == "f_soft":
        st = nom[:-1]
        sg = dict(zip(CASES, [nom, st + "и", st + "и", nom, st + "ью", st + "и"]))
        pl = dict(zip(CASES, [st + "и", st + "ей", st + "ям", st + "и", st + "ями", st + "ях"]))
    else:
        raise ValueError(f"{nid}: неизвестный decl {decl}")
    sg.update(s.get("forms", {}))
    if pl:
        pl.update(s.get("pl_forms", {}))
    if anim:
        if s["g"] == "m" and decl in ("m", "m_soft"):
            sg["acc"] = sg["gen"]
        if pl:
            pl["acc"] = pl["gen"]
    out = dict(sg)
    out["g"] = s["g"]
    if anim:
        out["anim"] = True
    if pl and not s.get("only_sg"):
        out["pl"] = pl
    return out


def build(src):
    doc = "СГЕНЕРИРОВАНО tools/lexgen.py из tools/lexgen/ru.json — руками не править (AL-51, DIALOGUE_GEN §5.1)."
    verbs = {"_doc": doc + " Глаголы: inf, pfv, past{m,f,n,pl}, fut|pres{1s..3p}, imp{2s,2p}, syn — id других лексем.",
             "lex": {"verb": {k: verb(k, v) for k, v in src["verbs"].items()}}}
    adjs = {"_doc": doc + " Прилагательные: m|f|n|pl → падежи; acc_an — вин. одушевлённых (м.р. и мн.ч.).",
            "lex": {"adj": {k: adj(k, v) for k, v in src["adj"].items()}}}
    nouns = {"_doc": doc + " Существительные: падежи ед.ч., g, anim, pl{падежи}.",
             "lex": {"noun": {k: noun(k, v) for k, v in src["nouns"].items()}}}
    keys = {"_doc": doc + " " + src["keys"].get("_doc", "")}
    for k, v in src["keys"].items():
        if k == "_doc":
            continue
        n = noun(k, src["nouns"][v]) if isinstance(v, str) else noun(k, v)
        keys[k] = n
    return {"lex_verbs.json": verbs, "lex_adj.json": adjs, "lex_nouns.json": nouns, "lex_keys.json": keys}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    src = json.loads(SRC.read_text(encoding="utf-8"))
    stale = []
    for name, data in build(src).items():
        text = json.dumps(data, ensure_ascii=False, indent=1) + "\n"
        p = OUT / name
        if a.check:
            if not p.exists() or p.read_text(encoding="utf-8") != text:
                stale.append(name)
        else:
            p.write_text(text, encoding="utf-8")
            n = sum(len(v) for v in data["lex"].values()) if "lex" in data else len(data) - 1
            print(f"{p.relative_to(ROOT)}: {n}")
    if stale:
        print("устарели (перезапусти tools/lexgen.py):", ", ".join(stale))
        sys.exit(1)


if __name__ == "__main__":
    main()
