#!/usr/bin/env python3
# Generates src/ekmod/esp_voxels_aa.h -- picnum -> the VOXEL Alien Armageddon
# draws on that tile, with the extents read out of the .kvx itself, plus a
# fingerprint of every file the walk read.
#
# WHY THIS EXISTS (D9, cause 2). The item ESP sizes its box from `tilesiz`,
# which describes a FLAT tile. AA draws most of its pickups as VOXELS, so that
# box measures a surface the player never sees -- the same hazard the HRP port's
# top section states for the HRP's 3D models, one renderer down. The user's own
# reading is what identified it: 5329 RAILGUNSPRITE (voxel) drew a box smaller
# than the weapon, 40 (voxel) was wrong, and 12247 SMALLSTACK (no voxel) was
# right.
#
# WHAT THE ENGINE DOES, AND WHY THIS TABLE IS SHAPED THE WAY IT IS.
# defs.cpp:1939-2044 is the whole `voxel` token: it loads the .kvx FIRST
# (qloadkvx, :1973) and only then reads the block's `tile`/`tile0`+`tile1`
# claims, writing `tiletovox[tile] = lastvoxid` for each one (:2001, :2019).
# A block whose .kvx will not load claims NOTHING -- the tile keeps its flat
# tile -- so a missing file is a real outcome and is counted below, never
# silently skipped. `scale` (:2027) multiplies the model in all three axes.
#
# THE LAST CLAIM WINS, NOT THE FIRST. `tiletovox[tilex] = lastvoxid` is a plain
# store with no "already set" test, so when two blocks claim one picnum the
# engine draws the SECOND one. This generator does the same and enumerates
# every such overwrite. (The item generator beside this one keeps the FIRST for
# its own collisions, and that is right there: nothing in the CON resolves two
# actors on one picnum, so it drops the row. Here the engine's answer is not
# ambiguous -- it is written down -- and matching it is the whole job.)
#
# WHAT IS MEASURED OUT OF EACH .kvx. voxmodel.cpp:784-805 is the loader, and it
# is the authority for the header layout:
#
#     int32  mip1leng          offset 0
#     int32  xsiz, ysiz, zsiz  offset 4   (voxsiz, :791)
#     int32  xpiv, ypiv, zpiv  offset 16  (voxpiv, /256.0 -- :802-804)
#
# and then `klseek(fil, (7*sizeof(int32_t)) + ...)` at :830 states the header
# is exactly seven int32s. read_pal (:685-687) seeks -768 from the END, so the
# file carries a trailing 256*3 palette.
#
# BOTH ARE CHECKED AGAINST EVERY FILE, and the check is the whole mip chain,
# not one header. A .kvx holds successive mips, each one [numbytes][header]
# [data], so walking `off += 4 + numbytes` from zero must land EXACTLY on
# filesize-768 -- the palette. That identity cannot survive a wrong field
# order or a wrong header size, which is what makes it evidence rather than a
# plausible reading. It held for all 612 .kvx files in this install on the
# first run; `filesize == 4 + mip1leng + 768` held for only 7, which is what
# a mip chain looks like and is why the weaker identity was replaced.
# vox2poly (:474-520) walks x in 0..xsiz inclusive, so the model occupies
# [0, siz] in each axis and the pivot is a point inside it.
#
# THE RADIUS, AND WHY IT IS NOT A WIDTH. md3_vox_calcmat_common
# (mdsprite.cpp:1869-1880) rotates the voxel's x and y axes by the sprite's own
# `ang` about the pivot, so the extent facing the camera changes as the player
# walks around it. A screen-aligned box has no angle, so the only half-width
# that never clips the item is the bounding-cylinder radius about that pivot:
# hypot(max(xpiv, xsiz-xpiv), max(ypiv, ysiz-ypiv)).
#
# AND THE COST IS NOT sqrt(2). That figure was written here first and it is
# wrong by a lot: sqrt(2) is the worst case only for a SQUARE footprint. The
# real over-width against the narrow face is sizX/sizY, and these voxels are
# long and thin -- 5329_railgunstriker is a 79x9 grid, so 8.8x; the median
# across the 51 voxel-backed item rows is 2.83x; picnum 60's 48x2 keycard is
# 24x. At edge-on yaws this box is about TWICE the width of the flat-tile box
# it replaces, so a reader who believed sqrt(2) would call a correct
# implementation broken. Broadside is exact and it never clips the item,
# which is why it shipped first. (corrected by review, 2026-09-21)
#
# AND THE YAW-AWARE RULE NOW SHIPS. projmath.h's ekm_voxel_width_model uses
# a*|cos t| + b*|sin t| with t = sprang - camang, and this radius became its
# CEILING and its fallback rather than its answer. It recovers a and b from
# sizX/sizY and the radius because THIS GENERATOR STILL DOES NOT EMIT
# xpiv/ypiv: exact for the 453 rows whose x/y pivot is centred, an upper bound
# -- never a clip, at worst 2x -- on the other 155. Emitting those two pivots
# is the one change that would make it exact everywhere, and it is the reason
# to open this file again. (2026-09-21)
#
# THE FINGERPRINT IS NOT DECORATION. This is a frozen reading of one install.
# Every .def the walk read is recorded with its size, and the .kvx files are
# recorded as a count and a total byte size -- size, never mtime, because mtime
# changes on a copy.

import argparse
import math
import os
import struct
import sys
import textwrap
from pathlib import Path

MAXTILES = 30720
MAXUSERTILES = MAXTILES - 256   # build.h:95 -- the engine reserves the last 256
MAXVOXELS = 2048                # build.h:97
KVX_HEADER_INTS = 7             # voxmodel.cpp:830
KVX_PAL_BYTES = 768             # voxmodel.cpp:685-687

# ------------------------------------------------------------------ tokenizer


class Tok(object):
    """One token, with the file and line it came from so an error can name it."""
    __slots__ = ("text", "quoted", "file", "line")

    def __init__(self, text, quoted, file, line):
        self.text = text
        self.quoted = quoted
        self.file = file
        self.line = line

    def __repr__(self):
        return "%s:%d:%s" % (self.file, self.line, self.text)


def tokenize(text, fname):
    """EDuke32's scriptfile tokens: whitespace separated, `//` to end of line,
    `/* */` nesting-free block comments, "quoted strings", and braces as tokens
    of their own so `{scale 1}` and `{ scale 1 }` read alike."""
    out = []
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c.isspace():
            i += 1
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0:
                line += text.count("\n", i)
                i = n
            else:
                line += text.count("\n", i, j)
                i = j + 2
            continue
        if c == '"':
            j = text.find('"', i + 1)
            if j < 0:
                out.append(Tok(text[i + 1:], True, fname, line))
                i = n
                continue
            out.append(Tok(text[i + 1:j], True, fname, line))
            line += text.count("\n", i, j)
            i = j + 1
            continue
        if c in "{}":
            out.append(Tok(c, False, fname, line))
            i += 1
            continue
        j = i
        while j < n and not text[j].isspace() and text[j] not in '{}"':
            j += 1
        out.append(Tok(text[i:j], False, fname, line))
        i = j
    return out


def read_text(path):
    with open(str(path), "rb") as fh:
        return fh.read().decode("latin-1")


def include_order(root_rel, default_rel, seen=None, order=None, missing=None):
    """The .def files the engine parses, in include order. Relative to the CWD,
    which main() sets to the game folder, exactly as the engine resolves them."""
    if seen is None:
        seen, order, missing = set(), [], []
    key = root_rel.replace("\\", "/").lower()
    if key in seen:
        return order, missing
    seen.add(key)
    if not os.path.exists(root_rel):
        missing.append(root_rel)
        return order, missing
    order.append(root_rel)
    toks = tokenize(read_text(root_rel), root_rel)
    i = 0
    while i < len(toks):
        t = toks[i].text.lower()
        if t in ("include", "#include") and i + 1 < len(toks):
            include_order(toks[i + 1].text, default_rel, seen, order, missing)
            i += 2
            continue
        if t in ("includedefault", "#includedefault"):
            include_order(default_rel, default_rel, seen, order, missing)
            i += 1
            continue
        i += 1
    return order, missing


# ------------------------------------------------------------------ kvx header


class Kvx(object):
    __slots__ = ("path", "size", "sx", "sy", "sz", "px", "py", "pz",
                 "mips", "chain_ok")


def read_kvx(path):
    """Mip 1's seven-int32 header (voxmodel.cpp:790-804), plus a walk of the
    whole mip chain purely to prove that header layout is the real one."""
    size = os.path.getsize(str(path))
    if size < KVX_HEADER_INTS * 4 + KVX_PAL_BYTES:
        return None
    with open(str(path), "rb") as fh:
        blob = fh.read()
    mip1leng, sx, sy, sz, px, py, pz = struct.unpack_from("<7i", blob, 0)
    if not (0 < sx <= 4096 and 0 < sy <= 4096 and 0 < sz <= 4096):
        return None
    k = Kvx()
    k.path = str(path).replace("\\", "/")
    k.size = size
    k.sx, k.sy, k.sz = sx, sy, sz
    # The pivot is 8.8 fixed point: voxmodel.cpp:802-804 divides by 256.
    k.px, k.py, k.pz = px / 256.0, py / 256.0, pz / 256.0

    # THE LAYOUT PROOF. Each mip is [int32 numbytes][the numbytes], so stepping
    # 4+numbytes from zero must land exactly on the 768-byte palette at the end.
    off, mips = 0, 0
    end = size - KVX_PAL_BYTES
    while 0 <= off < end and mips < 16:
        if off + 4 > end:
            break
        n = struct.unpack_from("<i", blob, off)[0]
        if n < KVX_HEADER_INTS * 4 - 4:
            break
        off += 4 + n
        mips += 1
    k.mips = mips
    k.chain_ok = (off == end)
    return k


def radius_of(k):
    """Bounding-cylinder radius about the pivot, in model units."""
    dx = max(k.px, float(k.sx) - k.px)
    dy = max(k.py, float(k.sy) - k.py)
    return math.hypot(dx, dy)


# ------------------------------------------------------------------ def walk


class Block(object):
    __slots__ = ("fname", "line", "rel", "kvx", "scale", "claims", "notrans")


class Walk(object):
    def __init__(self):
        self.blocks = []            # every `voxel` block the engine would reach
        self.load_failed = []       # (file, line, path) -- claims nothing
        self.over_max = []          # (file, line, path) -- MAXVOXELS reached
        self.bad_tile = []          # (file, line, text) -- out of range / unparsable
        self.claims = 0             # tiletovox[] stores the engine would make
        self.overwrites = []        # (picnum, previous rel, new rel)
        self.tile_to_vox = {}       # picnum -> block index (LAST claim wins)

    def run(self, files, game_dir):
        nextvoxid = 0
        for fname in files:
            toks = tokenize(read_text(fname), fname)
            i = 0
            while i < len(toks):
                if toks[i].text.lower() != "voxel":
                    i += 1
                    continue
                at = toks[i]
                i += 1
                if i >= len(toks):
                    break
                rel = toks[i].text
                i += 1
                if i >= len(toks) or toks[i].text != "{":
                    continue
                depth, j = 0, i
                while j < len(toks):
                    if toks[j].text == "{":
                        depth += 1
                    elif toks[j].text == "}":
                        depth -= 1
                        if depth == 0:
                            break
                    j += 1
                body, i = toks[i + 1:j], j + 1

                if nextvoxid >= MAXVOXELS:
                    self.over_max.append((at.file, at.line, rel))
                    continue
                kvx = None
                full = game_dir / rel
                if full.exists():
                    kvx = read_kvx(full)
                if kvx is None:
                    # defs.cpp:1973-1978: qloadkvx failed, the block is
                    # abandoned and claims no tile at all.
                    self.load_failed.append((at.file, at.line, rel))
                    continue
                nextvoxid += 1

                blk = Block()
                blk.fname, blk.line, blk.rel = at.file, at.line, rel
                blk.kvx, blk.scale, blk.claims, blk.notrans = kvx, 1.0, [], False
                self.blocks.append(blk)
                idx = len(self.blocks) - 1
                self._body(body, blk, idx)
        return self

    def _body(self, body, blk, idx):
        tile0, k = MAXTILES, 0
        while k < len(body):
            t = body[k].text.lower()
            if t == "scale" and k + 1 < len(body):
                try:
                    blk.scale = float(body[k + 1].text)
                except ValueError:
                    self.bad_tile.append((body[k + 1].file, body[k + 1].line,
                                          "scale " + body[k + 1].text))
                k += 2
            elif t == "notrans":
                blk.notrans = True
                k += 1
            elif t == "tile" and k + 1 < len(body):
                self._claim_one(body[k + 1], blk, idx)
                k += 2
            elif t == "tile0" and k + 1 < len(body):
                v = self._number(body[k + 1])
                tile0 = MAXTILES if v is None else v
                k += 2
            elif t == "tile1" and k + 1 < len(body):
                v = self._number(body[k + 1])
                k += 2
                if v is None:
                    continue
                beg, end = tile0, v
                if end < beg:
                    beg, end = end, beg   # defs.cpp:176-180 swaps, then carries on
                if not (0 <= beg < MAXUSERTILES and 0 <= end < MAXUSERTILES):
                    self.bad_tile.append((body[k - 1].file, body[k - 1].line,
                                          "tile0 %d tile1 %d" % (tile0, v)))
                    continue
                for pic in range(beg, end + 1):
                    self._store(pic, blk, idx)
            else:
                k += 1

    def _number(self, tok):
        try:
            return int(tok.text, 0)
        except ValueError:
            # scriptfile_getsymbol would resolve a `define`d name; nothing in
            # AA's voxel defs uses one, and guessing is worse than saying so.
            self.bad_tile.append((tok.file, tok.line, tok.text))
            return None

    def _claim_one(self, tok, blk, idx):
        v = self._number(tok)
        if v is None:
            return
        if not (0 <= v < MAXUSERTILES):     # defs.cpp:194-198
            self.bad_tile.append((tok.file, tok.line, tok.text))
            return
        self._store(v, blk, idx)

    def _store(self, pic, blk, idx):
        self.claims += 1
        prev = self.tile_to_vox.get(pic)
        if prev is not None and prev != idx:
            old = self.blocks[prev]
            self.overwrites.append((pic, "%s:%d %s" % (old.fname, old.line, old.rel),
                                    "%s:%d %s" % (blk.fname, blk.line, blk.rel)))
        self.tile_to_vox[pic] = idx


# ------------------------------------------------------------------ rendering

PROLOGUE = """// Generated by tools/gen_esp_voxels_aa.py: do not edit by hand. picnum -> the VOXEL AA draws on that tile and its
// extents (sizX/sizY/sizZ = .kvx header grid, pivZ = zpivot/256, radius = bounding cylinder about the pivot, scale =
// the def block's); `tilesiz` describes a FLAT tile. The last claim on a picnum wins (defs.cpp:2001).
"""


def render(walk, rows, files, kvx_files, game_dir, date, counts, notes):
    out = [PROLOGUE]

    # One summary line of what the walk found that a reader should know; the full counts go to stdout.
    facts = []
    if walk.overwrites:
        facts.append("Picnums claimed twice (LAST kept): %s." % ", ".join(str(p) for p, _o, _n in walk.overwrites))
    if notes:
        names = [n.split(" (")[0] for n in notes]
        facts.append("%s %s not reached by the engine." % (", ".join(names), "is" if len(names) == 1 else "are"))
    if facts:
        out.append("\n" + textwrap.fill(" ".join(facts), width=120, initial_indent="// ", subsequent_indent="// ") + "\n")
    if walk.load_failed:
        out.append("// `voxel` blocks whose .kvx is missing claim NO tile: %s.\n"
                   % ", ".join("%s:%d %s" % t for t in walk.load_failed))
    if walk.bad_tile:
        out.append("// Tile claims the engine would reject or this walk could not read: %s.\n"
                   % ", ".join("%s:%d %s" % t for t in walk.bad_tile))

    out.append("""
const int EKM_VOX_AA_COUNT = %d;

// One row. `sizZ` and `radius` are in the .kvx's own grid units and `scale` multiplies both.
struct EkVoxAaRow {
  short picnum;
  short sizX, sizY, sizZ;
  float pivZ;
  float radius;
  float scale;
};

// Function-local so the including translation units do not each carry a copy.
inline const EkVoxAaRow* ekm_vox_aa_rows(void) {
  static const EkVoxAaRow kRows[EKM_VOX_AA_COUNT] = {
""" % len(rows))

    for pic, k, scale in rows:
        base = os.path.basename(k.path)
        out.append('    { %6d, %4d, %4d, %4d, %10.4ff, %10.4ff, %8.4ff },\n'
                   % (pic, k.sx, k.sy, k.sz, k.pz, radius_of(k), scale))

    out.append("""  };
  return kRows;
}

// The i-th row, or a miss for an index outside the table, shaped like a picnum miss.
struct EkVoxAa {
  bool  found;
  int   picnum;
  // sizX runs ACROSS the sprite's facing and sizY ALONG it (md3_vox_calcmat_common); both are here because the box's
  // width is yaw-aware (projmath.h's ekm_voxel_width_model).
  float sizX;     // .kvx xsiz, grid units
  float sizY;     // .kvx ysiz, grid units
  float sizZ;     // .kvx zsiz, grid units
  float pivZ;     // .kvx zpivot/256, grid units
  float radius;   // bounding-cylinder radius about the pivot, grid units
  float scale;    // the def block's own `scale`
};

inline EkVoxAa ekm_vox_aa_at(int index) {
  EkVoxAa r;
  r.found = false;
  r.picnum = -1;
  r.sizX = r.sizY = r.sizZ = r.pivZ = r.radius = 0.0f;
  r.scale = 1.0f;
  if (index < 0 || index >= EKM_VOX_AA_COUNT) return r;
  const EkVoxAaRow* const row = ekm_vox_aa_rows() + index;
  r.found = true;
  r.picnum = (int)row->picnum;
  r.sizX = (float)row->sizX;
  r.sizY = (float)row->sizY;
  r.sizZ = (float)row->sizZ;
  r.pivZ = row->pivZ;
  r.radius = row->radius;
  r.scale = row->scale;
  return r;
}

// The voxel AA draws on a picnum, or a miss (the caller then sizes from the flat tile). BINARY SEARCH over the sorted
// table. A MODEL BEATS A VOXEL (polymost checks models first), so six picnums here (19960, 19962, 22323-22325,
// 22516) are never drawn as this voxel.
inline EkVoxAa ekm_vox_aa_find(int picnum) {
  const EkVoxAaRow* const rows = ekm_vox_aa_rows();
  int lo = 0, hi = EKM_VOX_AA_COUNT - 1;
  while (lo <= hi) {
    const int mid = lo + ((hi - lo) >> 1);
    const int at = (int)rows[mid].picnum;
    if (at == picnum) return ekm_vox_aa_at(mid);
    if (at < picnum) lo = mid + 1;
    else             hi = mid - 1;
  }
  EkVoxAa miss;
  miss.found = false;
  miss.picnum = -1;
  miss.sizX = miss.sizY = miss.sizZ = miss.pivZ = miss.radius = 0.0f;
  miss.scale = 1.0f;
  return miss;
}

struct EkVoxAaFile { const char* name; long size; };
const int EKM_VOX_AA_FILE_COUNT = %d;

inline const EkVoxAaFile* ekm_vox_aa_files(void) {
  static const EkVoxAaFile kFiles[EKM_VOX_AA_FILE_COUNT] = {
""" % len(files))

    for rel, sz in files:
        out.append('    { "%s", %d },\n' % (rel.replace("\\", "/"), sz))

    out.append("""  };
  return kFiles;
}

const int  EKM_VOX_AA_KVX_COUNT = %d;
const long EKM_VOX_AA_KVX_BYTES = %dL;
""" % (len(kvx_files), sum(s for _n, s in kvx_files)))

    return "".join(out)


# ------------------------------------------------------------------ main


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-dir", default=os.environ.get("AA_GAME_DIR"),
                    required="AA_GAME_DIR" not in os.environ,
                    help="<AA game folder>, the AA install root (or set AA_GAME_DIR)")
    ap.add_argument("--def", dest="deffile", default="duke3d.def",
                    help="the root .def, relative to --game-dir. AA's grpinfo "
                         "has its `defname` commented out, so the engine falls "
                         "back to this one")
    ap.add_argument("--out", default="src/ekmod/esp_voxels_aa.h",
                    help="output header path, relative to the repo root")
    ap.add_argument("--date", default="2026-09-21")
    args = ap.parse_args()

    repo = Path.cwd()
    game_dir = Path(args.game_dir)
    out_path = (repo / args.out).resolve()
    if not (game_dir / args.deffile).exists():
        print("no root def at %s" % (game_dir / args.deffile), file=sys.stderr)
        return 1

    # The include paths in a .def are relative to the game folder, so the walk
    # runs with that as the working directory -- exactly as the engine resolves
    # them.
    os.chdir(str(game_dir))
    files, missing_inc = include_order(args.deffile, args.deffile)
    for m in missing_inc:
        print("include %s: not on disk -- skipped, as the engine would" % m,
              file=sys.stderr)

    walk = Walk().run(files, game_dir)

    # EVERY .def ON DISK THAT CARRIES A `voxel` BLOCK, reached or not. A def
    # that exists but is commented out of the include chain defines nothing,
    # and saying so is the difference between "AA has no switch voxels" and
    # "AA ships them and does not load them".
    reached = set(f.replace("\\", "/").lower() for f in files)
    notes = []
    for path in sorted(Path(".").rglob("*.def")):
        rel = str(path).replace("\\", "/")
        if rel.lower() in reached:
            continue
        try:
            body = read_text(path)
        except OSError:
            continue
        toks = tokenize(body, rel)
        n = sum(1 for t in toks if t.text.lower() == "voxel" and not t.quoted)
        if n:
            notes.append("%s (%d `voxel` blocks, %d bytes)"
                         % (rel, n, os.path.getsize(str(path))))

    rows = []
    for pic in sorted(walk.tile_to_vox):
        blk = walk.blocks[walk.tile_to_vox[pic]]
        rows.append((pic, blk.kvx, blk.scale))

    used = {}
    for blk in walk.blocks:
        used[blk.kvx.path] = blk.kvx
    kvx_files = sorted((k.path, k.size) for k in used.values())

    counts = {
        'root': args.deffile,
        'file_count': len(files),
        'block_count': len(walk.blocks) + len(walk.load_failed) + len(walk.over_max),
        'loaded_count': len(walk.blocks),
        'failed_count': len(walk.load_failed),
        'claim_count': walk.claims,
        'row_count': len(rows),
        'overwrite_count': len(walk.overwrites),
        'bad_count': len(walk.bad_tile),
        'overmax_count': len(walk.over_max),
        'maxvox': MAXVOXELS,
        'kvx_count': len(kvx_files),
        'kvx_chain_count': sum(1 for k in used.values() if k.chain_ok),
        'kvx_bytes': sum(s for _n, s in kvx_files),
    }

    for key in ('file_count', 'block_count', 'loaded_count', 'failed_count',
                'claim_count', 'row_count', 'overwrite_count', 'bad_count',
                'overmax_count', 'kvx_count', 'kvx_chain_count', 'kvx_bytes'):
        print("%-34s %d" % (key + ":", counts[key]))
    for n in notes:
        print("not reached: %s" % n)

    # A COUNT AND AN ENUMERATION OF THE SAME THING MUST AGREE. Every
    # store either created a row or overwrote one, so the three must balance.
    if walk.claims != len(rows) + len(walk.overwrites):
        print("stores %d but rows+overwrites %d -- nothing written"
              % (walk.claims, len(rows) + len(walk.overwrites)), file=sys.stderr)
        return 1
    if not rows:
        print("no picnum is voxel-backed -- nothing written", file=sys.stderr)
        return 1
    # THE HEADER LAYOUT IS EVIDENCE OR THIS WRITES NOTHING. Every extent below
    # is read at a fixed offset, so a file whose mip chain does not land on the
    # palette is a file this walk cannot claim to have understood.
    if counts['kvx_chain_count'] != counts['kvx_count']:
        for k in sorted(used.values(), key=lambda v: v.path):
            if not k.chain_ok:
                print("%s: mip chain does not end at filesize-768" % k.path,
                      file=sys.stderr)
        print("%d of %d .kvx files do not parse -- nothing written"
              % (counts['kvx_count'] - counts['kvx_chain_count'],
                 counts['kvx_count']), file=sys.stderr)
        return 1

    file_rows = [(f, os.path.getsize(f)) for f in files]
    text = render(walk, rows, file_rows, kvx_files, game_dir, args.date, counts,
                  notes)
    # CRLF explicitly: every source file in this repo is CRLF and a plain
    # text-mode write emits LF, which rewrites the whole file's endings.
    with open(str(out_path), "w", encoding="utf-8", newline="") as fh:
        fh.write(text.replace("\n", "\r\n"))
    print("wrote %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
