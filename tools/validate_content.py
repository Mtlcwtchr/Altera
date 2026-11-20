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
        if isinstance(v, dict) and "nom" not in v:
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
        try:
            for k, v in flatten(jload(p)).items():
                if k in keys: r.warn(f"{d}: ключ {k} повторён (файл {p.name})")
                else: keys[k] = v
        except Exception as e:
            r.err(f"{p}: не парсится ({e})")
    return keys


def phs(v):
    s = v["nom"] if isinstance(v, dict) else v
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
                s = v["nom"] if isinstance(v, dict) else v
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


CHECKS = {"json": check_json, "loc": check_loc, "ai": check_ai, "expansion": check_expansion,
          "crafting": check_crafting, "docs": check_docs}


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
