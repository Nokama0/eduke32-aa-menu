#!/usr/bin/env python3
# AA6e census: which enemy picnums resolve to NO name in the ESP's name chain
# (esp.cpp ekm_actor_display_name), so the label would print a bare number or,
# at best, a names.h code name. Read-only; prints a report, writes nothing.
#   python tools/aa6e_unnamed_enemies.py --game-dir <AA game folder> (or set AA_GAME_DIR)
# Census = the engine-flagged picnums the LAST `tilenames: inventory of N` block
# in the mod log lists, plus the admitted notenemy/worm picnums (aa_notenemy.h,
# aa_worm.h). Chain modelled statically: curated, nameplate, derived string,
# derived base (base named by curated/nameplate/derived string). The team-3
# "Civilian" default arm is runtime state and is not modelled. (2026-09-29)
import argparse
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import gen_esp_names_derived_aa as g

ADMITTED = {7220, 7727, 23552, 22323, 22325}  # POLYMORPH, ROLLYTURRET, SSLIME, PODHEAD, PODTAIL


def inventory(log_path):
    lines = log_path.read_text(encoding="latin-1").splitlines()
    start = max(i for i, l in enumerate(lines) if "tilenames: inventory of" in l)
    total = int(re.search(r"inventory of (\d+)", lines[start]).group(1))
    pics = []
    for l in lines[start + 1:]:
        # Other subsystems' lines interleave with the block; keep reading
        # `tilenames:` lines until the advertised count is collected.
        if l.startswith("tilenames: "):
            pics += [int(m) for m in re.findall(r"(\d+)=", l)]
        if len(pics) >= total:
            break
    return total, pics


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-dir", default=os.environ.get("AA_GAME_DIR"),
                    required="AA_GAME_DIR" not in os.environ,
                    help="<AA game folder>, the AA install root (or set AA_GAME_DIR)")
    ap.add_argument("--repo-dir", default=str(Path(__file__).parent.parent))
    a = ap.parse_args()
    game, repo = Path(a.game_dir), Path(a.repo_dir)
    total, logged = inventory(game / "ekmod.log")
    pics = set(logged) | ADMITTED
    src = repo / "src" / "ekmod"
    curated = g.load_curated_set(src / "esp_names.h")
    plate = g.load_nameplate_set(src / "esp_names_aa.h")
    dtext = (src / "esp_names_derived_aa.h").read_text(encoding="utf-8")
    dstr = set(int(x) for x in re.findall(r'case (\d+): return "', dtext))
    dbase = {int(p): int(b) for p, b in re.findall(r'case (\d+): return (\d+);', dtext)}
    names = g.parse_names_h(game / "names.h")
    defs = g.parse_defs_con(game / "scripts" / "AADEFS.CON")
    rev = {}
    for m in (defs, names):
        for k, v in m.items():
            rev.setdefault(v, k)
    named = lambda p: p in curated or p in plate or p in dstr
    code = set(names.values())
    out = []
    for p in sorted(pics):
        if named(p) or (p in dbase and named(dbase[p])):
            continue
        out.append((p, rev.get(p, "?"), "code-name" if p in code else "NUMBER"))
    print(f"log inventory says {total}, parsed {len(logged)}, plus admitted -> {len(pics)} picnums")
    for p, s, t in out:
        print(f"{p:6} {s:24} {t}")
    n = sum(1 for o in out if o[2] == "NUMBER")
    print(f"unnamed total {len(out)} (NUMBER {n}, code-name {len(out) - n})")


if __name__ == "__main__":
    main()
