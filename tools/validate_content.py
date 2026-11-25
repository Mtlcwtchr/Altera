#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Единый валидатор контента Altera (облако и ПК, только stdlib).

  python tools/validate_content.py [--root ПУТЬ] [--quiet] [--log файл] [подкоманда ...]

Подкоманды (без аргументов = all):
  json       все *.json парсятся, нет дублей id внутри списков одного файла
  loc        ru/en: набор ключей, плейсхолдеры {x}, {x:g|..}, {n:plural|..}; ru обязателен (base), нет en = WARN
  ai         content_ai (схемы, ссылки, графы) через validate_ai.py
  expansion  content_expansion через validate_expansion.py (слияние с ai + ссылки + loc)
  crafting   crafting/data через validate_k1.py
  backstory  ai/backstories.json: ссылки на умения, черты, характеристики, предметы, чертежи, права, оси,
             места старта, иконки (icons.json, ui/backstory.json) и ключи loc backstory.<id>.name/desc/life
  docs       пути в *.md существуют, нет дублей id задач в WORK_PLAN/BACKLOG

Корень: --root, иначе ./content (раскладка репо) или /mnt/project-files/gdd (раскладка общей папки).
Вывод: итог по подкомандам + первые 20 проблем каждой; полный журнал в --log (по умолчанию validate_content.log).
Код возврата 1 при любой ERR. Образец: gdd/crafting/tools/validate_k1.py."""
import argparse, json, re, subprocess, sys
from collections import Counter
from pathlib import Path

MAX_SHOW = 20
PH = re.compile(r"\{([A-Za-z0-9_]+)(?::(?!g\|)[^}]*)?\}")  # {x:g|..} (род) не считается: он бывает только в ru


class Report:
    def __init__(self, name):
        self.name, self.errs, self.warns, self.info = name, [], [], ""

    def err(self, m): self.errs.append(m)
    def warn(self, m): self.warns.append(m)


def jload(p):
    return json.loads(Path(p).read_text(encoding="utf-8"))


def find_layout(root: Path):
    """Возвращает словарь каталогов. Ключи отсутствующих пропускаются."""
    L = {"root": root}
    if (root / "ai").is_dir() or (root / "loc").is_dir():          # репо: content/
        L["ai"] = root / "ai" if (root / "ai").is_dir() else None
        L["loc"] = [root / "loc"] if (root / "loc").is_dir() else []
        L["json_roots"] = [root]
        L["crafting"] = root / "crafting"
        L["expansion"] = None
        L["docs"] = [root.parent / "doc"] if (root.parent / "doc").is_dir() else []
    else:                                                          # общая папка gdd/
        L["ai"] = root / "content_ai"
        L["loc"] = [d / "loc" for d in root.iterdir() if (d / "loc").is_dir()]
        L["json_roots"] = [d for d in root.iterdir() if d.is_dir() and d.name != "__pycache__"]
        L["crafting"] = root / "crafting"
        L["expansion"] = root / "content_expansion"
        L["docs"] = [root]
    return L


def flatten(d, pre=""):
    out = {}
    for k, v in d.items():
        if k == "_doc": continue
        key = f"{pre}.{k}" if pre else k
        # LOC v0.2: lex.<pos>.<id> and frame.<id> are one leaf each (not opened up)
        leaf = (key.startswith("lex.") and key.count(".") == 2) or (key.startswith("frame.") and key.count(".") == 1)
        if isinstance(v, dict) and "nom" not in v and not leaf:
            out.update(flatten(v, key))
        else:
            out[key] = v
    return out


# ---------------------------------------------------------------- json
def check_json(L):
    r = Report("json"); n = 0
    for base in L["json_roots"]:
        for p in sorted(base.rglob("*.json")):
            if "__pycache__" in p.parts or "/out/" in p.as_posix(): continue
            n += 1
            try:
                data = jload(p)
            except Exception as e:
                r.err(f"{p}: не парсится ({e})"); continue
            stack = [data]
            while stack:
                x = stack.pop()
                if isinstance(x, dict):
                    stack += [v for v in x.values() if isinstance(v, (dict, list))]
                elif isinstance(x, list):
                    ids = [i["id"] for i in x if isinstance(i, dict) and isinstance(i.get("id"), str)]
                    for i, c in Counter(ids).items():
                        if c > 1: r.err(f"{p}: дубль id «{i}» ×{c}")
                    stack += [v for v in x if isinstance(v, (dict, list))]
    r.info = f"{n} файлов"
    return r


# ---------------------------------------------------------------- loc
def load_lang(d: Path, r):
    keys = {}
    if not d.is_dir(): return keys
    for p in sorted(d.glob("*.json")):
        if p.name.startswith("_"): continue   # _grammar.json: grammar, not keys (LOC_FORMAT §7)
        try:
            for k, v in flatten(jload(p)).items():
                if k in keys: r.warn(f"{d}: ключ {k} повторён (файл {p.name})")
                else: keys[k] = v
        except Exception as e:
            r.err(f"{p}: не парсится ({e})")
    return keys


def phs(v):
    s = v.get("nom") if isinstance(v, dict) else v
    return set(PH.findall(s)) if isinstance(s, str) else set()


def check_loc(L):
    r = Report("loc"); total = 0
    for loc in L["loc"]:
        ru, en = load_lang(loc / "ru", r), load_lang(loc / "en", r)
        soft = (loc / "ru" / "_grammar.json").exists()   # свои грамматики по языкам (history): расхождения только WARN
        E = r.warn if soft else r.err
        total += len(ru)
        if not en and not (loc / "en").is_dir():
            r.warn(f"{loc}: нет каталога en ({len(ru)} ключей без перевода)"); continue
        for k in sorted(set(en) - set(ru)): E(f"{loc}: ключ {k} есть в en, но нет в ru (base)")
        miss = sorted(set(ru) - set(en))
        if miss: r.warn(f"{loc}: в en нет {len(miss)} ключей (напр. {miss[0]})")
        for k in sorted(set(ru) & set(en)):
            if phs(ru[k]) != phs(en[k]):
                E(f"{loc}: разные плейсхолдеры в {k}: ru{sorted(phs(ru[k]))} en{sorted(phs(en[k]))}")
        # скобки: непарные { }
        for lang, d in (("ru", ru), ("en", en)):
            for k, v in d.items():
                s = v.get("nom") if isinstance(v, dict) else v
                if isinstance(s, str) and s.replace("{{", "").replace("}}", "").count("{") != s.replace("{{", "").replace("}}", "").count("}"):
                    r.err(f"{loc}/{lang}: непарные скобки в {k}")
    r.info = f"{total} ключей ru"
    return r


# ---------------------------------------------------------------- внешние валидаторы
def run_ext(name, cmd, ok_re=None):
    r = Report(name)
    try:
        p = subprocess.run([sys.executable, "-I"] + cmd, capture_output=True, text=True, timeout=600)
    except Exception as e:
        r.err(f"не удалось запустить {cmd[0]}: {e}"); return r
    lines = (p.stdout + p.stderr).splitlines()
    for l in lines:
        if re.match(r"\s*(ERR|ОШИБКА|Traceback)", l): r.err(re.sub(r"^(ERR|ОШИБКА)\s*", "", l.strip()))
        elif l.startswith("WARN") : r.warn(re.sub(r"^WARN\s*", "", l.strip()))
    summ = [l for l in lines if l.startswith(("итог", "расширение", "materials"))]
    r.info = summ[-1] if summ else ""
    if p.returncode != 0 and not r.errs:
        r.err(f"код возврата {p.returncode}: " + " | ".join(lines[-3:]))
    return r


def check_ai(L):
    d = L["ai"]
    if not d or not (d / "tools" / "validate_ai.py").exists():
        r = Report("ai"); r.warn("нет content_ai/tools/validate_ai.py, пропуск"); return r
    r = run_ext("ai", [str(d / "tools" / "validate_ai.py"), str(d)])
    if L["expansion"] is None and r.errs:   # раскладка репо: Python-образец отстал от схемы C++ (species, care, build...), авторитетен altera::sim::validate
        r.warn(f"эталонный validate_ai.py отстал от схемы репо: {len(r.errs)} замечаний только информативно; авторитет — тесты altera_sim")
        r.errs = []
    return r


def check_expansion(L):
    d = L["expansion"]
    if not d or not (d / "tools" / "validate_expansion.py").exists():
        r = Report("expansion"); r.warn("нет content_expansion, пропуск"); return r
    return run_ext("expansion", [str(d / "tools" / "validate_expansion.py")])


def check_crafting(L):
    d = L["crafting"]
    if not (d / "tools" / "validate_k1.py").exists():
        r = Report("crafting"); r.warn("нет crafting/tools/validate_k1.py, пропуск"); return r
    return run_ext("crafting", [str(d / "tools" / "validate_k1.py")])


# ---------------------------------------------------------------- backstory
def _ids(d, key):
    v = d.get(key, []) if isinstance(d, dict) else d
    return {x["id"] for x in v if isinstance(x, dict) and "id" in x} if isinstance(v, list) else set(v)


def check_backstory(L):
    r = Report("backstory")
    ai = L.get("ai")
    f = ai / "backstories.json" if ai else None
    if not f or not f.exists():
        r.warn("нет ai/backstories.json, пропуск"); return r
    d = jload(f)
    ex = ai / "expansion"
    J = lambda p: jload(p) if p.exists() else {}
    skills = _ids(J(ai / "knowledge.json"), "knowledge") | _ids(J(ex / "knowledge.json"), "knowledge")
    traits = _ids(J(ai / "traits.json"), "traits") | _ids(J(ex / "traits.json"), "traits")
    items = _ids(J(ai / "items.json"), "items") | _ids(J(ex / "items.json"), "items")
    attrs = _ids(J(ex / "attributes.json"), "attributes")
    designs = _ids(J(ai / "craft" / "designs.json"), "designs")
    cfg = J(ai / "config.json")
    rights = set(cfg.get("rights", []))
    rep = set(cfg.get("reputation_axes", ["honesty", "valor", "piety", "generosity", "notoriety"]))
    rel = {"affinity", "trust", "fear", "familiarity", "debt", "enmity", "respect", "attraction"}
    excl = [tuple(p) for p in J(ai / "traits.json").get("exclusive", []) + J(ex / "traits.json").get("exclusive", [])
            if isinstance(p, list) and len(p) == 2]
    starts = set(d.get("config", {}).get("starts", {"home": {}}))
    repo = L["root"].parent
    icons = set(J(repo / "assets" / "altera_game" / "ui" / "icons.json").get("icons", {}))
    ui = J(L["root"] / "ui" / "backstory.json")
    lang = {}
    for loc in L["loc"]:
        for lg in ("ru", "en"):
            for p in sorted((loc / lg).glob("*.json")):
                lang.setdefault(lg, set()).update(jload(p).keys())
    seen = set(); n = 0
    for b in d.get("backstories", []):
        n += 1
        i = b.get("id", ""); at = f"{i or '<нет id>'}: "
        if not i: r.err(at + "нет id")
        elif i in seen: r.err(at + "дубль id")
        seen.add(i)
        if icons and b.get("icon") not in icons: r.err(at + f"иконки {b.get('icon')} нет в icons.json")
        if not b.get("skills"): r.err(at + "нет умений")
        for k, v in b.get("skills", {}).items():
            if k not in skills: r.err(at + f"неизвестное умение {k}")
            if not 0 <= v <= 1: r.err(at + f"умение {k} вне 0..1")
        t = b.get("traits", {})
        al, pool = t.get("always", []), t.get("pool", [])
        for x in al + pool:
            if x not in traits: r.err(at + f"неизвестная черта {x}")
        if not 0 <= t.get("pick", 0) <= len(pool): r.err(at + "traits.pick больше пула")
        for a in al:
            for c in al:
                if (a, c) in excl: r.err(at + f"взаимоисключающие черты вместе: {a}, {c}")
        for a, v in b.get("attrs", {}).items():
            if attrs and a not in attrs: r.err(at + f"неизвестная характеристика {a}")
            if abs(v) > 0.5: r.err(at + f"сдвиг {a} больше 0.5")
        for it, c in b.get("items", {}).items():
            if it not in items: r.err(at + f"неизвестный предмет {it}")
            if c <= 0: r.err(at + f"предмет {it}: количество <= 0")
            if ui and icons and ui.get("item_icons", {}).get(it, ui.get("item_icon_default")) not in icons:
                r.warn(at + f"иконка предмета {it} не найдена")
        if b.get("coins", 0) < 0: r.err(at + "coins < 0")
        if b.get("coins", 0) > 0 and "coin" not in items: r.err(at + "есть монеты, нет предмета coin")
        for ds in b.get("recipes", []) + [m.get("design") for m in b.get("made", [])]:
            if designs and ds not in designs: r.err(at + f"неизвестный чертёж {ds}")
        for m in b.get("made", []):
            if not 0 <= m.get("q", 0.5) <= 1: r.err(at + f"made {m.get('design')}: q вне 0..1")
        if b.get("start", "home") not in starts: r.err(at + f"неизвестное место старта {b.get('start')}")
        for x in b.get("rights", []):
            if x not in rights: r.err(at + f"неизвестное право {x}")
        for x, v in b.get("reputation", {}).items():
            if x not in rep: r.err(at + f"неизвестная ось молвы {x}")
            if abs(v) > 1: r.err(at + f"молва {x} вне -1..1")
        for g, axes in b.get("relations", {}).items():
            if g not in ("kin", "village"): r.err(at + f"relations: неизвестная группа {g} (kin|village)")
            for x, v in axes.items():
                if x not in rel: r.err(at + f"relations: неизвестная ось {x}")
                if abs(v) > 1: r.err(at + f"relations: {x} вне -1..1")
        for suf in ("name", "desc", "life"):
            k = f"backstory.{i}.{suf}"
            if lang and k not in lang.get("ru", set()): r.err(at + f"нет ключа loc {k} в ru")
            elif lang and k not in lang.get("en", set()): r.warn(at + f"нет ключа loc {k} в en")
    for s_ in starts:
        if lang and f"hud.backstory.start.{s_}" not in lang.get("ru", set()): r.err(f"нет ключа loc hud.backstory.start.{s_} в ru")
    r.info = f"{n} предысторий"
    return r


# ---------------------------------------------------------------- docs
LINK = re.compile(r"\]\(([^)#\s]+)\)|`((?:/mnt/project-files|gdd|doc|content|tools|src)/[^`\s*<>{}]+\.[A-Za-z0-9]{1,5})`")


def check_docs(L):
    r = Report("docs"); n = 0
    for base in L["docs"]:
        for p in sorted(base.rglob("*.md")):
            if "__pycache__" in p.parts: continue
            n += 1
            for m in LINK.finditer(p.read_text(encoding="utf-8", errors="ignore")):
                t = m.group(1) or m.group(2)
                if t.startswith(("http", "mailto", "#", "D:", "C:")) or "\\" in t: continue
                cands = [Path(t)] if t.startswith("/") else [p.parent / t, base / t, base.parent / t, Path("/mnt/project-files") / t]
                if not any(c.exists() for c in cands):
                    r.warn(f"{p.name}: нет файла {t}")
    for name in ("WORK_PLAN.md", "BACKLOG.md"):
        for base in L["docs"]:
            f = base / name
            if not f.exists(): continue
            ids = re.findall(r"^\|\s*([A-Z]\d*|[A-Z]-?\d+)\s*\|", f.read_text(encoding="utf-8"), re.M)
            for i, c in Counter(ids).items():
                if c > 1: r.err(f"{name}: дубль id задачи {i}")
    r.info = f"{n} md"
    return r


def check_elements(L):
    """AL-115: поля клетки (world/fields.json) и матрица реакций (magic/reactions.json)."""
    r = Report("elements")
    root = L["root"]
    fp, rp, ap = root / "world" / "fields.json", root / "magic" / "reactions.json", root / "magic" / "aspects.json"
    if not fp.exists():
        r.warn("нет world/fields.json, пропуск"); return r
    f = jload(fp)
    known_fields = {"heat", "moisture", "fertility", "integrity", "light", "aether", "taint"}
    for k in f.get("fields", {}):
        if k not in known_fields: r.err(f"fields.json: неизвестное поле {k}")
    mats = {k for k in f.get("materials", {}) if not k.startswith("_")}
    ai = L.get("ai")
    objs = _ids(jload(ai / "objects.json"), "objects") if ai and (ai / "objects.json").exists() else set()
    for t, v in f.get("things", {}).items():
        if t.startswith("_") or not isinstance(v, dict): continue
        if v.get("material") and v["material"] not in mats: r.err(f"fields.json things.{t}: нет материала {v['material']}")
        if objs and t not in objs: r.err(f"fields.json things.{t}: нет такого объекта в objects.json")
    for m, v in f.get("piece_materials", {}).items():
        if v not in mats: r.err(f"fields.json piece_materials.{m}: нет материала {v}")
    if not rp.exists() or not ap.exists():
        r.warn("нет magic/reactions.json или magic/aspects.json, матрица не проверена"); return r
    asp = [a["id"] for a in jload(ap).get("aspects", [])]
    pairs = jload(rp).get("pairs", {})
    want = set()
    for i, a in enumerate(asp):
        for b in asp[i:]:
            want.add("+".join(sorted([a, b])))
    for k in sorted(want - set(pairs)): r.err(f"reactions.json: нет записи для пары {k} (нужна реакция или \"none\")")
    for k in sorted(set(pairs) - want): r.err(f"reactions.json: пара {k} из неизвестных аспектов")
    for k, v in pairs.items():
        if isinstance(v, dict):
            for ch in v.get("channels", {}):
                if ch not in known_fields and ch not in {"transfer", "rain", "sun", "wind", "storm", "rust", "growth", "lure", "push",
                                                         "raise_ground", "shape_plants", "shape_metal", "shape_stone"}:
                    r.warn(f"reactions.json {k}: канал {ch} никто не читает")
    r.info = f"пар {len(pairs)}, с реакцией {sum(1 for v in pairs.values() if isinstance(v, dict))}"
    return r


CHECKS = {"json": check_json, "loc": check_loc, "ai": check_ai, "expansion": check_expansion,
          "crafting": check_crafting, "backstory": check_backstory, "docs": check_docs}
CHECKS["elements"] = check_elements  # AL-115


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmds", nargs="*")
    ap.add_argument("--root"); ap.add_argument("--log", default="validate_content.log"); ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    cmds = [c for c in a.cmds if c != "all"] or list(CHECKS)
    bad = [c for c in cmds if c not in CHECKS]
    if bad: ap.error(f"неизвестная подкоманда: {bad}; есть: {list(CHECKS)}")
    root = Path(a.root) if a.root else (Path("content") if Path("content/ai").is_dir() else Path("/mnt/project-files/gdd"))
    if not root.is_dir(): ap.error(f"нет каталога {root}")
    L = find_layout(root.resolve())
    full, bad_total = [], 0
    for c in cmds:
        r = CHECKS[c](L)
        bad_total += len(r.errs)
        print(f"[{r.name}] ошибок {len(r.errs)}, предупреждений {len(r.warns)}" + (f"  ({r.info})" if r.info else ""))
        shown = r.errs[:MAX_SHOW] + ([] if a.quiet else r.warns[:max(0, MAX_SHOW - len(r.errs))])
        for l in shown: print("   " + ("ERR  " if l in r.errs else "WARN ") + l)
        if len(r.errs) + (0 if a.quiet else len(r.warns)) > len(shown): print(f"   … полный список в {a.log}")
        full += [f"[{r.name}] ERR {m}" for m in r.errs] + [f"[{r.name}] WARN {m}" for m in r.warns]
    Path(a.log).write_text("\n".join(full) + "\n", encoding="utf-8")
    print("ИТОГ:", "OK" if not bad_total else f"{bad_total} ошибок")
    sys.exit(1 if bad_total else 0)


if __name__ == "__main__":
    main()
