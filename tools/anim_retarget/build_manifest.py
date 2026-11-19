"""Write the clip manifest and the gap report for the retargeted humanoid clips.

python -I build_manifest.py --jobs jobs.json --results results_*.jsonl
       --clip-map clip_map.json --interactions interactions.json
       --out-dir <repo>/content/animations/humanoid_ual

Outputs: manifest.json (clips + key bindings) and GAPS.md (what is missing or weak).
"""
import argparse
import glob
import json
import os
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bindings  # noqa: E402

LICENSE = {
    "ual": {"source": "Quaternius Universal Animation Library 1/2 (Standard)", "license": "CC0",
            "redistribute": True},
    "mixamo": {"source": "Adobe Mixamo", "license": "Mixamo terms: free in games and films",
               "redistribute": False,
               "note": "Only inside the built game; not as standalone animation files or packs."},
    "iglesias": {"source": "Kevin Iglesias FREE packs (itch.io)", "license": "free, commercial use allowed",
                 "redistribute": False, "note": "Do not redistribute or resell the clips as a pack."},
    "motifect": {"source": "Motifect Daily Life v1.0", "license": "free in projects",
                 "redistribute": False, "note": "No resale as a set."},
    "cmu": {"source": "CMU Graphics Lab Motion Capture Database (mocap.cs.cmu.edu)",
            "license": "free to use, including commercial (per database page)", "redistribute": True,
            "note": "Confirm with the authors before release (project decision)."},
}
LOOP_SEAM_DEG = 8.0      # last frame within this of the first: treat as a loop
TRAVEL_IN_PLACE_M = 0.05  # pelvis xy travel below this: in place


def pack_family(pack):
    if pack.startswith("ual"):
        return "ual"
    if pack.startswith("mx_"):
        return "mixamo"
    if pack.startswith("igl_"):
        return "iglesias"
    if pack == "mot":
        return "motifect"
    return "cmu"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", required=True)
    ap.add_argument("--results", required=True, nargs="+")
    ap.add_argument("--clip-map", required=True)
    ap.add_argument("--interactions", required=True)
    ap.add_argument("--out-dir", required=True)
    a = ap.parse_args()
    out_dir = a.out_dir
    jobs = {j["id"]: j for j in json.load(open(a.jobs, encoding="utf-8"))}
    res = {}
    for pat in a.results:
        for p in glob.glob(pat):
            for line in open(p, encoding="utf-8"):
                if line.strip():
                    r = json.loads(line)
                    res[r["id"]] = r

    clips, failed = {}, []
    for cid, j in jobs.items():
        r = res.get(cid)
        if not r or not r.get("ok") or not os.path.exists(j["out"]):
            failed.append((cid, (r or {}).get("error", "not run")))
            continue
        fam = pack_family(j["pack"])
        seam = r.get("loop_seam_deg")
        travel = r.get("hips_travel_m") or r.get("hips_travel_local") or [0, 0, 0]
        name_loop = "loop" in cid
        e = {
            "file": os.path.relpath(j["out"], out_dir).replace("\\", "/"),
            "pack": j["pack"], "family": fam,
            "source_name": j.get("source_name"),
            "frames": r.get("frames"), "fps": 30, "duration_s": r.get("duration_s"),
            "loop": bool(name_loop or (seam is not None and seam <= LOOP_SEAM_DEG)),
            "loop_seam_deg": seam,
            "root_motion_m": [round(travel[0], 3), round(travel[1], 3)] if fam != "ual" else None,
            "in_place": (abs(travel[0]) + abs(travel[1])) < TRAVEL_IN_PLACE_M if fam != "ual" else None,
            "fingers": r.get("fingers"),
            "ground_shift_m": r.get("ground_shift_m"),
        }
        if j.get("female"):
            e["variant"] = "female"
        if j.get("description"):
            e["description"] = j["description"]
        if j.get("pair_role"):
            e["pair"] = {"role": j["pair_role"], "take": j["pair_take"]}
        clips[cid] = {k: v for k, v in e.items() if v is not None}

    cmap = json.load(open(a.clip_map, encoding="utf-8"))["map"]
    inter_ids = [i["id"] for i in json.load(open(a.interactions, encoding="utf-8"))["interactions"]]

    missing_refs = defaultdict(list)

    def bind(table, keys, meta=None):
        out = {}
        for k in keys:
            want = table.get(k, [])
            have = [c for c in want if c in clips]
            for c in want:
                if c not in clips:
                    missing_refs[c].append(k)
            m = (meta or {}).get(k, {})
            entry = {"clips": have, "status": ("covered" if have else "gap")}
            if m.get("status") == "proc" and have:
                entry["status"] = "covered_proc"
            if m.get("recipe"):
                entry["recipe"] = m["recipe"]
            out[k] = entry
        return out

    combat_keys = [k for k in cmap if k not in bindings.OTHER_RIG]
    combat = bind(bindings.COMBAT, combat_keys, cmap)
    inter = bind(bindings.INTERACTION, inter_ids)
    extras = {k: [c for c in v if c in clips] for k, v in bindings.EXTRAS.items()}
    for k, v in bindings.EXTRAS.items():
        for c in v:
            if c not in clips:
                missing_refs[c].append(f"extras:{k}")

    used = {c for e in list(combat.values()) + list(inter.values()) for c in e["clips"]} | {
        c for v in extras.values() for c in v}
    manifest = {
        "_doc": "Humanoid clips on the Quaternius UAL rig (bones root, pelvis, spine_01..03, neck_01, Head, "
                "clavicle/upperarm/lowerarm/hand_l|r, fingers, thigh/calf/foot/ball_l|r). One GLB per clip, "
                "one animation named as the clip id, 30 fps, frames from 0. Root bone is still; any travel is "
                "on the pelvis (root_motion_m is pelvis xy travel over the clip, metres, Blender axes Z-up, "
                "character faces -Y). Built by tools/anim_retarget (see README there). keys.combat binds "
                "animations.json / clip_map.json names, keys.interaction binds interactions.json ids; first "
                "clip is the preferred pick, picks are by name and need a look in a viewer.",
        "rig": "quaternius_ual", "fps": 30,
        "licenses": LICENSE,
        "clips": dict(sorted(clips.items())),
        "keys": {"combat": combat, "interaction": inter, "other_rig": bindings.OTHER_RIG},
        "extras": extras,
    }
    os.makedirs(out_dir, exist_ok=True)
    json.dump(manifest, open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8"),
              ensure_ascii=False, indent=1)

    # ------------------------------------------------------------------ report
    fam_count = Counter(c["family"] for c in clips.values())
    pack_count = Counter(c["pack"] for c in clips.values())
    L = ["# Анимации на риге Quaternius UAL: покрытие и пробелы", "",
         f"Собрано клипов: **{len(clips)}** (ошибок выгрузки: {len(failed)}). "
         f"По источникам: " + ", ".join(f"{k} {v}" for k, v in sorted(fam_count.items())) + ".", "",
         "Файлы: `content/animations/humanoid_ual/clips/<pack>/<id>.glb`, список и привязки: `manifest.json`. "
         "Подбор клипов к ключам сделан по названиям и выборочным превью; перед использованием посмотреть в просмотрщике.", ""]

    def section(title, table, keys_meta=None):
        gaps = [k for k, e in table.items() if e["status"] == "gap"]
        proc = [k for k, e in table.items() if e["status"] == "covered_proc"]
        cov = [k for k, e in table.items() if e["status"] == "covered"]
        L.append(f"## {title}")
        L.append(f"Ключей: {len(table)}. Покрыто клипом: {len(cov)}, клип плюс процедурная доработка: {len(proc)}, "
                 f"пробелов: {len(gaps)}.")
        L.append("")
        if gaps:
            L.append("Пробелы (клипа нет): " + ", ".join(f"`{g}`" for g in gaps) + ".")
            L.append("")
        return gaps

    cg = section("Бой (clip_map.json / animations.json)", combat)
    L.append("Звериные ключи (" + ", ".join(f"`{k}`" for k in bindings.OTHER_RIG) +
             ") относятся к ригу животных Quaternius, в эту работу не входят.")
    L.append("")
    ig = section("Взаимодействия (interactions.json)", inter)
    weak = [k for k, e in inter.items() if e["status"] == "covered" and all(c.startswith(("ual1__idle", "ual1__walk")) for c in e["clips"])]
    L += ["## Что закрыто только приблизительно",
          "Ключи ниже получили клип, но он лишь близок по смыслу и требует доработки позой или предметом:",
          "",
          "- Ремёсла (`knap_blade`, `make_axe`, `make_spear`, `make_needle`, `make_spindle`, `make_digging_stick`): "
          "база Iglesias HammeringGround и CMU slicing/chopping, нужен предмет в руке и поза над верстаком.",
          "- Прядение, ткачество, шитьё (`spin_thread`, `weave_cloth`, `sew_*`, `twist_cord`): CMU 79_05/80_04 sewing (стоя); "
          "для сидячего варианта соединить с `ual1__sitting_idle_loop` (ноги) по рецепту GAP_ANIMATIONS_HOWTO.",
          "- Готовка (`cook_*`, `brew_remedy`): CMU mixing batter / hand mixing dough, стоя за столом; у костра нужен вариант на корточках.",
          "- Парные сцены (`comfort`, `help_with_task`, `restrain`, `give_food`): CMU 18–23, обе роли A и B в одном дубле "
          "(поле `pair` в манифесте), синхронизировать по кадрам и выставить актёров по исходной съёмке.",
          "- Обогрев у огня (`warm_at_*`): CMU cold/freezing (зябнет), а не руки к огню.",
          "- Суд, совет, законы (`judge_case`, `propose_law`, `call_council`, `vote`): общие жесты речи.", ""]
    if weak:
        L.append("Ключи, где стоит только idle/walk: " + ", ".join(f"`{k}`" for k in weak) + ".")
        L.append("")
    L += ["## Чего нет ни в одном источнике (делать вручную или процедурно)",
          "- Добивание на коленях (`exec_kneel_stab`, `execute`), связывание рук (`bind_hands`) и освобождение (`release_captive`).",
          "- Перевязка и жгут (`bandage_arm`, `bandage_torso`, `tourniquet_leg`), перенос раненого на плече (`carry_shoulder`).",
          "- Финт глазами (`feint_eye_dart`), протирка клинка (`wipe_blade`).",
          "- Интимная сцена (`intimacy`), парный поцелуй.",
          "- Ковка у наковальни стоя, свежевание туши, помешивание котла у костра (есть только близкие базы).",
          "", "## Качество ретаргета",
          "- Конечности (плечо, предплечье, бедро, голень) совпадают с источником по направлению; ключицы, кисти, стопы и пальцы "
          "переносятся поворотом от покоя без подгонки (у ригов разное строение).",
          "- Таз ставится по нижней лодыжке клипа на высоту покоя UAL (поле `ground_shift_m`); у CMU поправка 3–4 см.",
          "- Пальцы есть у Mixamo и Motifect; у Iglesias и CMU кисть без пальцев (пальцы в позе покоя).",
          "- Перемещение по миру лежит на кости pelvis (`root_motion_m`), кость root неподвижна. Для движка с root motion "
          "перенести xy таза в root при импорте.",
          ""]
    big_seam = sorted((c["loop_seam_deg"], k) for k, c in clips.items()
                      if "loop" in k and c.get("loop_seam_deg", 0) > LOOP_SEAM_DEG)
    if big_seam:
        L.append("Клипы с `loop` в имени, но со скачком на шве больше 8°: " +
                 ", ".join(f"`{k}` ({s}°)" for s, k in big_seam[:20]) + ".")
        L.append("")
    if failed:
        L.append("## Ошибки выгрузки")
        for cid, err in failed:
            L.append(f"- `{cid}`: {err[:160]}")
        L.append("")
    if missing_refs:
        L.append("## Ссылки привязок на несуществующие клипы")
        for c, ks in sorted(missing_refs.items()):
            L.append(f"- `{c}` (из {', '.join(ks)})")
        L.append("")
    L += ["## Лицензии",
          "- Quaternius UAL: CC0.",
          "- Mixamo: только внутри игры, не распространять клипы отдельными файлами или паками.",
          "- Kevin Iglesias FREE: коммерческое использование разрешено, сами паки не перепродавать и не раздавать.",
          "- Motifect Daily Life: свободно в проектах, без перепродажи набором.",
          "- CMU: по странице базы свободно для любых целей; перед релизом подтвердить у авторов (решение проекта).",
          "- Поэтому GLB лежат вне git (папка clips в .gitignore) и пересобираются скриптом из исходников в Загрузках.",
          "", f"Неиспользованных в привязках клипов: {len(set(clips) - used)} (остаются в манифесте как запас).",
          "", "Клипов по пакам: " + ", ".join(f"{k} {v}" for k, v in sorted(pack_count.items())) + "."]
    open(os.path.join(out_dir, "GAPS.md"), "w", encoding="utf-8").write("\n".join(L) + "\n")
    print("clips", len(clips), "failed", len(failed), "combat gaps", len(cg), "interaction gaps", len(ig),
          "missing refs", len(missing_refs))


if __name__ == "__main__":
    main()
