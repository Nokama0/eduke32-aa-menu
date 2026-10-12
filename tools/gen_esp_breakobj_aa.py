#!/usr/bin/env python3
# Generates src/ekmod/esp_breakobj_aa.h -- every picnum whose actor runs
# `state breakobject`, plus a fingerprint of the files it was read from.
#
# WHY THIS EXISTS (AA12). The user asked for an ESP row over the breakables
# that REVEAL CASH when destroyed, and explicitly not over every breakable:
# "making sure that only those that reveal cash items are shown on this new ESP
# row, not every breakable object, since that's already a thing."
#
# The discriminator is AA's own PERACTOR gamevar `droptile`
# (`gamevar droptile 0 2`, AADEFS.CON:3980), which `AAPRELOAD.CON:1556-1568`
# fills at MAP LOAD from the map sprite's xvel (or its lotag when that exceeds
# 5) and which `state breakobject` (INTERACT.CON:1399) spawns on death:
#
#     ifhitweapon { ifdead { ifn droptile 0 { ifg droptile 10 espawnvar droptile
#
# So the runtime test is "droptile is one of the two cash stacks". THIS TABLE IS
# NOT THAT TEST. It is the cheap PRE-FILTER in front of it: a gamevar read is a
# bounds-checked walk through engine memory, and doing one for all 16384 sprite
# slots every frame is not a cost this row should carry. A picnum in this table
# is a sprite that COULD carry a meaningful droptile, because its actor is one
# of the ones that reads the var; the gamevar decides whether it does.
#
# MEMBERSHIP IS THE CON'S OWN CALL GRAPH. A picnum is in this table if and only
# if its actor block reaches `state breakobject`, directly or through a state it
# calls. The closure is not optional: AA writes plenty of these as
# `useractor notenemy TABLET1 0 state flashercode state breakobject enda`, and
# some as multi-line blocks whose call sits several lines in.
#
# LAST DECLARATION WINS, WHICH IS THE ENGINE'S RULE AND NOT A CONVENIENCE.
# CON_ACTOR (gamedef.cpp:3195-3263) silently overwrites execPtr for an actor
# declared twice -- there is no ERROR_DUPLICATEDEFINITION -- so the block that
# runs is the LAST one parsed. gen_esp_items_aa.py keeps the first instead and
# argues that the two agree on its own rows; this generator follows the engine
# and reports what the other choice would have changed, so the two cannot
# disagree silently.
#
# RUN BY HAND, NOT BY build.ps1: the build must not depend on the game being
# installed. Re-run after an AA update, from the repo root:
#
#   python tools/gen_esp_breakobj_aa.py
#
# It overwrites src/ekmod/esp_breakobj_aa.h. Do not hand-edit that file; fix
# this script and re-run it.

import argparse
import os
import re
import sys
import textwrap
from pathlib import Path

# MAXTILES (build.h). A picnum outside this is not a tile.
MAXTILES = 30720

# The state whose call decides membership.
BREAK_STATE = "breakobject"

# Block openers and the keyword that closes each. `state` at top level is a
# DECLARATION and inside a block it is a CALL -- gamedef.cpp:2586-2590 branches
# on exactly that -- so this walker does too.
ACTOR_OPENERS = ("actor", "useractor", "eventloadactor")
EVENT_OPENERS = ("onevent", "appendevent", "prependevent")
STATE_OPENERS = ("state", "defstate")


# ---------------------------------------------------------------------------
def strip_comments(text):
    """CON's two comment forms removed, line numbers preserved. A block comment
    becomes its own newlines so a reported line is still the real one."""
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

    THE ORDER IS PART OF THE ANSWER. CON_DEFINE IGNORES a redefinition and keeps
    the FIRST value (gamedef.cpp:2898-2906), so which file is read first decides
    what a name means. A plain glob would be a different order AND a different
    set: this install has one orphan, 'SE90 temp.con', that nothing includes and
    the engine therefore never reads. Identical to gen_con_proj_aa.py's walk."""
    out = []

    def walk(rel, fromname, fromline):
        p = Path(rel)
        if not p.exists():
            raise SystemExit("%s:%d includes %s, which does not exist"
                             % (fromname, fromline, rel))
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


def parse_names_h(path):
    """`#define NAME picnum` -> {NAME: picnum}. The canonical tile list, and the
    same file tilenames.cpp parses at runtime."""
    names = {}
    for line in path.read_text(encoding='latin-1').splitlines():
        m = re.match(r'#define\s+(\S+)\s+(-?\d+)\s*$', line.strip())
        if m:
            names[m.group(1)] = int(m.group(2))
    return names


class Block:
    __slots__ = ("name", "kind", "file", "line", "calls")

    def __init__(self, name, kind, file, line):
        self.name = name
        self.kind = kind
        self.file = file
        self.line = line
        self.calls = set()


class Scan:
    def __init__(self):
        self.defines = {}        # name -> value, FIRST wins
        self.ignored_redef = []  # (file, line, name, new, kept)
        # TWO NAMESPACES, NOT ONE, AND THE ENGINE HAS TWO SLOTS. `actor` /
        # `useractor` fill g_tile[].execPtr and `eventloadactor` fills a
        # different pointer that runs once at load; a tile may have BOTH, and AA
        # has two that do -- CLOCK (INTERACT.CON:2086/2088) and MILKPLUSSTATUE
        # (:2396/2397). A first version of this walker kept one dict for both,
        # and the eventloadactor block hid the useractor block that actually
        # calls `state breakobject`, silently dropping two rows under a
        # first-wins reading. Keyed apart, membership is the union.
        self.actors = {}         # name -> Block, LAST wins (the engine's rule)
        self.first_actors = {}   # name -> Block, FIRST wins, for the A/B below
        self.loads = {}          # name -> eventloadactor Block, LAST wins
        self.first_loads = {}    # name -> eventloadactor Block, FIRST wins
        self.states = {}         # name -> Block
        self.dup_actors = []     # (name, first_file, first_line, file, line)
        self.dup_loads = []      # the same, for eventloadactor blocks
        # EVERY CALL SITE, NOT ONLY THE NAMES THEY BELONG TO. A count and an
        # enumeration of the same thing that disagree are what catch a broken
        # instrument, and these two legitimately differ by one: AA
        # declares DANCINGPANDA twice and both blocks call the state.
        self.break_sites = []    # (file, line, block kind, block name)
        self.stray_end = []      # (file, line, token, open_kind, open_name)
        self.unclosed = []       # (file, line, kind, name)
        self.files = []

    def run(self, files):
        self.files = files
        for p in files:
            for lineno, toks in read_lines(p):
                if toks[0].lower() == 'define' and len(toks) >= 3:
                    self.do_define(p, lineno, toks)
        for p in files:
            self.walk_blocks(p)

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

    def walk_blocks(self, p):
        cur = None
        for lineno, toks in read_lines(p):
            low = [t.lower() for t in toks]
            i = 0
            n = len(toks)
            while i < n:
                t = low[i]
                if cur is None:
                    if t in ACTOR_OPENERS and i + 1 < n:
                        # `useractor` takes a TYPE before the tile name; the
                        # other two do not.
                        j = i + 2 if t == 'useractor' else i + 1
                        if j >= n:
                            i = n
                            continue
                        kind = 'load' if t == 'eventloadactor' else 'actor'
                        cur = Block(toks[j], kind, p.name, lineno)
                        i = j + 1
                        continue
                    if t in EVENT_OPENERS and i + 1 < n:
                        cur = Block(toks[i + 1], 'event', p.name, lineno)
                        i += 2
                        continue
                    if t in STATE_OPENERS and i + 1 < n:
                        cur = Block(toks[i + 1], 'state', p.name, lineno)
                        i += 2
                        continue
                    i += 1
                    continue

                # inside a block
                if t == 'enda' and cur.kind in ('actor', 'load'):
                    # eventloadactor RUNS at load and is a real actor block, so
                    # a breakobject call inside one counts exactly as one in an
                    # `actor` block does. AA has none today; the walker does not
                    # depend on that staying true, and it keeps the two kinds in
                    # separate namespaces so neither can hide the other.
                    self.close_actor(cur)
                    cur = None
                    i += 1
                    continue
                if t == 'ends' and cur.kind == 'state':
                    self.states[cur.name] = cur
                    cur = None
                    i += 1
                    continue
                if t == 'endevent' and cur.kind == 'event':
                    cur = None
                    i += 1
                    continue
                if t in ('enda', 'ends', 'endevent'):
                    # A close that does not match what is open means this
                    # walker's idea of the block structure is wrong, and every
                    # membership answer after it would be nonsense. Recorded
                    # rather than swallowed; main() refuses to write on it.
                    self.stray_end.append((p.name, lineno, t, cur.kind, cur.name))
                    cur = None
                    i += 1
                    continue
                if t == 'state' and i + 1 < n:
                    cur.calls.add(toks[i + 1])
                    if toks[i + 1] == BREAK_STATE:
                        self.break_sites.append((p.name, lineno, cur.kind,
                                                 cur.name))
                    i += 2
                    continue
                i += 1
            # `prev_last` tracking is gen_esp_items_aa.py's, for quote arms; no
            # rule here reads across lines, so nothing is carried.
        if cur is not None:
            self.unclosed.append((p.name, cur.line, cur.kind, cur.name))

    def close_actor(self, blk):
        load = blk.kind == 'load'
        last = self.loads if load else self.actors
        first = self.first_loads if load else self.first_actors
        dups = self.dup_loads if load else self.dup_actors
        if blk.name not in first:
            first[blk.name] = blk
        prev = last.get(blk.name)
        if prev is not None:
            dups.append((blk.name, prev.file, prev.line, blk.file, blk.line))
        # LAST WINS: CON_ACTOR overwrites execPtr (gamedef.cpp:3195-3263), so
        # the block the engine runs is the last one parsed.
        last[blk.name] = blk

    def reaches(self, blk, want):
        """Does this block call `want`, directly or through a state it calls?

        A state that is called but never declared is simply absent -- the engine
        would report that as an error of its own, and inventing an answer for it
        here would be worse than having none."""
        if want in blk.calls:
            return True
        seen = set()
        stack = list(blk.calls)
        while stack:
            nm = stack.pop()
            if nm in seen:
                continue
            seen.add(nm)
            s = self.states.get(nm)
            if s is None:
                continue
            if want in s.calls:
                return True
            stack.extend(s.calls)
        return False


def resolve(name, names_h, defines):
    """(picnum, source). names.h is PRIMARY -- it is the canonical tile list and
    the file tilenames.cpp already parses. A CON `define` is consulted only for
    a name names.h does not carry at all. A literal-named actor resolves to
    itself. The same order, and the same three sources, gen_esp_items_aa.py's
    own `resolve()` uses."""
    if name in names_h:
        return names_h[name], "names.h"
    if name in defines:
        return defines[name], "CON define"
    lit = parse_int(name)
    if lit is not None:
        return lit, "literal"
    return None, None


# ---------------------------------------------------------------------------
PROLOGUE = """#pragma once

// GENERATED BY tools/gen_esp_breakobj_aa.py -- DO NOT HAND-EDIT; a frozen reading of one AA install. Picnums whose
// actor runs `state breakobject` (INTERACT.CON:1399), transitively: a PRE-FILTER only, since the gamevar `droptile`
// (AADEFS.CON:3980) says whether a sprite pays.

"""

COUNTS = """// Walk: %(file_count)d script files, %(actor_count)d actors (%(dup_count)d duplicate declarations; the last wins, as in the engine). %(found_count)d actors reach
// breakobject = %(row_count)d rows + %(unresolved_count)d unresolved + %(oob_count)d out of range + %(collide_count)d on a taken picnum; the generator refuses to write unless
// they sum.
"""


def render(scan, rows, unresolved, oob, collide, found, src_tally, ab_delta,
           root, game_dir, date):
    out = [PROLOGUE % {'game_dir': str(game_dir).replace("\\", "/"),
                       'date': date}]
    out.append(COUNTS % {
        'root': root,
        'file_count': len(scan.files),
        'define_count': len(scan.defines),
        'redef_count': len(scan.ignored_redef),
        'actor_count': len(scan.actors),
        'dup_count': len(scan.dup_actors),
        'load_count': len(scan.loads),
        'dup_load_count': len(scan.dup_loads),
        'state_count': len(scan.states),
        'break_state': BREAK_STATE,
        'site_count': len(scan.break_sites),
        'site_actor_count': sum(1 for s in scan.break_sites
                                if s[2] in ('actor', 'load')),
        'site_state_count': sum(1 for s in scan.break_sites if s[2] == 'state'),
        'found_count': len(found),
        'row_count': len(rows),
        'unresolved_count': len(unresolved),
        'oob_count': len(oob),
        'collide_count': len(collide),
        'from_names_h': src_tally["names.h"],
        'from_con': src_tally["CON define"],
        'from_literal': src_tally["literal"],
        'ab_delta': ab_delta,
    })

    out.append("""
// Emitted, not counted at runtime, so the suite can check the count against the enumeration.
const int EKM_BREAKOBJ_COUNT = %d;

// `name` is the CON's symbol for the tile, never null; for diagnostics only. The ESP label for this category is fixed
// (\"Cash inside\"), see esp_cash_aa.h.
struct EkBreakObjRow { short picnum; const char* name; };

// Sorted by picnum. Function-local so including translation units share one copy.
inline const EkBreakObjRow* ekm_breakobj_rows(void) {
  static const EkBreakObjRow kRows[EKM_BREAKOBJ_COUNT] = {
""" % len(rows))

    width = max(len(name) for _pic, name, _src in rows)
    for pic, name, src in rows:
        out.append('    { %5d, "%s"%s },\n'
                   % (pic, name, " " * (width - len(name))))

    out.append("""  };
  return kRows;
}

inline const char* ekm_breakobj_symbol_at(int index) {
  if (index < 0 || index >= EKM_BREAKOBJ_COUNT) return "";
  return ekm_breakobj_rows()[index].name;
}

inline int ekm_breakobj_picnum_at(int index) {
  if (index < 0 || index >= EKM_BREAKOBJ_COUNT) return -1;
  return (int)ekm_breakobj_rows()[index].picnum;
}

// Binary search over the sorted table: picnums are sparse (%d to %d), so a tile-indexed array would be 30720
// entries. Null means the actor does not run `state %s`: the pre-filter working, not a gap.
inline const char* ekm_breakobj_name(int picnum) {
  const EkBreakObjRow* const rows = ekm_breakobj_rows();
  int lo = 0, hi = EKM_BREAKOBJ_COUNT - 1;
  while (lo <= hi) {
    const int mid = lo + ((hi - lo) >> 1);
    const int at = (int)rows[mid].picnum;
    if (at == picnum) return rows[mid].name;
    if (at < picnum) lo = mid + 1;
    else             hi = mid - 1;
  }
  return 0;
}

inline bool ekm_breakobj_is_member(int picnum) {
  return ekm_breakobj_name(picnum) != 0;
}
""" % (rows[0][0], rows[-1][0], BREAK_STATE))

    out.append("""
// Files this table was read from, in the engine's include order, with sizes. `size` is a long to match con_stops_aa.h's
// EkConScriptFile.
struct EkBreakObjFile { const char* name; long size; };
""")
    out.append("const int EKM_BREAKOBJ_FILE_COUNT = %d;\n\n" % len(scan.files))
    out.append("inline const EkBreakObjFile* ekm_breakobj_files(void) {\n"
               "  static const EkBreakObjFile kFiles[EKM_BREAKOBJ_FILE_COUNT] = {\n")
    out.append(",\n".join('    { "%s", %d }' % (p.name, p.stat().st_size)
                          for p in scan.files))
    out.append("\n  };\n  return kFiles;\n}\n")

    # Refusals and oddities, as wrapped paragraphs: each is a thing a future AA update could turn from zero into a
    # number, so it is printed only when it is not zero.
    drops = []
    for name, fn, ln in unresolved:
        drops.append("%s (%s:%d) -- no names.h entry, no `define`, not a literal" % (name, fn, ln))
    for name, pic, fn, ln in oob:
        drops.append("%s (%s:%d) -- picnum %d is outside 0..%d" % (name, fn, ln, pic, MAXTILES - 1))
    for pic, first, second, fn, ln in collide:
        drops.append("%s (%s:%d) -- picnum %d already taken by %s" % (second, fn, ln, pic, first))
    if drops:
        out.append("\n" + textwrap.fill(
            "// DROPPED rows (unresolvable symbol, picnum outside 0..MAXTILES, or two actors on one picnum): "
            + "; ".join(drops), width=120, subsequent_indent="// ", break_long_words=False,
            break_on_hyphens=False) + "\n")

    # Scoped to this table's own names; the total is printed beside the list so a reader can see it is a subset.
    dupHere = [d for d in scan.dup_actors if d[0] in found]
    dupLoadHere = [d for d in scan.dup_loads if d[0] in found]
    dups = ["%s -- %s:%d is overwritten by %s:%d" % (name, kf, kl, f, l) for name, kf, kl, f, l in dupHere]
    dups += ["%s (eventloadactor) -- %s:%d is overwritten by %s:%d" % (name, kf, kl, f, l)
             for name, kf, kl, f, l in dupLoadHere]
    out.append("\n" + textwrap.fill(
        "// DUPLICATE DECLARATIONS AMONG THE ROWS ABOVE (the engine runs the LAST, and so does this table): "
        "%d of %d `actor`/`useractor` and %d of %d `eventloadactor` duplicates in the whole CON."
        % (len(dupHere), len(scan.dup_actors), len(dupLoadHere), len(scan.dup_loads))
        + "".join(" " + d + ";" for d in dups)[:-1] if dups else "",
        width=120, subsequent_indent="// ", break_long_words=False, break_on_hyphens=False) + "\n")

    if scan.ignored_redef:
        out.append("\n// `define` redefinitions the engine ignored (first value kept):\n")
        for fn, ln, name, new, kept in scan.ignored_redef:
            out.append("//   %s:%d  %s -> %d ignored, %d kept\n" % (fn, ln, name, new, kept))

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
    ap.add_argument("--names", default="names.h",
                    help="the tile name header, relative to --game-dir")
    ap.add_argument("--out", default="src/ekmod/esp_breakobj_aa.h",
                    help="output header path, relative to the repo root")
    ap.add_argument("--date", default="2026-09-21")
    args = ap.parse_args()

    repo = Path.cwd()
    game_dir = Path(args.game_dir)
    root = game_dir / args.script
    names_path = game_dir / args.names
    if not root.exists():
        print("no root script at %s" % root, file=sys.stderr)
        return 1
    if not names_path.exists():
        print("no names header at %s" % names_path, file=sys.stderr)
        return 1

    names_h = parse_names_h(names_path)

    # The include paths in the CON are relative to the game folder, so the walk
    # runs with that as the working directory and the output path is resolved
    # against the repo root first.
    out_path = (repo / args.out).resolve()
    os.chdir(str(game_dir))
    files = include_order(args.script)

    scan = Scan()
    scan.run(files)

    if scan.stray_end or scan.unclosed:
        # A BROKEN BLOCK STRUCTURE POISONS EVERY ANSWER, so nothing is written.
        for fn, ln, t, kind, name in scan.stray_end:
            print("%s:%d  `%s` closes a %s block (%s)" % (fn, ln, t, kind, name),
                  file=sys.stderr)
        for fn, ln, kind, name in scan.unclosed:
            print("%s:%d  %s block %s is never closed" % (fn, ln, kind, name),
                  file=sys.stderr)
        print("block structure is not understood -- nothing written",
              file=sys.stderr)
        return 1

    # THE UNION OF THE TWO NAMESPACES. A tile is a breakable if the block the
    # engine runs per tic reaches the state, or if its load-time block does --
    # they are different pointers and either may carry the call.
    def reaching(actors, loads):
        out = set()
        for name in set(actors) | set(loads):
            for blk in (actors.get(name), loads.get(name)):
                if blk is not None and scan.reaches(blk, BREAK_STATE):
                    out.add(name)
                    break
        return out

    foundSet = reaching(scan.actors, scan.loads)
    found = sorted(foundSet)
    firstFound = reaching(scan.first_actors, scan.first_loads)
    # WHAT THIS DELTA IS, AND WHAT IT IS NOT. It compares first-wins against
    # last-wins WITHIN the already-separated actor and load namespaces, so a
    # zero here does NOT re-prove the namespace fix described above -- that
    # one is proved by CLOCK and MILKPLUSSTATUE being in the emitted table at
    # all. Reported together the two read as one claim and they are two.
    # (review-aa12, 2026-09-21)
    ab_delta = len(foundSet ^ firstFound)

    rows, unresolved, oob, collide = [], [], [], []
    src_tally = {"names.h": 0, "CON define": 0, "literal": 0}
    by_pic = {}
    for name in found:
        # The per-tic block is what a reader goes looking for; a name that only
        # has a load block cites that one instead.
        blk = scan.actors.get(name) or scan.loads[name]
        pic, src = resolve(name, names_h, scan.defines)
        if pic is None:
            unresolved.append((name, blk.file, blk.line))
            continue
        if not (0 <= pic < MAXTILES):
            oob.append((name, pic, blk.file, blk.line))
            continue
        if pic in by_pic:
            collide.append((pic, by_pic[pic], name, blk.file, blk.line))
            continue
        by_pic[pic] = name
        src_tally[src] += 1
        rows.append((pic, name, "%s, %s:%d" % (src, blk.file, blk.line)))
    rows.sort(key=lambda r: r[0])

    print("script files (include order): %d" % len(files))
    print("define names resolved:        %d" % len(scan.defines))
    print("actor/useractor names:        %d  (%d duplicate declaration(s))"
          % (len(scan.actors), len(scan.dup_actors)))
    print("eventloadactor names:         %d  (%d duplicate declaration(s))"
          % (len(scan.loads), len(scan.dup_loads)))
    print("state blocks:                 %d" % len(scan.states))
    print("`state %s` call sites:  %d  (%d in actor blocks, %d in states)"
          % (BREAK_STATE, len(scan.break_sites),
             sum(1 for s in scan.break_sites if s[2] in ('actor', 'load')),
             sum(1 for s in scan.break_sites if s[2] == 'state')))
    print("actors reaching `state %s`: %d" % (BREAK_STATE, len(found)))
    print("  picnums resolved (rows):    %d" % len(rows))
    print("  dropped unresolved/oob/dup: %d / %d / %d"
          % (len(unresolved), len(oob), len(collide)))
    print("name sources (names.h/CON/literal): %d / %d / %d"
          % (src_tally["names.h"], src_tally["CON define"],
             src_tally["literal"]))
    print("first-wins A/B differs by:    %d actor(s)" % ab_delta)

    # A COUNT AND AN ENUMERATION OF THE SAME THING MUST AGREE. The
    # actors that produced no row are exactly the ones this refuses to round off.
    accounted = len(rows) + len(unresolved) + len(oob) + len(collide)
    if accounted != len(found):
        print("actors reaching the state %d but rows+dropped %d -- nothing "
              "written" % (len(found), accounted), file=sys.stderr)
        return 1
    if not rows:
        print("no rows -- nothing written", file=sys.stderr)
        return 1

    text = render(scan, rows, unresolved, oob, collide, found, src_tally,
                  ab_delta, args.script, game_dir, args.date)
    # CRLF explicitly: every source file in this repo is CRLF and a plain
    # text-mode write emits LF, which rewrites the whole file's endings.
    with open(str(out_path), "w", encoding="utf-8", newline="") as fh:
        fh.write(text.replace("\n", "\r\n"))
    print("wrote %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
