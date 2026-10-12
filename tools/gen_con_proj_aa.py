#!/usr/bin/env python3
# Generates src/ekmod/con_proj_aa.h -- picnum -> { hitscan, vel } for every
# projectile Alien Armageddon's OWN CON declares, plus a fingerprint of the
# files it was read from.
#
# WHY THIS EXISTS. The aim lead is decided from the stock Duke weapon SLOT,
# and AA reassigns what each slot shoots at runtime -- WEAPON7_SHOOTS
# (the Devastator slot) is set to BARRETPROJ, which AADEFS.CON declares
# PROJ_WORKSLIKE 1179649, and bit 0 of that is PROJECTILE_HITSCAN
# (duke3d/src/actors.h:368). So the Penetrator was led as if it fired a 644
# rocket. It goes the other way too: WEAPON1_SHOOTS (the pistol slot) is set
# to MANDOLASER, a real 768 projectile the stock table refuses to lead.
#
# WHAT DECIDES MEMBERSHIP, AND IT IS THE ENGINE'S OWN BRANCH. A tile lands in
# this table if and only if AA declares at least one `defineprojectile` line
# for it. That is not a convention: C_DefineProjectile ends with
# `g_tile[j].flags |= SFLAG_PROJECTILE` (duke3d/src/gamedef.cpp:2324), and
# A_ShootWithZvel dispatches on exactly that flag -- A_ShootCustom, which flies
# the CON's workslike/vel, when it is set, and A_ShootHardcoded when it is not
# (player.cpp:1757-1759). So a tile absent from this table is a tile the
# engine fires through its HARDCODED path, where the stock speed table is the
# right answer and this one would be a wrong one. AA's WEAPONn_SHOOTS does
# name stock tiles that way -- RPG, SHOTSPARK1, CHAINGUN, SHOTGUN,
# SHRINKER, FREEZEBLAST, GROWSPARK, KNEE, HANDHOLDINGLASER: all NINE,
# counted from the scripts rather than recalled -- and none is declared
# here, so each falls back to the stock table.
#
# THE DEFAULTS ARE READ, NOT GUESSED, AND ONE OF THEM IS A TRAP.
# C_InitProjectiles (gamedef.cpp:6551-6570) initialises every tile's projectile
# to `workslike = 1, vel = 600`, and C_DefineProjectile copies that default in
# on the FIRST field a name declares. workslike 1 IS PROJECTILE_HITSCAN, so a
# declared projectile that never says PROJ_WORKSLIKE is HITSCAN -- not
# "unknown". A declared projectile that never says PROJ_VEL flies at 600.
#
# RUN BY HAND, NOT BY build.ps1: the build must not depend on the game being
# installed. Re-run after an AA update, from the repo root:
#
#   python tools/gen_con_proj_aa.py
#
# It overwrites src/ekmod/con_proj_aa.h. Do not hand-edit that file; fix this
# script and re-run it.

import argparse
import os
import re
import sys
from pathlib import Path

# gamedef.cpp:6564-6565 -- DefaultProjectile's first field is workslike and its
# ninth is vel. Both are quoted here rather than derived so that a reader can
# check them against the line above without running anything.
DEFAULT_WORKSLIKE = 1
DEFAULT_VEL = 600

# actors.h:368. Bit 0, and only bit 0, decides whether the shot arrives this
# tic. Every other bit in a workslike is about what the projectile DOES on the
# way or on impact, which a lead does not care about.
PROJECTILE_HITSCAN = 0x00000001

# MAXTILES (build.h). A picnum outside this is not a tile, and the engine
# itself rejects one: CON_DEFINEPROJECTILE reports ERROR_EXCEEDSMAXTILES and
# drops the line (gamedef.cpp:4453-4458).
MAXTILES = 30720


# ---------------------------------------------------------------------------
def strip_comments(text):
    """CON's two comment forms removed, line numbers preserved. A block comment
    becomes its own newlines so a reported line is still the real one -- the
    sibling generator learned that the hard way."""
    text = re.sub(r'/\*.*?\*/', lambda m: "\n" * m.group(0).count("\n"),
                  text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def read_lines(path):
    """(lineno, tokens) for every non-blank line, comments already gone."""
    text = strip_comments(path.read_text(encoding='latin-1'))
    for lineno, line in enumerate(text.splitlines(), 1):
        toks = line.split()
        if toks:
            yield lineno, toks


def include_order(root):
    """Every script the engine actually parses, in the order it parses them.

    THE ORDER IS PART OF THE ANSWER, not tidiness. CON_DEFINE IGNORES a
    redefinition and keeps the FIRST value (gamedef.cpp:2898-2906, which logs
    'ignored redefinition of'), so which file is read first decides what a name
    means. A plain glob of scripts/*.CON would be a different order AND a
    different set: this install has one orphan, 'SE90 temp.con', that nothing
    includes and the engine therefore never reads.

    Depth first, at the include statement, exactly as C_Include does."""
    out = []
    seen = set()

    def walk(rel, fromname, fromline):
        p = Path(rel)
        if not p.exists():
            raise SystemExit("%s:%d includes %s, which does not exist"
                             % (fromname, fromline, rel))
        key = str(p.resolve()).lower()
        if key in seen:
            # AA includes nothing twice today. If that ever changes, the second
            # pass cannot add a define (first wins) but it CAN re-run a
            # defineprojectile, so the file is walked again rather than skipped.
            pass
        seen.add(key)
        out.append(p)
        for lineno, toks in read_lines(p):
            if toks[0].lower() == 'include' and len(toks) >= 2:
                walk(toks[1], p.name, lineno)

    walk(root, "(root)", 0)
    return out


def parse_int(tok):
    """A CON literal, or None. `int(tok, 0)` would read 0x and 0b forms CON does
    not have and would choke on a leading 0 it does."""
    return int(tok) if re.match(r'^[+-]?\d+$', tok) else None


class Scan:
    def __init__(self):
        self.defines = {}        # name -> value, FIRST wins
        self.ignored_redef = []  # (file, line, name, new, kept)
        self.proj = {}           # name -> { field: (value, file, line) }
        self.repeated = []       # (name, field, kept_line, dropped_line)
        self.files = []

    def run(self, files):
        self.files = files
        for p in files:
            for lineno, toks in read_lines(p):
                head = toks[0].lower()
                if head == 'define' and len(toks) >= 3:
                    self.do_define(p, lineno, toks)
                elif head == 'defineprojectile' and len(toks) >= 4:
                    self.do_projectile(p, lineno, toks)

    def do_define(self, p, lineno, toks):
        name, val = toks[1], parse_int(toks[2])
        if val is None:
            return
        if name in self.defines:
            if self.defines[name] != val:
                self.ignored_redef.append((p.name, lineno, name, val,
                                           self.defines[name]))
            return
        self.defines[name] = val

    def do_projectile(self, p, lineno, toks):
        name, field, raw = toks[1], toks[2].upper(), toks[3]
        if field not in ('PROJ_WORKSLIKE', 'PROJ_VEL'):
            return
        # C_GetNextValue(LABEL_DEFINE) takes a literal or a define, so both are
        # resolved here the same way the engine resolves them.
        val = parse_int(raw)
        if val is None:
            val = self.defines.get(raw)
        if val is None:
            return
        row = self.proj.setdefault(name, {})
        if field in row:
            # LAST WINS, because C_DefineProjectile is a plain assignment into
            # one projectile_t (gamedef.cpp:2289/2295) and the file is read top
            # to bottom. The OPPOSITE of `define`, which keeps the first.
            self.repeated.append((name, field, lineno, row[field][2]))
        row[field] = (val, p.name, lineno)

    def rows(self):
        """(picnum, hitscan, vel, name), sorted by picnum, plus the refusals."""
        out, unresolved, oob, collide = [], [], [], []
        by_pic = {}
        for name in sorted(self.proj):
            pic = self.defines.get(name)
            if pic is None:
                pic = parse_int(name)
            if pic is None:
                unresolved.append(name)
                continue
            if not (0 <= pic < MAXTILES):
                oob.append((name, pic))
                continue
            fields = self.proj[name]
            works = fields.get('PROJ_WORKSLIKE', (DEFAULT_WORKSLIKE,))[0]
            vel = fields.get('PROJ_VEL', (DEFAULT_VEL,))[0]
            hit = 1 if (works & PROJECTILE_HITSCAN) else 0
            if pic in by_pic:
                collide.append((pic, by_pic[pic], name))
                continue
            by_pic[pic] = name
            out.append((pic, hit, vel, name, works,
                        'PROJ_VEL' in fields, 'PROJ_WORKSLIKE' in fields))
        out.sort(key=lambda r: r[0])
        return out, unresolved, oob, collide


# ---------------------------------------------------------------------------
PROLOGUE = """#pragma once

// Generated by tools/gen_con_proj_aa.py: do not hand-edit. Per picnum: does AA's projectile arrive this tic (HITSCAN,
// workslike bit 0) or how fast does it fly? A tile is in the table iff AA declares a `defineprojectile` for it
// (SFLAG_PROJECTILE); an absent picnum goes through the hardcoded path, where stock Duke's speed table is right.

"""

def render(scan, rows, unresolved, oob, collide, root, game_dir, date):
    stmt_count = sum(len(v) for v in scan.proj.values())
    hits = sum(1 for r in rows if r[1])
    nowork = sum(1 for r in rows if not r[6])
    novel = sum(1 for r in rows if not r[5])

    out = [PROLOGUE]

    out.append("""
const int EKM_CONPROJ_COUNT = %d;
const int EKM_CONPROJ_HITSCAN_COUNT = %d;
const int EKM_CONPROJ_FLYING_COUNT = %d;

const int EKM_CONPROJ_DEFAULT_WORKSLIKE = %d;
const int EKM_CONPROJ_DEFAULT_HITSCAN = %d;
const int EKM_CONPROJ_DEFAULT_VEL = %d;
const int EKM_CONPROJ_NO_WORKSLIKE_COUNT = %d;
const int EKM_CONPROJ_NO_VEL_COUNT = %d;
""" % (len(rows), hits, len(rows) - hits, DEFAULT_WORKSLIKE,
       1 if (DEFAULT_WORKSLIKE & PROJECTILE_HITSCAN) else 0, DEFAULT_VEL,
       nowork, novel))

    out.append("""
// One projectile's answer, returned by value; `found` false means no `defineprojectile`, so the caller falls back.
struct EkConProj { bool found; int picnum; int hitscan; int vel; const char* name; };

struct EkConProjRow { short picnum; short hitscan; short vel; const char* name; };

inline const EkConProjRow* ekm_conproj_rows(void) {
  static const EkConProjRow kRows[EKM_CONPROJ_COUNT] = {
""")

    for pic, hit, vel, name, works, _hv, _hw in rows:
        out.append('    { %5d, %d, %5d, "%s" },   // workslike %d\n'
                   % (pic, hit, vel, name, works))

    out.append("""  };
  return kRows;
}

// The i-th row, or a miss for an index outside the table -- the same shape a
// picnum miss returns, so a caller cannot tell the two apart by accident and
// then act on a half-filled struct.
inline EkConProj ekm_con_proj_at(int index) {
  EkConProj r;
  r.found = false;
  r.picnum = -1;
  r.hitscan = 0;
  r.vel = 0;
  r.name = "";
  if (index < 0 || index >= EKM_CONPROJ_COUNT) return r;
  const EkConProjRow* const row = ekm_conproj_rows() + index;
  r.found = true;
  r.picnum = (int)row->picnum;
  r.hitscan = (int)row->hitscan;
  r.vel = (int)row->vel;
  r.name = row->name;
  return r;
}

inline EkConProj ekm_con_proj_aa(int picnum) {
  const EkConProjRow* const rows = ekm_conproj_rows();
  int lo = 0, hi = EKM_CONPROJ_COUNT - 1;
  while (lo <= hi) {
    const int mid = lo + ((hi - lo) >> 1);
    const int at = (int)rows[mid].picnum;
    if (at == picnum) return ekm_con_proj_at(mid);
    if (at < picnum) lo = mid + 1;
    else             hi = mid - 1;
  }
  EkConProj r;
  r.found = false;
  r.picnum = -1;
  r.hitscan = 0;
  r.vel = 0;
  r.name = "";
  return r;
}
""")

    out.append("""
struct EkConProjFile { const char* name; long size; };
""")
    out.append("const int EKM_CONPROJ_FILE_COUNT = %d;\n\n"
               % len(scan.files))
    out.append("inline const EkConProjFile* ekm_conproj_files(void) {\n"
               "  static const EkConProjFile kFiles[EKM_CONPROJ_FILE_COUNT] = {\n")
    out.append(",\n".join('    { "%s", %d }' % (p.name, p.stat().st_size)
                          for p in scan.files))
    out.append("\n  };\n  return kFiles;\n}\n")

    out.append("\n\n\n")  # the committed header ends with three blank lines
    return "".join(out)


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-dir", default=os.environ.get("AA_GAME_DIR"),
                    required="AA_GAME_DIR" not in os.environ,
                    help="<AA game folder>, the AA install root (or set AA_GAME_DIR)")
    ap.add_argument("--script", default="scripts/AAGAME.CON",
                    help="the root CON, relative to --game-dir; AA's own "
                         "grpinfo names this one")
    ap.add_argument("--out", default="src/ekmod/con_proj_aa.h",
                    help="output header path, relative to the repo root")
    ap.add_argument("--date", default="2026-09-21")
    args = ap.parse_args()

    repo = Path.cwd()
    game_dir = Path(args.game_dir)
    root = game_dir / args.script
    if not root.exists():
        print("no root script at %s" % root, file=sys.stderr)
        return 1

    # The include paths in the CON are relative to the game folder, so the walk
    # runs with that as the working directory and the output path is resolved
    # against the repo root first.
    out_path = (repo / args.out).resolve()
    os.chdir(str(game_dir))
    files = include_order(args.script)

    scan = Scan()
    scan.run(files)
    rows, unresolved, oob, collide = scan.rows()

    stmt_count = sum(len(v) for v in scan.proj.values())
    hits = sum(1 for r in rows if r[1])
    print("script files (include order): %d" % len(files))
    print("define names resolved:        %d" % len(scan.defines))
    print("define redefs ignored:        %d" % len(scan.ignored_redef))
    print("defineprojectile statements:  %d  (WORKSLIKE/VEL only)" % stmt_count)
    print("distinct projectile names:    %d" % len(scan.proj))
    print("rows emitted:                 %d  (hitscan %d, flying %d)"
          % (len(rows), hits, len(rows) - hits))
    print("workslike defaulted to 1:     %d" % sum(1 for r in rows if not r[6]))
    print("vel defaulted to 600:         %d" % sum(1 for r in rows if not r[5]))
    print("repeated field declarations:  %d" % len(scan.repeated))
    print("dropped (unresolved/oob/dup): %d / %d / %d"
          % (len(unresolved), len(oob), len(collide)))

    # A COUNT AND AN ENUMERATION OF THE SAME THING MUST AGREE. The
    # names that produced no row are exactly the ones this refuses to round off.
    accounted = len(rows) + len(unresolved) + len(oob) + len(collide)
    if accounted != len(scan.proj):
        print("names %d but rows+dropped %d -- nothing written"
              % (len(scan.proj), accounted), file=sys.stderr)
        return 1

    text = render(scan, rows, unresolved, oob, collide, args.script,
                  game_dir, args.date)
    # CRLF explicitly: every source file in this repo is CRLF and a plain
    # text-mode write emits LF, which rewrites the whole file's endings.
    with open(str(out_path), "w", encoding="utf-8", newline="") as fh:
        fh.write(text.replace("\n", "\r\n"))
    print("wrote %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
