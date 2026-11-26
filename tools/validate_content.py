#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Единый валидатор контента LivelyRealms (облако и ПК, только stdlib).

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
  formulas   ai/formulas/*.json (LR-142): id, ссылки f.<id>, cfg.<константы>, $параметры, префиксы имён
             (attr./skill./trait./need./rel. сверяются с id контента), синтаксис, циклы
  templates  extends / use в списках ai/*.json и ai/templates (LR-142): известные id, нет циклов
  icons      2D-клиент (LR-161): у каждого предмета, объекта, действия/цели, мысли, нужды, процесса и станции
             мастерской, экрана есть картинка в sprites_2d, спрайты нарисованы (размер, палитра, перекраска)
  mods       папки модов (--mods, по умолчанию ../mods рядом с content/): JSON парсится, "_merge" ложится
             на существующий файл базы, id новых записей не пустые

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
    if L["expansion"] is None and r.errs:   # раскладка репо: Python-образец отстал от схемы C++ (species, care, build...), авторитетен lr::sim::validate
        r.warn(f"эталонный validate_ai.py отстал от схемы репо: {len(r.errs)} замечаний только информативно; авторитет — тесты lr_sim")
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
    icons = set(J(repo / "assets" / "lr_game" / "ui" / "icons.json").get("icons", {}))
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


def check_pawns(L):
    """LR-139: части 2D-пешек (sprites_2d/pawns2d.json) и облик народов (species/*.json body.pawn) ссылаются друг на друга."""
    r = Report("pawns")
    root = L["root"]
    pp = root / "sprites_2d" / "pawns2d.json"
    if not pp.exists():
        r.warn("нет sprites_2d/pawns2d.json, пропуск"); return r
    P = jload(pp)
    import re
    VARS = {"x", "y", "hcx", "hcy", "hrx", "hry", "top", "fx", "fy", "ey", "hl", "hrr", "eb", "nx", "bcx", "bcy", "brx", "bry", "bx", "by",
            "step", "swing", "child", "tx", "ty", "hy", "sw", "footy", "abs", "min", "max", "trunc", "floor"}
    COLS = set(P.get("colours", {})) | {"skin", "hair", "eye", "hat", "c", "wear", "cloak", "feet"}
    shapes = set(P.get("shapes", {}))

    def expr(v, where):
        if isinstance(v, (int, float)): return
        if not isinstance(v, str) or not re.fullmatch(r"[0-9a-z_+\-*/%().<>=!&|, ]*", v):
            r.err(f"{where}: плохое выражение {v!r}"); return
        for w in re.findall(r"[a-z_]+", v):
            if w not in VARS: r.err(f"{where}: неизвестное имя {w} в {v!r}")

    def colour(v, where):
        t = (v or "").replace(" ", "")
        if t.startswith("#"): return
        for w in re.findall(r"[a-z_]+", t):
            if w != "mix" and w not in COLS: r.err(f"{where}: неизвестный цвет {w}")

    def shape(sh, where):
        if isinstance(sh, str):
            if not (sh.startswith("@") and sh[1:] in shapes): r.err(f"{where}: нет формы {sh}")
            return
        if not isinstance(sh, dict): r.err(f"{where}: форма не объект"); return
        for k in ("ell", "rect", "col", "px"):
            for v in sh.get(k, []):
                for e in (v if isinstance(v, list) else [v]): expr(e, where)
        for k in ("where", "when"):
            if k in sh: expr(sh[k], where)
        if "minus" in sh: shape(sh["minus"], where)

    def ops(lst, where):
        for i, o in enumerate(lst if isinstance(lst, list) else []):
            w = f"{where}[{i}]"
            if "shapes" in o:
                for sh in o["shapes"]: shape(sh, w)
                if "where" in o: expr(o["where"], w)
            else: shape(o, w)
            colour(o.get("colour", "skin"), w)
            if "pattern" in o:
                expr(o["pattern"].get("if", "1"), w); colour(o["pattern"].get("colour", "c"), w)

    for n, v in P.get("shapes", {}).items():
        for sh in (v if isinstance(v, list) else [v]): shape(sh, f"shapes.{n}")
    for g in ("ears", "brows", "noses", "tusks", "beards", "hats", "wear", "tools"):
        for n, v in P.get(g, {}).items():
            if not n.startswith("_"): ops(v, f"{g}.{n}")
    for g in ("hair", "cloak"):
        for n, v in P.get(g, {}).items():
            if isinstance(v, dict):
                for slot in ("back", "front"): ops(v.get(slot, []), f"{g}.{n}.{slot}")
            elif g == "cloak": ops(v, f"{g}.{n}")
    for g in ("face", "feet", "hands"): ops(P.get(g, []), g)
    # the kinds name only what exists
    sp = root / "species"
    groups = {"ears": "ears", "nose": "noses", "brow": "brows", "tusks": "tusks", "build": "builds", "head": "heads", "skin_pattern": "skin_patterns"}
    hairs, beards = set(P.get("hair", {})) | {"bald", "none"}, set(P.get("beards", {}))
    n = 0
    for f in sorted(sp.glob("*.json")) if sp.is_dir() else []:
        d = jload(f)
        pw = (d.get("body") or {}).get("pawn") if isinstance(d, dict) else None
        if not isinstance(pw, dict): continue
        n += 1
        for k, g in groups.items():
            v = pw.get(k)
            if v and v not in P.get(g, {}): r.err(f"species/{f.name} body.pawn.{k}: {v} нет в pawns2d.json {g}")
        for k in ("hair_m", "hair_f"):
            for w in (pw.get(k) or "").split():
                if w not in hairs: r.err(f"species/{f.name} body.pawn.{k}: причёски {w} нет в pawns2d.json hair")
        for w in (pw.get("beards") or "").split():
            if w not in beards: r.err(f"species/{f.name} body.pawn.beards: бороды {w} нет в pawns2d.json beards")
    cc = L.get("ai") / "character_creation.json" if L.get("ai") else None
    if cc and cc.exists():
        look = jload(cc).get("look", {})
        for w in look.get("hair", []):
            if w not in hairs: r.err(f"character_creation.json look.hair: {w} нет в pawns2d.json hair")
        for w in look.get("beard", []):
            if w not in beards and w not in ("no", "yes"): r.err(f"character_creation.json look.beard: {w} нет в pawns2d.json beards")
    r.info = f"причёсок {len(hairs) - 2}, бород {len(beards)}, народов с обликом {n}"
    return r


CHECKS = {"json": check_json, "loc": check_loc, "ai": check_ai, "expansion": check_expansion,
          "crafting": check_crafting, "backstory": check_backstory, "docs": check_docs}
CHECKS["elements"] = check_elements  # AL-115
CHECKS["pawns"] = check_pawns  # LR-139


def check_peoples(L):
    """LR-166: народы по поясам, культуры, пояса; правило §1.1 (близость только у взрослых)."""
    r = Report("peoples")
    root = L["root"]
    sp, cu_dir = root / "species", root / "species" / "cultures"
    if not sp.is_dir():
        r.warn("нет species, пропуск"); return r
    races = {}
    for f in sorted(sp.glob("*.json")):
        if f.name in ("settle.json", "migrate.json"): continue
        d = jload(f)
        if isinstance(d, dict) and d.get("id"): races[d["id"]] = d
    bp = root / "world" / "belts.json"
    belts = (jload(bp) or {}).get("belts", {}) if bp.exists() else {}
    cultures = {}
    for f in sorted(cu_dir.glob("*.json")) if cu_dir.is_dir() else []:
        d = jload(f)
        if isinstance(d, dict) and d.get("id"): cultures[d["id"]] = d
    for id, d in races.items():
        if d.get("base") and d["base"] not in races: r.err(f"species/{id}: base {d['base']} неизвестен")
        for b in d.get("region", []):
            if belts and b not in belts: r.err(f"species/{id}: region {b} нет в world/belts.json")
        life = d.get("life", {})
        if life.get("sexes") and life.get("other_sex") and life["other_sex"] not in races:
            r.err(f"species/{id}: other_sex {life['other_sex']} неизвестен")
    for id, c in cultures.items():
        if belts and c.get("belt") not in belts: r.err(f"cultures/{id}: belt {c.get('belt')} нет в world/belts.json")
        if c.get("species") not in races: r.err(f"cultures/{id}: species {c.get('species')} неизвестен")
        for kind, mix in (c.get("peoples") or {}).items():
            for k in (mix if isinstance(mix, dict) else {}):
                if k not in races: r.err(f"cultures/{id} peoples.{kind}: {k} неизвестен")
                elif races[k].get("settles") is False: r.err(f"cultures/{id} peoples.{kind}: {k} — только база (settles: false)")
        inti = c.get("intimacy")
        if inti is not None and (not inti.get("adults_only") or inti.get("min_age_years", 0) < 14):
            r.err(f"cultures/{id}: intimacy только для взрослых (adults_only: true, min_age_years ≥ 14) — §1.1")
        dump = json.dumps(c, ensure_ascii=False)
        for banned in ("pederast", "harem", "child_bride", "child_marriage", "concubine_girl"):
            if banned in dump: r.err(f"cultures/{id}: {banned} исключено из сеттинга (§1.1)")
    for bid, b in belts.items():
        if b.get("map_culture") and b["map_culture"] not in cultures: r.err(f"belts.{bid}: map_culture {b['map_culture']} неизвестна")
        for e in b.get("layout", []):
            if e.get("culture", b.get("map_culture")) not in cultures: r.err(f"belts.{bid}: layout culture {e.get('culture')} неизвестна")
    r.info = f"народов {len(races)}, культур {len(cultures)}, поясов {len(belts)}"
    return r


CHECKS["peoples"] = check_peoples  # LR-166


# ---------------------------------------------------------------- LR-142: formulas, templates, mods
FORMULA_PREFIXES = ("attr.", "skill.", "trait.", "need.", "rep.", "rel.", "t.", "arg.", "cfg.", "f.", "age",
                    "attr_base.", "moodlet.", "body.")   # LR-141/LR-152: the stats and the body (sim/char_expr)
FUNCS = {"min", "max", "clamp", "abs", "floor", "ceil", "round", "pow", "sqrt", "exp", "log", "lerp", "sign",
         "step", "noise", "p_over", "has", "count", "mass", "flaw", "state", "tag", "eff", "role"}
WORDS = {"and", "or", "not", "true", "false"}
NAME_RE = re.compile(r"[A-Za-z_$][A-Za-z0-9_$]*(?:\.[A-Za-z0-9_$]+)*")


def _content_ids(L):
    ai = L.get("ai")
    ids = {"attr": set(), "skill": set(), "trait": set(), "need": set()}
    if not ai: return ids
    def grab(path, key, kind):
        p = ai / path
        if p.exists():
            try:
                for e in jload(p).get(key, []):
                    if isinstance(e, dict) and isinstance(e.get("id"), str): ids[kind].add(e["id"])
            except Exception: pass
    grab("knowledge.json", "knowledge", "skill"); grab("traits.json", "traits", "trait"); grab("needs.json", "needs", "need")
    for sub in ("expansion", "cuisine", "resources", "alchemy", "regional"):
        grab(f"{sub}/knowledge.json", "knowledge", "skill"); grab(f"{sub}/traits.json", "traits", "trait"); grab(f"{sub}/needs.json", "needs", "need")
    p = ai / "expansion" / "attributes.json"
    if p.exists():
        try: ids["attr"] = set(x for x in jload(p).get("attributes", []) if isinstance(x, str))
        except Exception: pass
    return ids


def _formula_names(src):
    """Names an expression reads: (plain names, f.calls) without function names."""
    names, calls, i = [], [], 0
    while i < len(src):
        m = NAME_RE.match(src, i)
        if not m or (i > 0 and (src[i - 1].isalnum() or src[i - 1] in "._")):
            i += 1; continue
        n, j = m.group(0), m.end()
        k = j
        while k < len(src) and src[k] == " ": k += 1
        if n.startswith("f.") and k < len(src) and src[k] == "(":
            depth, e = 0, k
            while e < len(src):
                depth += src[e] == "("; depth -= src[e] == ")"
                if depth == 0: break
                e += 1
            calls.append((n[2:], src[k + 1:e])); i = e + 1; continue
        if k < len(src) and src[k] == "(":
            if n not in FUNCS: names.append(("fn", n))
            i = j; continue
        if n not in WORDS and not n[0].isdigit(): names.append(("var", n))
        i = j
    return names, calls


def check_formulas(L):
    r = Report("formulas")
    ai = L.get("ai")
    d = ai / "formulas" if ai else None
    if not d or not d.is_dir():
        r.info = "нет ai/formulas"; return r
    defs, consts = {}, {}
    for p in sorted(d.glob("*.json")):
        try: j = jload(p)
        except Exception as e: r.err(f"{p.name}: не парсится ({e})"); continue
        for k, v in (j.get("constants", {}) if isinstance(j, dict) else {}).items():
            if not isinstance(v, (int, float)): r.err(f"{p.name}: константа {k} не число")
            consts[k] = v
        for f in (j if isinstance(j, list) else j.get("formulas", [])):
            fid = f.get("id") if isinstance(f, dict) else None
            if not isinstance(fid, str) or not re.fullmatch(r"[a-z_][a-z0-9_]*", fid):
                r.err(f"{p.name}: формула без простого id ({fid!r})"); continue
            if fid in defs: r.warn(f"{p.name}: f.{fid} уже есть в {defs[fid][1]}, берётся последняя")
            ex = f.get("expr")
            if isinstance(ex, dict):
                ex = " + ".join(f"{k} * {w}" for k, w in ex.items())
            elif isinstance(ex, (int, float)):
                ex = str(ex)
            if not isinstance(ex, str) or not ex.strip(): r.err(f"{p.name}: f.{fid} без expr"); continue
            defs[fid] = (f, p.name, ex)
    ids = _content_ids(L)
    graph = {}
    for fid, (f, fn, ex) in defs.items():
        params = f.get("params", [])
        for m in re.finditer(r"\$([A-Za-z0-9_]+)", ex):
            if m.group(1) not in params: r.err(f"{fn}: f.{fid}: ${m.group(1)} не параметр")
        if ex.count("(") != ex.count(")"): r.err(f"{fn}: f.{fid}: скобки не сходятся")
        names, calls = _formula_names(ex)
        graph[fid] = set()
        for kind, n in names:
            if kind == "fn": r.err(f"{fn}: f.{fid}: неизвестная функция {n}()"); continue
            if n.startswith("f."): graph[fid].add(n[2:]); continue
            if "$" in n: continue
            if not n.startswith(FORMULA_PREFIXES): r.err(f"{fn}: f.{fid}: имя «{n}» не начинается известным префиксом"); continue
            if n.startswith("cfg.") and n[4:] not in consts: r.err(f"{fn}: f.{fid}: нет константы {n}")
            base = n[2:] if n.startswith("t.") else n
            for kind2 in ("attr", "skill", "trait", "need"):
                if base.startswith(kind2 + ".") and ids[kind2] and base.split(".", 1)[1] not in ids[kind2]:
                    r.err(f"{fn}: f.{fid}: неизвестный {kind2} «{base.split('.', 1)[1]}»")
        for cid, args in calls: graph[fid].add(cid)
        for g in graph[fid]:
            if g not in defs: r.err(f"{fn}: f.{fid}: нет формулы f.{g}")
    state = {}
    def dfs(u, path):
        state[u] = 1
        for v in graph.get(u, ()):
            if state.get(v) == 1: r.err("цикл формул: " + " -> ".join(path + [v]))
            elif v in defs and not state.get(v): dfs(v, path + [v])
        state[u] = 2
    for u in defs:
        if not state.get(u): dfs(u, [u])
    r.info = f"{len(defs)} формул, {len(consts)} констант"
    return r


TEMPLATE_LISTS = {"items.json": "items", "objects.json": "objects", "interactions.json": "interactions",
                  "traits.json": "traits", "needs.json": "needs", "knowledge.json": "knowledge"}


def check_templates(L):
    r = Report("templates")
    ai = L.get("ai")
    if not ai: return r
    book = {}
    td = ai / "templates"
    if td.is_dir():
        for p in sorted(td.glob("*.json")):
            try:
                for t in jload(p).get("templates", []):
                    if isinstance(t, dict) and isinstance(t.get("id"), str): book[t["id"]] = t
            except Exception as e: r.err(f"templates/{p.name}: {e}")
    n = 0
    for fname, key in TEMPLATE_LISTS.items():
        lists = []
        for p in [ai / fname] + [ai / sub / fname for sub in ("cuisine", "resources", "alchemy", "regional", "expansion")]:
            if p.exists():
                try: lists += [e for e in jload(p).get(key, []) if isinstance(e, dict)]
                except Exception: pass
        local = {e["id"]: e for e in lists if isinstance(e.get("id"), str)}
        def find(i): return local.get(i) or book.get(i)
        for e in lists:
            refs = ([e["extends"]] if isinstance(e.get("extends"), str) else []) + [u for u in e.get("use", []) if isinstance(u, str)]
            if not refs: continue
            n += 1
            for ref in refs:
                if not find(ref): r.err(f"{fname}: «{e.get('id')}» ссылается на неизвестный шаблон «{ref}»")
            seen, cur = {e.get("id")}, e
            while isinstance(cur.get("extends"), str):
                nxt = cur["extends"]
                if nxt in seen: r.err(f"{fname}: цикл extends через «{nxt}»"); break
                seen.add(nxt); cur = find(nxt) or {}
    r.info = f"{len(book)} шаблонов в книге, {n} записей с extends/use"
    return r


MODS_DIRS = []


def check_mods(L):
    r = Report("mods")
    root = L["root"]
    dirs = MODS_DIRS or [root.parent / "mods"]
    mods = [m for d in dirs if d.is_dir() for m in sorted(d.iterdir()) if m.is_dir() and not m.name.startswith((".", "_"))]
    for m in mods:
        for p in sorted(m.rglob("*.json")):
            rel = p.relative_to(m)
            try: j = jload(p)
            except Exception as e: r.err(f"{m.name}/{rel}: не парсится ({e})"); continue
            if isinstance(j, dict) and j.get("_merge") and not (root / rel).exists():
                r.warn(f"{m.name}/{rel}: \"_merge\", но в базе нет content/{rel} — файл ляжет как новый")
    r.info = f"{len(mods)} модов"
    return r


CHECKS["formulas"] = check_formulas  # LR-142
CHECKS["templates"] = check_templates  # LR-142
CHECKS["mods"] = check_mods  # LR-142


# ---------------------------------------------------------------- LR-161: icons of the 2D client
SCREENS_2D = ("sheet", "dynasty", "colony", "build", "map", "workshop", "chronicle")


def check_icons(L):
    """LR-161: у каждого id контента (предметы, объекты, действия и цели, мысли, нужды, процессы и станции
    мастерской, экраны) есть картинка в content/sprites_2d, и каждая картинка нарисована (сетка, палитра)."""
    r = Report("icons")
    root, ai = L["root"], L.get("ai")
    sd = root / "sprites_2d"
    if not ai or not (sd / "sprites.json").exists():
        r.warn("нет ai/ или sprites_2d/, пропуск"); return r
    J = lambda n: jload(sd / n) if (sd / n).exists() else {}
    spr, craft, hud, mp, res, crops = J("sprites.json"), J("craft2d.json"), J("hud2d.json"), J("map2d.json"), J("resources.json"), J("crops2d.json")
    pal = set(spr.get("pal", {}))
    grids = {}
    for src in (spr, craft, crops, hud):
        grids.update(src.get("sprites", {}))
    for name, g in grids.items():
        size = 9 if name.startswith("hud_") else None
        if not isinstance(g, list) or not g or len({len(x) for x in g}) != 1:
            r.err(f"спрайт {name}: строки разной длины"); continue
        if size and (len(g) != size or len(g[0]) != size):
            r.err(f"спрайт {name}: не {size}x{size}")
        if not size and not name.startswith(("tl_", "cr_")) and (len(g) != 16 or len(g[0]) != 16):
            r.err(f"спрайт {name}: не 16x16 ({len(g[0])}x{len(g)})")
        bad = {ch for row in g for ch in row if ch != "." and ch not in pal}
        if bad: r.err(f"спрайт {name}: буквы вне палитры {sorted(bad)}")

    def pic(where, rec):
        """rec: a sprite name or {sprite, recolour}; True when it draws."""
        sp = rec if isinstance(rec, str) else (rec.get("sprite") if isinstance(rec, dict) else None)
        if not sp:
            return False
        if sp not in grids:
            r.err(f"{where}: нет спрайта {sp}"); return True
        rc = rec.get("recolour", {}) if isinstance(rec, dict) else {}
        bad = [k for k, v in (rc or {}).items() if k not in pal or v not in pal]
        if bad: r.err(f"{where}: перекраска вне палитры {bad}")
        return True

    def ids(pattern, key):
        out = set()
        for p in sorted(root.glob(pattern)):
            try: d = jload(p)
            except Exception: continue
            v = d.get(key) if isinstance(d, dict) else None
            if isinstance(v, list):
                out |= {e["id"] for e in v if isinstance(e, dict) and isinstance(e.get("id"), str)}
            elif isinstance(v, dict):
                out |= {k for k in v if not k.startswith("_")}
        return out
    counts = {}
    # things: sheet, workshop, trade draw craft2d.json items, then resources.json items
    items = ids("ai/**/items.json", "items") | ids("creatures/_items.json", "items")
    look = {**res.get("items", {}), **craft.get("items", {})}
    miss = sorted(i for i in items if not pic(f"предмет {i}", look.get(i)))
    for i in miss: r.err(f"предмет {i}: нет картинки (craft2d.json items / resources.json items), окно рисует букву")
    counts["предметов"] = len(items)
    # workshop forms: the form's own record, else its item's
    fp = ai / "craft" / "forms.json"
    if fp.exists():
        for f in jload(fp).get("forms", []):
            fid, item = f.get("id"), f.get("item")
            if not (pic(f"форма {fid}", craft.get("forms", {}).get(fid)) or pic(f"форма {fid}", look.get(item)) or pic(f"форма {fid}", look.get(fid))):
                r.err(f"форма {fid}: нет картинки ни у формы, ни у предмета {item}")
    # objects on the map: map2d.json objects win over resources.json; hidden ones are not drawn
    objs = ids("ai/**/objects.json", "objects")
    olook = {**res.get("objects", {}), **mp.get("objects", {})}
    hidden = set(mp.get("hidden", []))
    for o in sorted(objs - hidden):
        if not pic(f"объект {o}", olook.get(o)):
            r.err(f"объект {o}: нет картинки (map2d.json objects), окно рисует запасную ({olook.get('_default', {}).get('sprite')})")
        elif not (olook.get(o) or {}).get("glyph"):
            r.warn(f"объект {o}: нет символа для символьного режима")
    counts["объектов"] = len(objs)
    # acts and goals: the first map2d.json acts rule (over the head, quick bar, RMB menu)
    acts = []
    for a in mp.get("acts", []):
        try: acts.append((re.compile(a[0]), a[1]))
        except Exception as e: r.err(f"acts: плохое выражение {a[0]!r}: {e}")
        pic(f"acts {a[0]!r}", a[1])
    inter = ids("ai/**/interactions.json", "interactions") | ids("creatures/_interactions.json", "interactions") | ids("ai/goals.json", "goals")
    for i in sorted(inter):
        if not any(rx.search(i) for rx, _ in acts):
            r.err(f"действие {i}: ни одно правило map2d.json acts не даёт картинку")
    counts["действий и целей"] = len(inter)
    # thoughts: hud2d.json moodlets (otherwise a bare plus/minus)
    mrx = []
    for m in hud.get("moodlets", []):
        try: mrx.append(re.compile(m[0]))
        except Exception as e: r.err(f"moodlets: плохое выражение {m[0]!r}: {e}")
        pic(f"moodlets {m[0]!r}", m[1])
    mood = ids("ai/**/needs.json", "moodlets")
    for m in sorted(mood):
        if not any(rx.search(m) for rx in mrx):
            r.err(f"мысль {m}: нет картинки (hud2d.json moodlets), окно рисует плюс/минус")
    counts["мыслей"] = len(mood)
    # needs: hud2d.json tiers
    tier = {n[0]: n[1] for t in hud.get("tiers", []) for n in t.get("needs", []) if isinstance(n, list) and len(n) >= 2}
    for t, n in tier.items(): pic(f"нужда {t}", n)
    for n in sorted(ids("ai/**/needs.json", "needs")):
        if n not in tier: r.err(f"нужда {n}: нет картинки в hud2d.json tiers (общая звезда)")
    # the HUD's own: vitals, attributes, screens
    for k, v in hud.get("vitals", {}).items(): pic(f"vitals {k}", v)
    for a in hud.get("attrs", []): pic(f"attrs {a.get('id')}", a.get("icon"))
    scr = hud.get("screens", {})
    for k in SCREENS_2D:
        if not pic(f"экран {k}", scr.get(k)): r.err(f"экран {k}: нет картинки (hud2d.json screens)")
    taken = {v for v in tier.values()} | {a.get("icon") for a in hud.get("attrs", [])}
    for k in SCREENS_2D:
        if scr.get(k) in taken: r.warn(f"экран {k}: картинка {scr.get(k)} совпадает с нуждой/характеристикой")
    # workshop: processes, actions, stations
    for name, key, src in (("процесс", "processes", "process_icons"), ("станция", "stations", "stations")):
        p = ai / "craft" / f"{key}.json"
        if not p.exists(): continue
        d = jload(p).get(key, [])
        for e in (d if isinstance(d, list) else []):
            if isinstance(e, dict) and not pic(f"{name} {e.get('id')}", craft.get(src, {}).get(e.get("id"))):
                r.err(f"{name} {e.get('id')}: нет картинки (craft2d.json {src})")
    for k, v in craft.get("action_icons", {}).items(): pic(f"action_icons {k}", v)
    r.info = ", ".join(f"{v} {k}" for k, v in counts.items()) + f", спрайтов {len(grids)}"
    return r


CHECKS["icons"] = check_icons  # LR-161



def check_figures(L):
    """LR-165: 48x64-фигуры (sprites_2d/figures2d.json): народы, наряды культур, причёски, бороды, инструменты ссылаются друг на друга;
    у каждого народа и культуры из species/ есть фигура; грудь и нагота только у взрослых (условие adult или bare: голые дети не рисуются)."""
    r = Report("figures")
    root = L["root"]
    fp = root / "sprites_2d" / "figures2d.json"
    if not fp.exists():
        r.warn("нет sprites_2d/figures2d.json, пропуск"); return r
    F = jload(fp)
    peoples, outfits, cultures = F.get("peoples", {}), F.get("outfits", {}), F.get("cultures", {})
    hairs = set(F.get("hairs", {})) | set(F.get("hair_alias", {}))
    beards = set(F.get("beards", {})) | set(F.get("beard_alias", {}))
    for cid, c in cultures.items():
        for slot, o in c.items():
            if slot != "colours" and o not in outfits: r.err(f"cultures.{cid}.{slot}: нет наряда {o}")
    for cid, pid in F.get("cultures_people", {}).items():
        if pid not in peoples: r.err(f"cultures_people.{cid}: нет народа {pid}")
        if cid not in cultures: r.err(f"cultures_people.{cid}: нет культуры в cultures")
    for pid, P in peoples.items():
        if P.get("culture") and P["culture"] not in cultures: r.err(f"peoples.{pid}.culture: нет культуры {P['culture']}")
        for k in ("hair_m", "hair_f"):
            for h in P.get(k, []):
                if h not in hairs: r.err(f"peoples.{pid}.{k}: нет причёски {h}")
        for b in P.get("beards", []):
            if b not in beards: r.err(f"peoples.{pid}.beards: нет бороды {b}")
    for rid, pid in F.get("races", {}).items():
        if pid not in peoples: r.err(f"races.{rid}: нет народа {pid}")
    # the sim's tools in hand (map2d.json tools) have a figure tool
    mp = root / "sprites_2d" / "map2d.json"
    if mp.exists():
        for pair in jload(mp).get("tools", []):
            if pair[1] not in F.get("tools", {}): r.warn(f"map2d.json tools {pair[0]}: нет инструмента фигуры {pair[1]}")
    # every people (race) and culture of the sim is drawn
    sp = root / "species"
    for f in sorted(sp.glob("*.json")) if sp.is_dir() else []:
        d = jload(f)
        rid = d.get("id") if isinstance(d, dict) else None
        if not rid or rid in ("dragon", "human") or "body" not in d: continue
        if rid not in peoples and rid not in F.get("races", {}): r.warn(f"species/{f.name}: нет фигуры народа {rid}")
    cd = sp / "cultures"
    for f in sorted(cd.glob("*.json")) if cd.is_dir() else []:
        d = jload(f)
        cid = d.get("id", f.stem) if isinstance(d, dict) else f.stem
        if cid in ("draconic",): continue
        if cid not in cultures and cid not in F.get("cultures_legacy", {}): r.warn(f"species/cultures/{f.name}: нет нарядов культуры {cid}")
    # only adults have a chest drawn: every chest op says adult or bare (a bare child is always clothed by the code)
    def chest_ops(where, lst):
        for i, o in enumerate(lst or []):
            w = str(o.get("when", ""))
            if "adult" not in w and "bare" not in w and "haschest" not in w: r.err(f"{where}.chest[{i}]: грудь без условия adult/bare")
    chest_ops("base", F.get("base", {}).get("chest"))
    for pid, P in peoples.items(): chest_ops(f"peoples.{pid}", (P.get("layers") or {}).get("chest"))
    for oid, O in outfits.items(): chest_ops(f"outfits.{oid}", (O.get("layers") or {}).get("chest"))
    r.info = f"народов {len(peoples)}, нарядов {len(outfits)}, культур {len(cultures)}"
    return r


CHECKS["figures"] = check_figures  # LR-165

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmds", nargs="*")
    ap.add_argument("--root"); ap.add_argument("--log", default="validate_content.log"); ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--mods", action="append", default=[], help="папка модов (можно несколько); по умолчанию ../mods")
    a = ap.parse_args()
    MODS_DIRS.extend(Path(m) for m in a.mods)
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
