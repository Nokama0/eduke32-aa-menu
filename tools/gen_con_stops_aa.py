#!/usr/bin/env python3
# Generates src/ekmod/con_stops_aa.h -- the fire-rate stops that come from
# Alien Armageddon's OWN CON scripts, per weapon, plus a fingerprint of the
# files they were read from.
#
# WHY THIS EXISTS. firestops.h derives a weapon's stops from the engine's live
# weapondata_t, which models P_ProcessWeapon exactly. AA's CON is a SECOND
# author of the same counter: 377 `kickback_pic` references across
# scripts/*.CON, including `setp[].kickback_pic <literal>` writes, with frame
# numbers no weapondata_t field mentions. A frame AA's script waits on that our
# advance steps over is defect D2's "fires more slowly while ammo drains at the
# full increased rate". See
# the CON frame stops design note.
#
# RUN BY HAND, NOT BY build.ps1: the build must not depend on the game being
# installed. Re-run after an AA update, from the repo root:
#
#   python tools/gen_con_stops_aa.py
#
# It overwrites src/ekmod/con_stops_aa.h. Do not hand-edit that file; fix this
# script and re-run it.

import argparse
import os
import re
import sys
import textwrap
from collections import Counter, defaultdict
from pathlib import Path

# inv.h:74. AA keeps the stock index map (AADEFS.CON:3136-3147); FLAMETHROWER
# is slot 12 in EDuke32 and AA's CON never names it, which costs nothing --
# an unnamed weapon simply collects no CON frames of its own.
MAX_WEAPONS = 13
WEAPON_NAMES = {
    "KNEE_WEAPON": 0, "PISTOL_WEAPON": 1, "SHOTGUN_WEAPON": 2,
    "CHAINGUN_WEAPON": 3, "RPG_WEAPON": 4, "HANDBOMB_WEAPON": 5,
    "SHRINKER_WEAPON": 6, "DEVISTATOR_WEAPON": 7, "TRIPBOMB_WEAPON": 8,
    "FREEZE_WEAPON": 9, "HANDREMOTE_WEAPON": 10, "GROW_WEAPON": 11,
    "FLAMETHROWER_WEAPON": 12,
}

# A frame outside this is not a frame; firestops.h bounds its own arithmetic
# with the same number for the same reason.
FRAME_MAX = 4096

# Tokens that END whatever gate was still looking for a body. `else` is the
# load-bearing one: the block after it is the NOT-this-weapon arm.
SEPARATORS = {"else", "break", "case", "default", "switch", "endswitch"}

# `if<op> WEAPON11_SHOOTS GRENADEPROJ` names weapon 11 as surely as a
# curr_weapon gate does. Only the two-operand if-commands are read this way, so
# the three tokens consumed are always op + operand + operand; AA's CON uses
# exactly ife, ifn, ifvarand and ifle with a WEAPONn_ operand and the rest are
# listed because they are the same shape.
TWO_OPERAND_IF = {
    "ife", "ifn", "ifg", "ifl", "ifge", "ifle",
    "ifvare", "ifvarn", "ifvarg", "ifvarl", "ifvarge", "ifvarle",
    "ifvarand", "ifvaror", "ifvarxor", "ifvareither", "ifvarboth",
}

# AA's per-weapon gamevars, WEAPON<n>_<FIELD>: the engine's own weapon data,
# which is why the number in the NAME is the weapon slot.
WEAPONVAR_RE = re.compile(r'^WEAPON(\d+)_[A-Za-z_0-9]+$')

# A FRAME REFERENCED ONLY TO DRAW IS NOT A STOP, AND THIS INVERTS THE SAFETY
# ARGUMENT RATHER THAN RELAXING IT. Keep-the-stop protects frames where the
# engine or the script ACTS -- acts meaning something with a game-state
# consequence: a shot, ammo, a sound, a spawn, a flag. A vertical draw offset
# for the HUD weapon sprite has none, and the gun-bob it produces IS the firing
# animation, which is the thing the fire rate exists to shorten. Refusing to
# advance past it is refusing to do the job.
#
# CLASSIFIED PER SITE, NEVER PER FILE. weapons_hud.CON is a hint, not a proof:
# if anything in it keyed on kickback_pic had a consequence, excluding it would
# skip a real action. Every one of that file's kickback_pic references was read
# -- there are exactly eight, all listed here. A site that cannot be classified
# confidently KEEPS ITS STOP.
#
# THE RUN FAILS IF ANY ENTRY BELOW GOES UNMATCHED. A line that moved, a frame
# that changed or a file that was renamed would otherwise silently restore a
# stop this table means to drop, or drop one it does not -- so the generator
# refuses to write rather than publish a table whose exclusions no longer
# describe the script.
DRAW_ONLY_WHY = (
    "`add z <offset>` on the local draw var `z`, set two lines above and read "
    "only by the rotatesprite pair below it: a vertical offset for the HUD "
    "weapon sprite. Fires nothing, takes no ammo, plays no sound, sets no flag")
DRAW_ONLY = {}
for _line, _frame in ((4559, 4), (4560, 5), (4561, 6), (4562, 7),
                      (9346, 4), (9347, 5), (9348, 6), (9349, 7)):
    DRAW_ONLY[("weapons_hud.CON", _line, _frame)] = DRAW_ONLY_WHY

# WHICH STOP A LITERAL EARNS DEPENDS ON THE OPERATOR THAT READ IT. A frame is
# where AA acts; a stop is a counter value we may leave the counter on. The tic
# increments FIRST (player.cpp:4588), so the frame OBSERVED after a stop at s
# is s + 1 -- firestops.h's own off-by-one, for its own reason.
#
# ACTS_AT_F first becomes true when the counter reaches F, so the stop is F - 1
# and F itself is provably spurious. ACTS_AT_F_PLUS_1 (`>` F) first becomes
# true at F + 1, so the stop is F and F - 1 is provably spurious.
#
# EVERYTHING ELSE KEEPS BOTH, and that is the same rule uncertain attribution
# follows: a write names no "first acts at" frame at all, `ifvarand` is a bit
# test rather than an ordering, and `ifn`/`ifl` are true across a RANGE whose
# boundary sits either side of the literal. An operator this table does not
# name keeps both too -- a spurious stop costs a little speed, a missing one
# skips a shot.
ACTS_AT_F = {"ife", "ifvare", "ifge", "ifvarge", "ifle", "ifvarle", "case"}
ACTS_AT_F_PLUS_1 = {"ifg", "ifvarg"}


# ---------------------------------------------------------------------------
def tokenize(text):
    """Whitespace tokens with line numbers, comments removed. A block comment
    is replaced by its own newlines so line numbers stay true -- the first
    version collapsed them and every reported line after the first `/*` was
    wrong."""
    text = re.sub(r'/\*.*?\*/', lambda m: "\n" * m.group(0).count("\n"),
                  text, flags=re.S)
    out = []
    for lineno, line in enumerate(text.splitlines(), 1):
        line = re.sub(r'//.*', '', line)
        line = line.replace('{', ' { ').replace('}', ' } ')
        for t in line.split():
            out.append((t, lineno))
    return out


def member(tok):
    """`player[THISACTOR].kickback_pic` -> ('player', 'kickback_pic')."""
    m = re.match(r'^([A-Za-z_]+)\[[^\]]*\]\.([A-Za-z_]+)$', tok)
    return (m.group(1).lower(), m.group(2).lower()) if m else (None, None)


def is_int(tok):
    return re.fullmatch(r'-?\d+', tok) is not None


def resolve_weapon(tok):
    if tok in WEAPON_NAMES:
        return WEAPON_NAMES[tok]
    if is_int(tok) and 0 <= int(tok) < MAX_WEAPONS:
        return int(tok)
    return None


def weaponvar(tok):
    """`WEAPON11_SHOOTS` -> 11. A slot outside the table is not one."""
    m = WEAPONVAR_RE.match(tok)
    if not m:
        return None
    n = int(m.group(1))
    return n if 0 <= n < MAX_WEAPONS else None


def is_curr_weapon(tok):
    head, mem = member(tok)
    return head == 'player' and mem == 'curr_weapon'


def scan_blocks(toks):
    """For every token, the innermost brace block around it; for every block,
    the weapons its OWN level names in a curr_weapon test.

    THIS IS A WHOLE-BLOCK LOOK BECAUSE THE TEST CAN SIT BELOW THE SITE.
    AAPLAYER.CON:16169-16173 is `ife rpgchange YES { ifg player[].kickback_pic
    20 ... ifn player[].curr_weapon RPG_WEAPON ... }`: the frame is on the line
    ABOVE the only thing that names the weapon, so the sequential gate walk in
    parse() cannot see it and a second pass has to.

    A block whose own level also holds an `else` is marked ambiguous and names
    nothing: `ife curr_weapon X ... else ...` covers X and everything that is
    not X, so the block is not about one weapon. That is what keeps
    AAPLAYER.CON:13640's `setp[].kickback_pic 1` -- which runs for the shrinker
    AND the grow weapon -- from being narrowed onto the shrinker alone."""
    blocks = [{'weapons': set(), 'ambiguous': False, 'else_here': False}]
    tok_block = [0] * len(toks)
    stack = [0]
    n = len(toks)
    for i in range(n):
        tok = toks[i][0]
        if tok == '{':
            tok_block[i] = stack[-1]
            blocks.append({'weapons': set(), 'ambiguous': False,
                           'else_here': False})
            stack.append(len(blocks) - 1)
            continue
        if tok == '}':
            if len(stack) > 1:
                stack.pop()
            tok_block[i] = stack[-1]
            continue
        tok_block[i] = stack[-1]
        b = blocks[stack[-1]]
        if tok.lower() == 'else':
            b['else_here'] = True
        elif is_curr_weapon(tok) and i and \
                toks[i - 1][0].lower().startswith('if'):
            w = resolve_weapon(toks[i + 1][0]) if i + 1 < n else None
            if w is None:
                b['ambiguous'] = True
            else:
                b['weapons'].add(w)
    return blocks, tok_block


def block_weapon(blocks, tok_block, i):
    """The one weapon the block around token `i` is about, or None. Two
    different weapons, an unreadable operand or an `else` on the same level all
    mean None -- the site stays uncertain and goes to every weapon."""
    b = blocks[tok_block[i]]
    if b['ambiguous'] or b['else_here'] or len(b['weapons']) != 1:
        return None
    return frozenset(b['weapons'])


def collect_commands(files):
    """The set of tokens that start a STATEMENT, learned from the CON itself
    rather than from a hand-written keyword list this script would have to
    keep in step with EDuke32.

    A hand-written CON puts one command at the start of a line, so the first
    token of every line is a command. The if-family and the block/switch
    punctuation are removed because they are handled structurally. An OPERAND
    that happens to start some line anywhere in the mod lands in here wrongly
    -- and that direction is safe: it ends a gate EARLY, which makes a frame
    UNCERTAIN, which gives it to every weapon."""
    cmds = set()
    for p in files:
        toks = tokenize(p.read_text(encoding='latin-1'))
        last = None
        for tok, ln in toks:
            if ln != last:
                last = ln
                low = tok.lower()
                if low.startswith('if') or low in SEPARATORS or tok in ('{', '}'):
                    continue
                cmds.add(low)
    return cmds


# ---------------------------------------------------------------------------
def combine(a, b):
    """Two attributions, intersected. None means UNCONSTRAINED -- not "no
    weapons" -- so it is the identity here."""
    if a is None:
        return b
    if b is None:
        return a
    return a & b


class Extractor(object):
    def __init__(self, commands):
        self.commands = commands
        self.frames = defaultdict(set)      # weapon -> {frame}, for the report
        self.stops = defaultdict(set)       # weapon -> {stop}, what is emitted
        self.uncertain = []                 # (file, line, op, frame, why)
        self.unresolved = []                # (file, line, op, rhs)
        self.drawonly = []                  # (file, line, op, frame)
        self.drawseen = set()               # DRAW_ONLY keys actually matched
        self.stats = Counter()

    # -- one frame, attributed or not ---------------------------------------
    def record(self, frame, ctx, path, ln, op, site, fallback=None):
        key = (path.name, ln, frame)
        if key in DRAW_ONLY:
            # DRAW-ONLY: no stop, on any weapon. The site is still counted and
            # still published, so the decision is visible and reversible.
            self.drawseen.add(key)
            self.stats['draw_only'] += 1
            self.drawonly.append((path.name, ln, op, frame))
            return
        if frame == 0:
            # Frame 0 is REST, not a frame to land on: firerate_advance
            # refuses a counter at 0 outright, and `setp[].kickback_pic 0` is
            # AA ending a sequence rather than waiting inside one.
            self.stats['frame_zero'] += 1
            return
        if frame < 0 or frame > FRAME_MAX:
            self.stats['frame_out_of_range'] += 1
            self.unresolved.append((path.name, ln, op, str(frame)))
            return
        # FORM C, and it is a FALLBACK: the weapon the enclosing BLOCK is
        # about, consulted only where the gate walk found no constraint at
        # all. An EMPTY ctx is two gates contradicting each other and is never
        # widened by it -- that site stays uncertain, as the design requires.
        if ctx is None and fallback is not None:
            ctx = fallback
            self.stats['attr_block'] += 1
        if ctx is None or len(ctx) == 0:
            # UNCERTAIN GOES TO EVERY WEAPON. An empty ctx is a contradiction
            # -- two nested gates naming different weapons, which is dead code
            # we cannot read; None is "nothing above or around it names a
            # weapon at all". Both are the same answer here and the reason is
            # the same: a frame on the wrong weapon LOSES a stop on the right
            # one.
            self.stats['uncertain'] += 1
            why = ("nothing above or around it names a weapon" if ctx is None
                   else "two nested gates naming different weapons")
            if site:
                why = site + ", " + why
            self.uncertain.append((path.name, ln, op, frame, why))
            for w in range(MAX_WEAPONS):
                self.frames[w].add(frame)
                self.stops[w].update(stops_for(frame, op))
            self.stats['opclass_' + op_class(op)] += 1
            return
        self.stats['attributed'] += 1
        self.stats['opclass_' + op_class(op)] += 1
        for w in ctx:
            self.frames[w].add(frame)
            self.stops[w].update(stops_for(frame, op))

    # -- where a gate's body ends --------------------------------------------
    @staticmethod
    def body_line(toks, j, ln):
        """None if the gate owns the block that FOLLOWS, else the line its
        braceless body is confined to. A gate whose line ENDS at token j, or
        continues into another if, owns the block; a gate followed by anything
        else on its own line does not."""
        nxt = toks[j] if j < len(toks) else None
        if nxt is None or nxt[1] != ln or nxt[0] == '{' \
           or nxt[0].lower().startswith('if'):
            return None
        return ln

    # -- the walk ------------------------------------------------------------
    def parse(self, path):
        toks = tokenize(path.read_text(encoding='latin-1'))
        blocks, tok_block = scan_blocks(toks)
        n = len(toks)
        ctx = None              # attribution in force, None == unconstrained
        stack = []              # (ctx, negative gate) saved at each '{'
        pending = None          # a gate that has not met its body yet
        pending_line = None     # not None: that body is the rest of THIS line
        neg = None              # `ifn curr_weapon W` whose ELSE arm is W
        neg_line = None         # the same thing for the negative gate
        carried = None          # a negative gate whose body just closed
        sw = []                 # switch frames: (on_weapon, on_kick, base_ctx)
        i = 0
        while i < n:
            tok, ln = toks[i]
            low = tok.lower()
            # `carried` lives for exactly ONE token: the `}` that closed a
            # negative gate's body hands its weapon to the token after it, and
            # that token is either `else` or nothing to do with it.
            else_weapon, carried = carried, None

            if pending_line is not None and ln != pending_line:
                pending, pending_line = None, None
            if neg_line is not None and ln != neg_line:
                # A braceless negative body ends with its line, so an `else` on
                # the next line is that gate's positive arm.
                else_weapon, neg, neg_line = neg, None, None

            if tok == '{':
                stack.append((ctx, neg))
                ctx = combine(ctx, pending)
                pending, pending_line = None, None
                neg, neg_line = None, None
                i += 1
                continue
            if tok == '}':
                ctx, carried = stack.pop() if stack else (None, None)
                pending, pending_line = None, None
                neg, neg_line = None, None
                i += 1
                continue

            here = combine(ctx, pending)
            fallback = block_weapon(blocks, tok_block, i)

            # -- THE ELSE OF A NEGATIVE GATE IS A POSITIVE ATTRIBUTION -------
            # not-not-weapon-8 is weapon 8. AAPLAYER.CON:17040-17043 is
            # `ifn player[].curr_weapon 8 set triphack -1 / else { ife
            # player[].kickback_pic 4 ... }`, and frame 4 there is the
            # tripbomb's. The `ifn` BODY still names nothing: it is every
            # weapon except this one.
            if low == 'else':
                w = else_weapon
                if w is None and neg is not None and neg_line == ln:
                    w = neg          # `ifn ... <body> else ...`, all one line
                neg, neg_line = None, None
                if w is not None:
                    self.stats['gate_else'] += 1
                    pending = frozenset([w])
                    pending_line = self.body_line(toks, i + 1, ln)
                    i += 1
                    continue

            # ANOTHER `if` TAKES THE BLOCK, AND WITH IT THE `else`. A negative
            # gate still waiting for a braced body is dropped the moment a
            # further condition is chained onto it: in CON the `else` binds to
            # the NEAREST if, so `ifn curr_weapon 8 / ifg x 3 { } else { }`
            # would otherwise hand that else arm to weapon 8 -- the one weapon
            # it is the only thing to exclude. The gate below re-arms it.
            if neg is not None and neg_line is None and low.startswith('if'):
                neg, neg_line = None, None

            # -- the gates on player[].curr_weapon ---------------------------
            if low.startswith('if') and i + 2 < n and is_curr_weapon(toks[i + 1][0]):
                w = resolve_weapon(toks[i + 2][0])
                if low in ('ife', 'ifvare'):
                    if w is None:
                        self.stats['gate_unresolved'] += 1
                        pending, pending_line = None, None
                    else:
                        self.stats['gate'] += 1
                        pending = frozenset([w])
                        pending_line = self.body_line(toks, i + 3, ln)
                    i += 3
                    continue
                if low in ('ifn', 'ifvarn'):
                    neg, neg_line = None, None
                    if w is None:
                        self.stats['gate_unresolved'] += 1
                    else:
                        self.stats['gate_neg'] += 1
                        neg = w
                        neg_line = self.body_line(toks, i + 3, ln)
                    pending, pending_line = None, None
                    i += 3
                    continue

            # -- A WEAPONn_* FIELD NAMES THE WEAPON --------------------------
            # `ife WEAPON11_SHOOTS GRENADEPROJ` (AAPLAYER.CON:13643) is a test
            # on weapon 11's own engine data, so the block under it is weapon
            # 11's. Only the two-operand if-commands are read this way, so the
            # three tokens consumed are always op + operand + operand.
            #
            # ONLY THE FIRST OPERAND, and that is not fastidiousness: 16 sites
            # are `ife player[].kickback_pic WEAPONn_FIREDELAY`, where the
            # weapon var is the value being COMPARED AGAINST. Reading those
            # here swallowed the kickback_pic reference whole -- the run lost
            # 16 of its 371 references and 16 of its 26 unreadable operands,
            # silently, because this branch sits above the one that counts
            # them.
            if low in TWO_OPERAND_IF and i + 2 < n:
                a = weaponvar(toks[i + 1][0])
                if a is not None:
                    self.stats['gate_weaponvar'] += 1
                    pending = frozenset([a])
                    pending_line = self.body_line(toks, i + 3, ln)
                    i += 3
                    continue

            # -- switch, which is AA's OTHER way of gating on the weapon ------
            if low == 'switch' and i + 1 < n:
                head, mem = member(toks[i + 1][0])
                sw.append((head == 'player' and mem == 'curr_weapon',
                           head == 'player' and mem == 'kickback_pic', ctx))
                pending, pending_line = None, None
                i += 2
                continue
            if low == 'endswitch':
                if sw:
                    ctx = sw.pop()[2]
                pending, pending_line = None, None
                i += 1
                continue
            if low == 'default' and sw:
                # The arm reached when NO case matched: we cannot name the
                # weapon, so the arm keeps the switch's own attribution.
                ctx = sw[-1][2]
                pending, pending_line = None, None
                i += 1
                continue
            if low == 'case' and sw and (sw[-1][0] or sw[-1][1]):
                onWeapon, onKick, base = sw[-1]
                vals, j = [], i
                while j + 1 < n and toks[j][0].lower() == 'case':
                    vals.append(toks[j + 1][0])
                    j += 2
                if onWeapon:
                    ws = [resolve_weapon(v) for v in vals]
                    if any(w is None for w in ws):
                        self.stats['case_unresolved'] += 1
                        ctx = base
                    else:
                        self.stats['case_gate'] += 1
                        ctx = combine(base, frozenset(ws))
                else:
                    for v in vals:
                        if is_int(v):
                            self.record(int(v), base, path, ln, 'case',
                                        'switch arm', fallback)
                        else:
                            self.unresolved.append((path.name, ln, 'case', v))
                pending, pending_line = None, None
                i = j
                continue

            # -- a kickback_pic reference ------------------------------------
            head, mem = member(tok)
            if mem == 'kickback_pic':
                prev = toks[i - 1][0].lower() if i else ''
                nxt = toks[i + 1][0] if i + 1 < n else ''
                if head == 'player' and prev.startswith('if'):
                    self.stats['test'] += 1
                    if is_int(nxt):
                        self.record(int(nxt), here, path, ln, prev, '', fallback)
                    else:
                        self.unresolved.append((path.name, ln, prev, nxt))
                elif head in ('setp', 'setplayer'):
                    self.stats['write'] += 1
                    if is_int(nxt):
                        # AA WRITING a frame means that frame matters to it,
                        # so a written literal is treated exactly like a
                        # tested one.
                        self.record(int(nxt), here, path, ln, 'set', '', fallback)
                    else:
                        self.unresolved.append((path.name, ln, 'set', nxt))
                else:
                    # `getp[].kickback_pic <var>` reads the counter into a
                    # gamevar and `switch player[].kickback_pic` is counted by
                    # its case arms above: neither names a frame.
                    self.stats['read_or_switch'] += 1
                i += 1
                continue

            if low.startswith('if'):
                i += 1
                continue
            if low in SEPARATORS:
                pending, pending_line = None, None
                neg, neg_line = None, None
                i += 1
                continue
            if low in self.commands:
                # A STATEMENT, so this gate's body is braceless and ends with
                # this line -- it does NOT own some later block. The negative
                # gate is bounded the same way, so its `else` is still found.
                if pending is not None and pending_line is None:
                    pending_line = ln
                if neg is not None and neg_line is None:
                    neg_line = ln
            i += 1


# ---------------------------------------------------------------------------
def op_class(op):
    """'at_f', 'at_f1' or 'both' -- see ACTS_AT_F above."""
    low = (op or "").lower()
    if low in ACTS_AT_F:
        return 'at_f'
    if low in ACTS_AT_F_PLUS_1:
        return 'at_f1'
    return 'both'


def stops_for(frame, op):
    """The stop(s) one literal earns, given the operator that read it. A value
    below 1 is dropped: firerate_advance refuses a counter at 0 or below, so a
    stop there can never be the value an advance clamps to. Frame 1 therefore
    needs no stop of its own -- the engine's own fire press is what sets the
    counter to 1."""
    cls = op_class(op)
    out = []
    if cls in ('at_f', 'both') and frame - 1 >= 1:
        out.append(frame - 1)
    if cls in ('at_f1', 'both') and frame >= 1:
        out.append(frame)
    return out


# The frames a shot is actually fired in. Past this a weapon's cycle has long
# finished, so a gap that starts up there is not something the fire rate works
# with.
GAP_WINDOW = 40


def largest_gap(stops):
    """The largest jump between two CONSECUTIVE stops that both sit below
    GAP_WINDOW -- what an advance actually has to skip over.

    1 means the stops down there are consecutive integers and NOTHING can be
    skipped, whatever the set's size; 0 means there are fewer than two stops
    below the window to jump between. Stops at or above the window are not
    counted, so this never credits a weapon for a jump it can only make after
    its cycle is over."""
    low = [s for s in sorted(stops) if s < GAP_WINDOW]
    if len(low) < 2:
        return 0
    return max(low[k + 1] - low[k] for k in range(len(low) - 1))


def frames_to_stops(frames):
    """The BLANKET conversion this generator used before the operator was
    carried to the site: both F-1 and F for every frame, whatever read it.
    Kept only so the per-weapon report can print what the per-operator rule
    saved, and never used to emit."""
    out = set()
    for f in frames:
        if f - 1 >= 1:
            out.add(f - 1)
        if f >= 1:
            out.add(f)
    return sorted(out)


# This note was wanted beside kW03 in the generated header.
# It is here rather than in the header because that file refuses hand-edits and
# a re-run would drop it.
W03_FLOOR_NOTE = (
    "  // The chaingun's fire-rate floor comes from the load-bearing subset of these frames, not all of them (most are\n"
    "  // animation and sound): docs/re/chaingun-floor.md classifies every one below TotalTime, 12 of 12.")

HEADER = """#pragma once

// Generated by tools/gen_con_stops_aa.py from Alien Armageddon's own CON scripts (<game folder>/scripts/*.CON): do
// not hand-edit, fix the script and re-run. A measurement of this install, not a stop set in force.

const int EKM_CON_MAX_WEAPONS = {max_weapons};

// The files this table was read from, sorted by name, with each one's size (a long: what constops.cpp narrows the
// Win32 file size to; every AA script is far below 2 GB).
struct EkConScriptFile {{ const char* name; long size; }};
const int EKM_CON_FILE_COUNT = {file_count};

inline const EkConScriptFile* ekm_con_files(void) {{
  // Function-local so the including translation units do not each carry a copy, as weaponspeed.h's own table is.
  static const EkConScriptFile kFiles[EKM_CON_FILE_COUNT] = {{
{files}
  }};
  return kFiles;
}}

// One weapon's CON stops, returned by value: a two-field return cannot be misused by reading an out-param in the list
// that fills it.
struct EkConStops {{ int nStops; const short* stops; }};

inline EkConStops ekm_con_stops_aa(int weapon) {{
{tables}
  // A weapon with no CON frames: never a null pointer or a zero-length array (/W3 /WX rejects the latter).
  static const short kNone[1] = {{ 0 }};
  EkConStops r;
  r.nStops = 0;
  r.stops = kNone;
  switch (weapon) {{
{cases}
    default: break;
  }}
  return r;
}}

// The largest gap between two consecutive stops that are both below EKM_CON_GAP_WINDOW: 1 means consecutive integers
// and nothing can be skipped; 0 means fewer than two stops to jump between.
const int EKM_CON_GAP_WINDOW = {gap_window};

inline int ekm_con_largest_gap(int weapon) {{
  static const short kGap[EKM_CON_MAX_WEAPONS] = {{ {gap_values} }};
  if (weapon < 0 || weapon >= EKM_CON_MAX_WEAPONS) return 0;
  return kGap[weapon];
}}

// The sites this run could not attribute, as data and not only as the prose at the end of this file.
struct EkConUncertainSite {{ const char* file; int line; }};
const int EKM_CON_UNCERTAIN_COUNT = {uncertain};

inline const EkConUncertainSite* ekm_con_uncertain(void) {{
  // Unsized so a run with no uncertain site still compiles (/W3 /WX rejects a zero-length array; such a run emits one
  // dead row that a count of 0 keeps loops away from). The assert stops the count and the rows drifting apart.
  static const EkConUncertainSite kSites[] = {{
{uncertain_rows}
  }};
  static_assert((int)(sizeof(kSites) / sizeof(kSites[0])) ==
                (EKM_CON_UNCERTAIN_COUNT > 0 ? EKM_CON_UNCERTAIN_COUNT : 1),
                "the uncertain count and the uncertain rows must agree");
  return kSites;
}}

// The draw-only exclusions, as data so the suite can ask whether one particular CON line carries a stop. Reversible
// by deleting the entry from DRAW_ONLY in the generator and re-running it.
struct EkConDrawOnlySite {{ const char* file; int line; int frame; }};
const int EKM_CON_DRAW_ONLY_COUNT = {draw_only};

inline const EkConDrawOnlySite* ekm_con_draw_only(void) {{
  // Unsized for the same reason ekm_con_uncertain's table is.
  static const EkConDrawOnlySite kSites[] = {{
{drawonly_rows}
  }};
  static_assert((int)(sizeof(kSites) / sizeof(kSites[0])) ==
                (EKM_CON_DRAW_ONLY_COUNT > 0 ? EKM_CON_DRAW_ONLY_COUNT : 1),
                "the draw-only count and the draw-only rows must agree");
  return kSites;
}}

inline bool ekm_con_is_draw_only(const char* file, int line) {{
  if (!file) return false;
  const EkConDrawOnlySite* const s = ekm_con_draw_only();
  for (int i = 0; i < EKM_CON_DRAW_ONLY_COUNT; i++) {{
    if (s[i].line != line) continue;
    const char* a = s[i].file;
    const char* b = file;
    while (*a && *b && *a == *b) {{ a++; b++; }}
    if (*a == 0 && *b == 0) return true;
  }}
  return false;
}}

// Whether one CON line went to every weapon. Exact spelling, exact line.
inline bool ekm_con_is_uncertain(const char* file, int line) {{
  if (!file) return false;
  const EkConUncertainSite* const s = ekm_con_uncertain();
  for (int i = 0; i < EKM_CON_UNCERTAIN_COUNT; i++) {{
    if (s[i].line != line) continue;
    const char* a = s[i].file;
    const char* b = file;
    while (*a && *b && *a == *b) {{ a++; b++; }}
    if (*a == 0 && *b == 0) return true;
  }}
  return false;
}}

// The recorded size for one script name, or -1 for a name this table does not have. Linear over {file_count} entries, called
// once at load on the worker thread and never on a frame.
inline long ekm_con_recorded_size(const char* name) {{
  if (!name) return -1;
  const EkConScriptFile* const f = ekm_con_files();
  for (int i = 0; i < EKM_CON_FILE_COUNT; i++) {{
    const char* a = f[i].name;
    const char* b = name;
    while (*a && *b) {{
      char ca = *a, cb = *b;
      if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
      if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
      if (ca != cb) break;
      a++; b++;
    }}
    if (*a == 0 && *b == 0) return f[i].size;
  }}
  return -1;
}}

{trailer}
"""


def render(ex, files, game_dir, date):
    stops = {w: sorted(ex.stops[w]) for w in range(MAX_WEAPONS)}

    file_rows = ",\n".join(
        '    {{ "{0}", {1} }}'.format(p.name, p.stat().st_size) for p in files)

    tables, cases = [], []
    for w in range(MAX_WEAPONS):
        s = stops[w]
        if not s:
            continue
        rows = ", ".join(str(v) for v in s)
        if w == 3:
            # Emitted from the generator, not hand-added to the header: this
            # file says DO NOT HAND-EDIT and a re-run would silently drop it.
            tables.append(W03_FLOOR_NOTE)
        tables.append("  static const short kW%02d[%d] = { %s };" % (w, len(s), rows))
        cases.append("    case %d: r.nStops = %d; r.stops = kW%02d; break;"
                     % (w, len(s), w))

    unc = ex.uncertain
    if unc:
        unc_lines = []
        for name, ln, op, frame, why in unc:
            unc_lines.append("//   %s:%d  `%s ... %d` -- %s" % (name, ln, op, frame, why))
        uncertain_list = "\n".join(unc_lines)
    else:
        uncertain_list = "//   (none)"
    # The same sites as code. A zero-length array is rejected by /W3 /WX, so an
    # empty run emits one dead row rather than nothing -- and the count stays
    # the truth, because every loop over this table is bounded by the count.
    if unc:
        uncertain_rows = ",\n".join(
            '    {{ "{0}", {1} }}'.format(name, ln) for name, ln, _o, _f, _w in unc)
    else:
        uncertain_rows = '    { "", 0 }'

    if ex.unresolved:
        unres_lines = ["//   %s:%d  `%s ... %s`" % (n, l, o, r)
                       for n, l, o, r in ex.unresolved]
        unresolved_list = "\n".join(unres_lines)
    else:
        unresolved_list = "//   (none)"

    if ex.drawonly:
        # GROUPED BY REASON, because eight copies of one paragraph is noise a
        # reader skips -- and skipping is how a wrong exclusion survives. The
        # sites are still listed one by one, which is the decision itself.
        draw_lines, by_why = [], []
        for n, l, o, f in ex.drawonly:
            why = DRAW_ONLY[(n, l, f)]
            if not by_why or by_why[-1][0] != why:
                by_why.append((why, []))
            by_why[-1][1].append("%s:%d `%s ... %d`" % (n, l, o, f))
        for why, sites in by_why:
            for chunk in textwrap.wrap(why, 70):
                draw_lines.append("//   " + chunk)
            for s in sites:
                draw_lines.append("//     " + s)
        drawonly_list = "\n".join(draw_lines)
        drawonly_rows = ",\n".join(
            '    {{ "{0}", {1}, {2} }}'.format(n, l, f)
            for n, l, _o, f in ex.drawonly)
    else:
        drawonly_list = "//   (none)"
        drawonly_rows = '    { "", 0, 0 }'

    weaponvar = sum(1 for _n, _l, _o, r in ex.unresolved
                    if re.match(r'^WEAPON\d+_', r))
    ref_total = ex.stats['test'] + ex.stats['write'] + ex.stats['read_or_switch']
    total_sites = (ex.stats['attributed'] + ex.stats['uncertain'] +
                   ex.stats['draw_only'] + ex.stats['frame_zero'] +
                   len(ex.unresolved) + ex.stats['read_or_switch'])

    # The closing note: uncertain sites by file, then what the unresolved references are, wrapped at 120.
    by_file = {}
    for name, ln, _o, _f, _w in unc:
        by_file.setdefault(name, []).append(str(ln))
    sites = "; ".join("%s %s" % (n, ", ".join(v)) for n, v in sorted(by_file.items())) or "none"
    kinds = []
    if weaponvar:
        kinds.append("WEAPONn_" + "/".join(sorted({re.sub(r'^WEAPON\d+_', '', r)
                                                  for _n, _l, _o, r in ex.unresolved
                                                  if re.match(r'^WEAPON\d+_', r)})))
    if any(not re.match(r'^WEAPON\d+_', r) for _n, _l, _o, r in ex.unresolved):
        kinds.append("temp variables")
    unres_files = sorted({n for n, _l, _o, _r in ex.unresolved})
    trailer = textwrap.fill(
        "Uncertain frames, each on every weapon above (nothing near them names a weapon): %s. "
        "%d further references have no literal to read (%s, in %s)."
        % (sites, len(ex.unresolved), " or ".join(kinds), " and ".join(unres_files)),
        width=117, initial_indent="// ", subsequent_indent="// ")

    gaps = [largest_gap(stops[w]) for w in range(MAX_WEAPONS)]
    gap_table = "\n".join(
        "//   w%02d  %2d stop(s), largest gap %2d" % (w, len(stops[w]), gaps[w])
        for w in range(MAX_WEAPONS))

    return HEADER.format(
        game_dir=str(game_dir).replace("\\", "/"),
        date=date,
        opclass_at_f=ex.stats['opclass_at_f'],
        opclass_at_f1=ex.stats['opclass_at_f1'],
        opclass_both=ex.stats['opclass_both'],
        ref_total=ref_total,
        case_frames=total_sites - ref_total,
        total_sites=total_sites,
        read_or_switch=ex.stats['read_or_switch'],
        file_count=len(files),
        max_weapons=MAX_WEAPONS,
        attributed=ex.stats['attributed'],
        uncertain=ex.stats['uncertain'],
        gate=ex.stats['gate'],
        case_gate=ex.stats['case_gate'],
        gate_weaponvar=ex.stats['gate_weaponvar'],
        gate_else=ex.stats['gate_else'],
        attr_block=ex.stats['attr_block'],
        gap_window=GAP_WINDOW,
        gap_table=gap_table,
        gap_values=", ".join(str(g) for g in gaps),
        uncertain_rows=uncertain_rows,
        draw_only=ex.stats['draw_only'],
        drawonly_list=drawonly_list,
        drawonly_rows=drawonly_rows,
        unresolved=len(ex.unresolved),
        weaponvar=weaponvar,
        frame_zero=ex.stats['frame_zero'],
        files=file_rows,
        tables="\n".join(tables),
        cases="\n".join(cases),
        trailer=trailer,
    )


SHOT_ROW_RE = re.compile(r'"([A-Za-z0-9_]+\.CON)",\s*\{(.*?)\}\s*\}', re.S)
# No closing brace: the row pattern's own terminator eats the last cite's.
SHOT_CITE_RE = re.compile(r'\{\s*(\d+),\s*"((?:[^"\\]|\\.)*)"')


def check_shots(scripts_dir, header):
    """con_shots_aa.h is HAND-WRITTEN (its own comment says why), so it cannot
    be regenerated -- but every line it cites can be compared with the script.
    Reads only; writes nothing. A mismatch fails the run. (D15 Part B,
    2026-09-24)"""
    text = Path(header).read_text(encoding="utf-8")
    checked, bad = 0, []
    cache = {}
    for m in SHOT_ROW_RE.finditer(text):
        name = m.group(1)
        if name not in cache:
            p = Path(scripts_dir) / name
            cache[name] = (p.read_text(encoding="latin-1").split("\n")
                           if p.exists() else None)
        lines = cache[name]
        for c in SHOT_CITE_RE.finditer(m.group(2)):
            ln, want = int(c.group(1)), c.group(2).replace('\\"', '"')
            checked += 1
            got = lines[ln - 1].strip() if lines and 0 < ln <= len(lines) else None
            if got != want:
                bad.append((name, ln, want, got))
    print("con_shots_aa.h: %d cited line(s) checked, %d differ" % (checked, len(bad)))
    for name, ln, want, got in bad:
        print("   %s:%d  header %r  script %r" % (name, ln, want, got), file=sys.stderr)

    # THE HAND-KEPT WRITE LISTS (ekm_con_writes_aa), against this generator's
    # own attribution of every literal `setp[].kickback_pic N` write, N > 0:
    # the slot's own plus those given to every slot. (fix round 1, 2026-09-24)
    files = sorted(Path(scripts_dir).glob("*.CON"), key=lambda p: p.name.upper())
    writes = defaultdict(set)

    class _W(Extractor):
        def record(self, frame, ctx, path, ln, op, site, fallback=None):
            if op == 'set' and frame > 0:
                c = ctx if ctx is not None else fallback
                for w in (c if c else range(MAX_WEAPONS)):
                    writes[w].add(frame)
            return Extractor.record(self, frame, ctx, path, ln, op, site, fallback)

    ex = _W(collect_commands(files))
    for p in files:
        ex.parse(p)
    wbad = 0
    rows = re.findall(r'kW(\d+)\[\d+\]\s*=\s*\{([^}]*)\}', text)
    for w, vals in rows:
        have = sorted(int(v) for v in vals.split(","))
        want = sorted(writes[int(w)])
        if have != want:
            wbad += 1
            print("   w%s writes: header %s  script %s" % (w, have, want), file=sys.stderr)
    print("con_shots_aa.h: %d write list(s) checked, %d differ" % (len(rows), wbad))
    return 1 if bad or wbad or checked == 0 or not rows else 0


RELOAD_ROW_RE = re.compile(
    r'\{\s*(EK_RL_ANY_SLOT|\d+),\s*(\d+),\s*"(\w+)",\s*[^,]+,\s*EK_RL_([EUCI]),\s*'
    r'"([A-Za-z0-9_]+\.CON)",\s*(\d+),\s*"((?:[^"\\]|\\.)*)"\s*\}')
RELOAD_FLOOR_RE = re.compile(r'case (\d+): return (\d+);')


def check_reload(scripts_dir, header):
    """con_reload_aa.h is a HAND-WRITTEN LEDGER (AA2, 2026-09-25): every site
    this generator attributes to a ledgered slot, or to every slot, with a stop
    at or above that slot's floor must be a row there, each row must be such a
    site with the same operator and frame, and each cited line must match the
    script. Reads only; any failure fails the run."""
    text = Path(header).read_text(encoding="utf-8")
    body = text[text.find("ekm_reload_floor"):]
    floors = {int(w): int(f) for w, f in RELOAD_FLOOR_RE.findall(body[:body.find("default")])}
    rows = RELOAD_ROW_RE.findall(text)
    files = sorted(Path(scripts_dir).glob("*.CON"), key=lambda p: p.name.upper())
    lines = {p.name: p.read_text(encoding="latin-1").split("\n") for p in files}

    sites = []

    class _S(Extractor):
        def record(self, frame, ctx, path, ln, op, site, fallback=None):
            c = ctx if ctx is not None else fallback
            sites.append((path.name, ln, op, frame, None if not c else set(c)))
            return Extractor.record(self, frame, ctx, path, ln, op, site, fallback)

    ex = _S(collect_commands(files))
    for p in files:
        ex.parse(p)

    ledger = {}
    bad_cite = []
    for w, frame, op, cls, name, ln, want in rows:
        w = -1 if w == "EK_RL_ANY_SLOT" else int(w)
        ln, frame = int(ln), int(frame)
        want = want.replace('\\"', '"')
        src = lines.get(name)
        got = src[ln - 1].strip() if src and 0 < ln <= len(src) else None
        if got != want:
            bad_cite.append((name, ln, want, got))
        ledger.setdefault((name, ln, op, frame), set()).add(w)

    unledgered, seen, needed = [], set(), set()
    for name, ln, op, frame, ctx in sites:
        if frame <= 0 or (name, ln, frame) in DRAW_ONLY:
            continue
        stops = stops_for(frame, op)
        for w, fl in floors.items():
            if ctx is not None and w not in ctx:
                continue
            key = (name, ln, op, frame)
            if any(s >= fl for s in stops):
                needed.add((w, key))
            if key in ledger and (w in ledger[key] or -1 in ledger[key]):
                seen.add(key)
            elif any(s >= fl for s in stops):
                unledgered.append((w, name, ln, op, frame))
    orphans = sorted(k for k in ledger if k not in seen)
    print("con_reload_aa.h: %d row(s), %d cite(s) differ; floors %s; %d slot/site "
          "pair(s) at or above a floor, %d with no row; %d row(s) no site backs"
          % (len(rows), len(bad_cite), floors, len(needed), len(unledgered), len(orphans)))
    for name, ln, want, got in bad_cite:
        print("   %s:%d  header %r  script %r" % (name, ln, want, got), file=sys.stderr)
    for w, name, ln, op, frame in sorted(set(unledgered)):
        print("   w%02d unledgered: %s:%d %s %d" % (w, name, ln, op, frame), file=sys.stderr)
    for name, ln, op, frame in orphans:
        print("   no site: %s:%d %s %d" % (name, ln, op, frame), file=sys.stderr)
    return 1 if bad_cite or unledgered or orphans or not rows or not floors else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-dir", default=os.environ.get("AA_GAME_DIR"),
                    required="AA_GAME_DIR" not in os.environ,
                    help="<AA game folder>, the AA install root (or set AA_GAME_DIR)")
    ap.add_argument("--out", default="src/ekmod/con_stops_aa.h",
                    help="output header path, relative to the repo root")
    ap.add_argument("--date", default="2026-09-20")
    ap.add_argument("--check-shots", action="store_true",
                    help="compare con_shots_aa.h's cited lines with the scripts "
                         "and write nothing")
    ap.add_argument("--shots", default="src/ekmod/con_shots_aa.h")
    ap.add_argument("--check-reload", action="store_true",
                    help="check con_reload_aa.h's ledger against the scripts "
                         "and write nothing")
    ap.add_argument("--reload", default="src/ekmod/con_reload_aa.h")
    args = ap.parse_args()

    game_dir = Path(args.game_dir)
    if args.check_shots:
        sd = game_dir / "scripts"
        return check_shots(sd if sd.is_dir() else game_dir, args.shots)
    if args.check_reload:
        sd = game_dir / "scripts"
        return check_reload(sd if sd.is_dir() else game_dir, args.reload)
    # `scripts/` is where AA keeps them; a --game-dir pointed straight at that
    # folder is accepted too, so the script can be run against a copy.
    files = list((game_dir / "scripts").glob("*.CON")) or list(game_dir.glob("*.CON"))
    if not files:
        print("no *.CON under %s/scripts" % game_dir, file=sys.stderr)
        return 1
    files = sorted(files, key=lambda p: p.name.upper())

    commands = collect_commands(files)
    ex = Extractor(commands)
    for p in files:
        ex.parse(p)

    # AN UNMATCHED DRAW-ONLY ENTRY IS A REFUSAL TO WRITE, NOT A WARNING. If a
    # line moved, a frame changed or a file was renamed, this table no longer
    # describes the script it was read from, and publishing anyway would either
    # restore a stop it means to drop or drop one it does not. Both are silent.
    missing = sorted(set(DRAW_ONLY) - ex.drawseen)
    if missing:
        print("DRAW_ONLY entries matched no site -- the CON has moved under "
              "them, so nothing was written:", file=sys.stderr)
        for name, ln, frame in missing:
            print("   %s:%d frame %d" % (name, ln, frame), file=sys.stderr)
        return 1

    total = ex.stats['test'] + ex.stats['write'] + ex.stats['read_or_switch']
    print("script files:                %d" % len(files))
    print("statement commands learned:  %d" % len(commands))
    print("kickback_pic references:     %d  (test %d, write %d, read/switch %d)"
          % (total, ex.stats['test'], ex.stats['write'], ex.stats['read_or_switch']))
    print("curr_weapon gates / cases:   %d / %d" % (ex.stats['gate'], ex.stats['case_gate']))
    print("WEAPONn_* gates:             %d" % ex.stats['gate_weaponvar'])
    print("negative gates / their else: %d / %d"
          % (ex.stats['gate_neg'], ex.stats['gate_else']))
    print("attributed by block weapon:  %d" % ex.stats['attr_block'])
    print("frames attributed:           %d" % ex.stats['attributed'])
    print("frames UNCERTAIN (all wpns): %d" % ex.stats['uncertain'])
    print("draw-only, no stop at all:   %d  (of %d entries)"
          % (ex.stats['draw_only'], len(DRAW_ONLY)))
    print("frame-0 references dropped:  %d" % ex.stats['frame_zero'])
    print("no literal to read:          %d" % len(ex.unresolved))
    print("gates/cases unresolved:      %d / %d"
          % (ex.stats['gate_unresolved'], ex.stats['case_unresolved']))
    print("frames by operator class:    at F %d, at F+1 %d, both kept %d"
          % (ex.stats['opclass_at_f'], ex.stats['opclass_at_f1'],
             ex.stats['opclass_both']))
    print("per weapon (frames -> stops; 'blanket' is both F-1 and F for every")
    print("frame, which is what this generator emitted before the operator was")
    print("carried to the site):")
    for w in range(MAX_WEAPONS):
        f = sorted(ex.frames[w])
        s = sorted(ex.stops[w])
        print("   w%02d  %2d frame(s) -> %2d stop(s) (blanket %2d) gap %2d  %s"
              % (w, len(f), len(s), len(frames_to_stops(f)), largest_gap(s), s))

    out_path = Path(args.out)
    text = render(ex, files, game_dir, args.date)
    # CRLF explicitly: every source file in this repo is CRLF and a plain
    # text-mode write emits LF, which rewrites the whole file's endings.
    with open(out_path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text.replace("\n", "\r\n"))
    print("wrote %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
