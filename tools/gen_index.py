#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Генератор doc/INDEX.md: одна строка на файл кода = первая содержательная строка шапки (комментарий/докстринг).
Чтобы индекс был полезным, пишите в начале файла одну строку «что это» (// ..., # ..., /* ... */, \"\"\" ... \"\"\").
  python tools/gen_index.py            пересобрать doc/INDEX.md
  python tools/gen_index.py --check    код 1, если индекс устарел (для проверки перед PR)
  python tools/gen_index.py --missing  файлы без шапки (кандидаты на однострочную шапку)"""
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CODE = {".cpp", ".hpp", ".h", ".c", ".cc", ".hlsl", ".hlsli", ".py", ".bat", ".sh", ".cmake"}
DIRS = ["src", "tests", "tools"]
SKIP = {"third_party", "__pycache__", ".git", "build", "out"}
SKIP_PREFIX = ("cmake-build",)
MAXLEN = 110


def header(p: Path):
    try:
        lines = p.read_text(encoding="utf-8", errors="ignore").splitlines()[:40]
    except Exception:
        return ""
    in_block = False
    for l in lines:
        s = l.strip()
        if not s or s.startswith(("#!", "#pragma", "#include", "# -*-", "@echo", "#ifndef", "#define", "import ", "from ")): continue
        if s.lower().startswith(("// copyright", "# copyright", "// license", "// spdx", "# spdx")): continue
        if in_block or s.startswith(("/*", '"""', "'''")):
            in_block = True
            t = re.sub(r'^(/\*+|"""|\'\'\')', "", s).strip(" */\"'")
            if s.endswith(("*/", '"""', "'''")) and len(s) > 3: in_block = False
            if t: return t[:MAXLEN]
            continue
        if s.startswith(("//", "#", "::", "REM ", "rem ")):
            t = s.lstrip("/#: ").strip()
            t = re.sub(r"^(REM|rem)\s+", "", t)
            if t and not set(t) <= set("-=*_ "): return t[:MAXLEN]
            continue
        return ""    # первая строка — код: шапки нет
    return ""


def header_or_pair(p: Path):
    h = header(p)
    if not h and p.suffix in (".cpp", ".cc"):
        for e in (".hpp", ".h"):
            q = p.with_suffix(e)
            if q.exists(): return header(q)
    return h


def build():
    out = ["# Индекс кода", "", "Автогенерация: `python tools/gen_index.py` (не править руками). Одна строка на файл = его шапка. Карточки модулей: `doc/modules/`.", ""]
    missing = []
    for d in DIRS:
        base = ROOT / d
        if not base.is_dir(): continue
        groups = {}
        for p in sorted(base.rglob("*")):
            rel = p.relative_to(ROOT)
            if any(x in SKIP or x.startswith(SKIP_PREFIX) for x in rel.parts) or p.suffix not in CODE or not p.is_file(): continue
            groups.setdefault(str(rel.parent).replace("\\", "/"), []).append((p.name, header_or_pair(p)))
        if not groups: continue
        out.append(f"## {d}/")
        for g, files in groups.items():
            out.append(f"### {g}")
            for n, h in files:
                out.append(f"- `{n}`" + (f" — {h}" if h else ""))
                if not h: missing.append(f"{g}/{n}")
            out.append("")
    c = ROOT / "content"
    if c.is_dir():
        out.append("## content/ (данные)")
        for p in sorted(c.iterdir()):
            if p.is_dir():
                n = sum(1 for _ in p.rglob("*.json"))
                out.append(f"- `content/{p.name}/` — {n} json")
        out.append("")
    return "\n".join(out), missing


def main():
    text, missing = build()
    target = ROOT / "doc" / "INDEX.md"
    if "--missing" in sys.argv:
        print("\n".join(missing)); return
    if "--check" in sys.argv:
        ok = target.exists() and target.read_text(encoding="utf-8") == text
        print("INDEX.md актуален" if ok else "INDEX.md устарел: python tools/gen_index.py")
        sys.exit(0 if ok else 1)
    target.write_text(text, encoding="utf-8")
    print(f"doc/INDEX.md: {text.count(chr(10)) + 1} строк, без шапки {len(missing)} файлов")


if __name__ == "__main__":
    main()
