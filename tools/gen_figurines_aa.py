#!/usr/bin/env python3
# Generates src/ekmod/figurines_levels_aa.h -- for every level AA's CON names
# with `definelevelname`, which DISTINCT figurine picnums (MINIDUKE..
# MINIBOSS2FLY, 16010..16060, AADEFS.CON:2108-2158) its map file places.
#
# WHY (AA25b, 2026-09-28): the counter used to be rebuilt from live sprites,
# so a save loaded after a pickup lost that pickup (1 of 4 found read 0 / 3).
# The map file is the level's own history-free total; AA's collected bits say
# which of those are owned. Together they give X / Y no load can disturb.
#
# THE CON PARSE is gamedef.cpp:5737-5787's: `definelevelname V L file ...`,
# V < MAXVOLUMES (7) and L < MAXLEVELS (64), and a later line for the same
# slot OVERWRITES the filename (Bstrcpy), so the last definition wins here.
# `//` and `/* */` comments are skipped, as the CON lexer does.
#
# THE MAP PARSE is the v7/8/9 layout (scratchpad aa25_figsurvey.py, 384 maps,
# 0 unparsed): header 20 bytes, sectors 40, walls 32, sprites 44 with picnum
# at +14 and hitag at +40. A map the parse refuses is listed, never guessed.
#
# ONLY HITAG 0 IS A FIND. AAPRELOAD.CON:3669-3686 (EVENT_LOADACTOR) moves each
# figurine's map hitag into `mtype` and counts only mtype 0 into
# collectinlevel; AAITEMS.CON:4195-4203 stops any other before the pickup
# code -- a display piece (the hubs' trophy shelves). Those are counted apart.
#
# NO SKILL GATE. The same event zeroes lotag (:3675-3676) before
# G_SpawnAllSprites (premap.cpp:1370-1380) reaches A_Spawn's
# `lotag > ud.player_skill` deletion (game.cpp:3521), so it never fires here.
#
# RUN BY HAND, NOT BY build.ps1 -- the build must not need the game installed:
#   python tools/gen_figurines_aa.py [--game-dir <game folder>]
# Writes CRLF, like every source file in this repo.

import argparse
import os
import re
import struct
import sys
import textwrap
from pathlib import Path

FIG_LO, FIG_HI = 16010, 16060        # AADEFS.CON:2108, :2158
MAXVOLUMES, MAXLEVELS = 7, 64        # duke3d.h:72-73

LINE_RE = re.compile(r"^\s*definelevelname\s+(\S+)\s+(\S+)\s+(\S+)", re.I)


def strip_comments(text):
    """`/* */` blocks and `//` tails blanked, newlines kept so line numbers hold."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def resolve_ci(root, rel):
    """rel under root, each component matched case-insensitively; None if absent."""
    cur = root
    for part in [p for p in rel.replace("\\", "/").split("/") if p]:
        if not cur.is_dir():
            return None
        hit = None
        for e in os.listdir(str(cur)):
            if e.lower() == part.lower():
                hit = cur / e
                break
        if hit is None:
            return None
        cur = hit
    return cur if cur.is_file() else None


def map_figurines(path):
    """(find mask, finds, display pieces) or None if unparsed."""
    b = path.read_bytes()
    try:
        ver = struct.unpack_from("<i", b, 0)[0]
        if ver not in (7, 8, 9):
            return None
        o = 20
        ns = struct.unpack_from("<H", b, o)[0]; o += 2 + ns * 40
        nw = struct.unpack_from("<H", b, o)[0]; o += 2 + nw * 32
        nsp = struct.unpack_from("<H", b, o)[0]; o += 2
        if o + nsp * 44 > len(b):
            return None
        mask, figs, display = 0, 0, 0
        for i in range(nsp):
            s = o + i * 44
            pic = struct.unpack_from("<h", b, s + 14)[0]
            if not (FIG_LO <= pic <= FIG_HI):
                continue
            if struct.unpack_from("<h", b, s + 40)[0] != 0:
                display += 1
                continue
            figs += 1
            mask |= 1 << (pic - FIG_LO)
        return mask, figs, display
    except struct.error:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-dir", default=os.environ.get("AA_GAME_DIR"),
                    required="AA_GAME_DIR" not in os.environ,
                    help="<AA game folder>, the AA install root (or set AA_GAME_DIR)")
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent.parent
                                         / "src" / "ekmod" / "figurines_levels_aa.h"))
    a = ap.parse_args()
    game = Path(a.game_dir)

    con_files = sorted([p for p in (game / "scripts").iterdir() if p.suffix.lower() == ".con"]
                       + [p for p in game.iterdir() if p.suffix.lower() == ".con"],
                       key=lambda p: str(p).lower())
    slots, files_with = {}, []
    for cf in con_files:
        text = strip_comments(cf.read_bytes().decode("latin-1"))
        hits = 0
        for ln, line in enumerate(text.split("\n"), 1):
            m = LINE_RE.match(line)
            if not m:
                continue
            try:
                v, l = int(m.group(1)), int(m.group(2))
            except ValueError:
                continue
            if not (0 <= v < MAXVOLUMES and 0 <= l < MAXLEVELS):
                continue
            slots[(v, l)] = (m.group(3), "%s:%d" % (cf.name, ln))   # last wins
            hits += 1
        if hits:
            files_with.append("%s (%d)" % (cf.name, hits))

    rows, missing, unparsed, all_mask = [], [], [], 0
    fig_sprites = display_total = 0
    for (v, l) in sorted(slots):
        fname, src = slots[(v, l)]
        p = resolve_ci(game, fname)
        if p is None:
            missing.append((v, l, fname, src))
            continue
        r = map_figurines(p)
        if r is None:
            unparsed.append((v, l, fname, src))
            continue
        mask, figs, display = r
        fig_sprites += figs
        display_total += display
        if mask:
            rows.append((v, l, mask, figs, fname, src))
            all_mask |= mask

    distinct = bin(all_mask).count("1")
    L = []
    w = L.append
    w("#pragma once")
    w("")
    def spans(items):
        # "volume 4 levels 45-47" for each run of consecutive levels in one volume.
        runs = []
        for v, l in sorted((v, l) for v, l, _f, _s in items):
            if runs and runs[-1][0] == v and runs[-1][2] + 1 == l:
                runs[-1][2] = l
            else:
                runs.append([v, l, l])
        return "; ".join("volume %d level%s" % (v, " %d" % a if a == b else "s %d-%d" % (a, b))
                         for v, a, b in runs)

    bad = []
    if missing:
        bad.append("%d maps missing (%s, not on disk)" % (len(missing), spans(missing)))
    if unparsed:
        bad.append("%d unparsed (%s)" % (len(unparsed), spans(unparsed)))
    intro = ("// Generated by tools/gen_figurines_aa.py from the AA install: DO NOT HAND-EDIT. One row per "
             "`definelevelname` slot whose map places figurines; mask bit n = picnum 16010+n "
             "(esp_figurines_aa.h's flat index). %d slots listed, %s%d with figurines, %d distinct types."
             % (len(slots), "".join(b + ", " for b in bad), len(rows), distinct))
    for line in textwrap.wrap(intro, width=120, subsequent_indent="// ", break_long_words=False,
                              break_on_hyphens=False):
        w(line)
    w("")
    w("struct EkFigLevel { int volume, level; unsigned long long mask; };")
    w("")
    w("const int EKM_FIG_LEVELS_LISTED   = %d;" % len(slots))
    w("const int EKM_FIG_LEVELS_MISSING  = %d;" % len(missing))
    w("const int EKM_FIG_LEVELS_UNPARSED = %d;" % len(unparsed))
    w("const int EKM_FIG_LEVELS_COUNT    = %d;" % len(rows))
    w("const int EKM_FIG_DISTINCT_TYPES  = %d;" % distinct)
    w("const unsigned long long EKM_FIG_ALL_MASK = 0x%013XULL;" % all_mask)
    w("const int EKM_FIG_SPRITES_READ    = %d;" % (fig_sprites + display_total))
    w("const int EKM_FIG_SPRITES_COUNTED = %d;" % fig_sprites)
    w("const int EKM_FIG_SPRITES_DISPLAY = %d;" % display_total)
    w("")
    w("// Sorted by (volume, level) -- ekm_figurine_level_mask relies on it.")
    w("const EkFigLevel EKM_FIG_LEVELS[EKM_FIG_LEVELS_COUNT] = {")
    for (v, l, mask, figs, fname, src) in rows:
        w("  { %d, %2d, 0x%013XULL }," % (v, l, mask))
    w("};")
    w("")
    with open(a.out, "w", newline="") as fh:
        fh.write("\r\n".join(L))
    print("CON files:", ", ".join(files_with))
    for (v, l, fname, src) in missing:
        print("MISSING: volume %d level %d, %s (%s) -- not on disk, not guessed." % (v, l, fname, src))
    for (v, l, fname, src) in unparsed:
        print("UNPARSED: volume %d level %d, %s (%s)." % (v, l, fname, src))
    print("listed %d, missing %d, unparsed %d, with figurines %d, finds %d, display %d, distinct %d"
          % (len(slots), len(missing), len(unparsed), len(rows), fig_sprites, display_total, distinct))
    return 0


if __name__ == "__main__":
    sys.exit(main())
