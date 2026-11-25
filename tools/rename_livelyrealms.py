#!/usr/bin/env python3
"""AL-119: rename the game Altera -> LivelyRealms and its engine Campfire -> RealSimEngine.

Idempotent; run from the repository root over tracked text files (or over the
files given as arguments, e.g. a parallel branch's own files after a rebase):

    python tools/rename_livelyrealms.py            # every tracked text file
    python tools/rename_livelyrealms.py a.cpp b.json

Rules (gdd/setting/LIVELYREALMS_SETTING_v0.2.md §13):
- C++ namespace altera -> lr (altera::sim -> lr::sim);
- identifiers altera_* -> lr_* (targets, tests, test files test_altera_* -> test_lr_*),
  macros ALTERA_* -> LR_*;
- the display word Altera -> LivelyRealms ("Lively Realms" in window titles);
- the engine of this repo: CMake aliases Campfire:: -> RealSimEngine::,
  campfire_engine_editor -> realsim_editor, CAMPFIRE_* build variables -> REALSIM_*,
  the word Campfire inside src/engine -> RealSimEngine.
Left alone: doc/ and third_party/ (user / vendored), binaries, the folder
D:/AlteraGame/altera, lowercase "altera" in save-format ids (altera.save/1 stays
readable), and "Campfire" where it names the other engine (CampfireLite ports,
the settlement generator) or the in-game campfire object.
"""
import re
import subprocess
import sys
from pathlib import Path

SKIP_DIRS = ("doc/", "third_party/")
TEXT_EXT = {".cpp", ".hpp", ".h", ".inl", ".c", ".cc", ".txt", ".cmake", ".json", ".md", ".py", ".bat", ".sh",
            ".ps1", ".xml", ".hlsl", ".hlsli", ".html", ".js", ".css", ".toml", ".yml", ".yaml", ".ini", ".cfg"}
TEXT_NAMES = {"CMakeLists.txt", "CMakePresets.json", "CLAUDE.md", "run.sh", ".gitignore"}

RULES = [
    (re.compile(r"\bnamespace altera\b"), "namespace lr"),
    (re.compile(r"(?<![A-Za-z0-9_])altera::"), "lr::"),
    (re.compile(r"(?<![A-Za-z0-9])test_altera_"), "test_lr_"),
    (re.compile(r"(?<![A-Za-z0-9_])altera_"), "lr_"),
    (re.compile(r"(?<![A-Za-z0-9_])ALTERA_"), "LR_"),
    (re.compile(r"\bAltera(?=\s+[-—:]|\"\s*,\s*\d)"), "Lively Realms"),  # window and page titles
    (re.compile(r"\bAltera\b"), "LivelyRealms"),
    (re.compile(r"\bCampfire::"), "RealSimEngine::"),
    (re.compile(r"\bcampfire_engine_editor"), "realsim_editor"),
    (re.compile(r"\bCAMPFIRE_ENGINE_EDITOR"), "REALSIM_EDITOR"),
    (re.compile(r"\bCAMPFIRE_"), "REALSIM_"),
]
# "Campfire" as this repo's engine: only in these places
ENGINE_WORD_FILES = ("src/engine/", "CLAUDE.md", "cmake/dlss.cmake")
ENGINE_WORD = (re.compile(r"\bCampfire\b(?!::)"), "RealSimEngine")


def is_text(p: str) -> bool:
    path = Path(p)
    return path.suffix.lower() in TEXT_EXT or path.name in TEXT_NAMES or p.startswith(".idea/")


def rename_text(p: str, s: str) -> str:
    for rx, to in RULES:
        s = rx.sub(to, s)
    if p.startswith(ENGINE_WORD_FILES):
        s = ENGINE_WORD[0].sub(ENGINE_WORD[1], s)
    return s


def main(argv):
    files = argv or subprocess.run(["git", "ls-files"], capture_output=True, text=True, check=True).stdout.split()
    changed, moved = 0, 0
    for p in files:
        if p.startswith(SKIP_DIRS) or p.endswith("rename_livelyrealms.py") or not is_text(p) or not Path(p).is_file():
            continue
        try:
            s = Path(p).read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        t = rename_text(p, s)
        if t != s:
            Path(p).write_bytes(t.encode("utf-8"))
            changed += 1
        name = Path(p).name
        if "altera_" in name and not p.startswith(SKIP_DIRS):
            new = str(Path(p).with_name(re.sub(r"(?<![A-Za-z0-9])(test_)?altera_", lambda m: (m.group(1) or "") + "lr_", name)))
            if new != p:
                subprocess.run(["git", "mv", p, new], check=True)
                moved += 1
    # directories named altera_* (assets/altera_game, tests/data/altera_mvp)
    if not argv:
        dirs = sorted({str(Path(*Path(f).parts[:i + 1])) for f in files if not f.startswith(SKIP_DIRS)
                       for i, part in enumerate(Path(f).parts[:-1]) if re.match(r"altera_", part)}, key=len)
        for d in dirs:
            if Path(d).is_dir():
                new = str(Path(d).with_name(re.sub(r"^altera_", "lr_", Path(d).name)))
                subprocess.run(["git", "mv", d, new], check=True)
                moved += 1
    print(f"changed {changed} files, moved {moved}")


if __name__ == "__main__":
    main(sys.argv[1:])
