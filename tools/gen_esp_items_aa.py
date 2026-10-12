#!/usr/bin/env python3
# Generates src/ekmod/esp_items_aa.h -- picnum -> name for every pickup Alien
# Armageddon's OWN CON declares, plus a fingerprint of the files it was read
# from.
#
# WHY THIS EXISTS (D6). esp_items.h's table is stock Duke's, and its rule is
# that an unlisted picnum returns null and is never collected -- deliberately,
# because "a number is not a reason to call something a pickup". On AA that
# left every one of AA's own pickups unmarked while the sprite walk itself was
# fine: the mod log's `target refused:` lines show 34 distinct picnums on
# statnums 1 and 2, of which the curated table named 18. The gap was entirely
# in the table.
#
# WHAT DECIDES MEMBERSHIP. There is no engine flag for "pickup" the way
# SFLAG_PROJECTILE decides con_proj_aa.h's membership, so this is AA's own
# evidence and it takes THREE things at once, all read out of the CON:
#
#   R1  the actor's block tests `ifpdistl RETRIEVEDISTANCE` -- AA's own pickup
#       radius (AAUSER.CON:41, `define RETRIEVEDISTANCE 844`), which nothing
#       else in the scripts uses for anything else;
#   R2  it is CONSUMED as loot: it reaches one of AA's shared pickup epilogues
#       (`getcode`, `getweaponcode`, `quikweaponget`, `coinsget`, `givecoin`,
#       `collectibles`), or it inlines what those do -- `palfrom`, the pickup
#       flash, together with `killit`;
#   R3  it has NO HIT POINTS: the actor is declared with strength 0 or none.
#
# BOTH R1 AND R2 ARE NEEDED AND THE COUNTS SAY WHY. R2 alone matches 527
# actors, because a monster that flashes the screen and dies matches it too.
# R1 alone matches 128 and includes FECES (which uses the radius only to decide
# that the player STEPPED on it) and TOILETWATER (scenery you drink from, which
# is never taken). The two together leave 126, and R3 removes the last one:
# CHICKEN, `useractor notenemy CHICKEN 20`, which walks around and runs
# `state monsterai` and is only food once roasted.
#
# THE CLOSURE IS OVER STATES, AND IT HAS TO BE. Half of AA's pickups say
# nothing themselves: `useractor notenemy MINIDUKE 0 state collectibles enda`
# is the whole actor, and the range test and the epilogue both live in
# `collectibles` (AAITEMS.CON:4178). So R1 and R2 are evaluated over the actor
# together with every state it reaches.
#
# THE VERB SET WAS CHECKED AND IS NOT THE TEST. esp_items.h's own header says
# the stock set fell out of GAME.CON's `actor` blocks calling addweapon /
# addammo / addphealth / addinventory. Those four still exist in AA (20, 24, 83
# and 13 uses) and there are no `*var` forms of them at all, but they no longer
# separate anything: AA gives most ammo by writing `setp[].ammo_amount`
# directly (AAITEMS.CON:486's AMMO does), so 14 real pickups have none of the
# four -- and the four ALSO appear in APLAYER, DEATHZONE, POISONGAS, TANK,
# ORGANTIC and LILY, which are not pickups. `addstrength` is NOT a fifth
# pickup verb despite its 128 uses: gameexec.cpp:3499-3501 adds it to
# `vm.pSprite->extra`, the ACTOR's own health, not the player's.
#
# THE NAME IS AA'S OWN ENGLISH WHERE AA SAYS IT, AND THE CON SYMBOL OTHERWISE
# (D10). Every pickup arm fires `userquote <id>`, and AA's `definequote` for
# that id is the text the game itself prints -- quote 389 is "GOT EXECUTIONER!"
# for the actor the table used to call SHOCKRIFLESPRITE.
#
# ONE ACTOR CAN FIRE SEVERAL, which is the whole difficulty. AAITEMS.CON:174-175
# is `ifvarand inven_upgrades 8 userquote 2750 else userquote 6` -- "DASH BOOTS"
# once the upgrade is bought, "PROTECTIVE BOOTS" before it. So the rule, in
# order: one distinct quote reachable, use it; several, take the UNCONDITIONAL
# or `else` arm, which is the base item; still more than one, EMIT NO NAME and
# keep the symbol. The last is not a cop-out -- esp_items.h's standing rule is
# that a wrong name is worse than no name, and a code name is honest.
#
# `quote` IS NOT `userquote` AND MUST NOT BE READ AS ONE. `state collectibles`
# ends with `quote 1547`, "GOT COLLECTIBLE!" -- one shared message for all
# fifty-odd figurines. Taking it would give every one of them the same wrong
# name; they keep their symbols instead, which is why 77 rows still do.
#
# TIER A: a NARROW use of plain
# `quote` is safe once filtered to exactly the cases that made `quote 1547`
# unsafe: reachable in the pickup's own closure, reached by NO OTHER pickup
# actor anywhere in the CON, not a curated MESSAGE_QUOTES exception, and
# passing the same label-vs-message rule (quote_label) userquote naming uses.
# An earlier version claimed the label/message rule alone would reject RADAWAY's
# quote 1495 ("RADIATION POISONING CURED!", 3 words, under budget) -- measured
# false: nothing mechanical separates it from quote 190 "EXPLOSIVE SHELL KIT",
# also 3 words, which DOES name a thing. So 1495 is a curated exception, not a
# rule outcome, and it is the only one.
#
# TIER B: a symbol ending `UPGRADE` becomes `<prefix expanded> upgrade`, from
# UPGRADE_PREFIXES and nowhere else. Tier A runs FIRST and can claim an
# `*UPGRADE` symbol on its own quote -- SHOTGUNUPGRADE (quote 190) and
# TRIPBOMBUPGRADE (quote 222) both do, with AA's own player-visible words, so
# they are named by tier A and never reach tier B at all.
#
# RUN BY HAND, NOT BY build.ps1: the build must not depend on the game being
# installed. Re-run after an AA update, from the repo root:
#
#   python tools/gen_esp_items_aa.py
#
# It overwrites src/ekmod/esp_items_aa.h. Do not hand-edit that file; fix this
# script and re-run it.

import argparse
import os
import re
import sys
import textwrap
from pathlib import Path

# MAXTILES (build.h). A picnum outside this is not a tile.
MAXTILES = 30720

# AAUSER.CON:41 `define RETRIEVEDISTANCE 844`. Matched as a token, so a name
# that merely contains it does not count.
RANGE_TOKEN = "retrievedistance"

# AA's shared pickup epilogues. Each one flashes the screen and then either
# kills the sprite or hides it behind cstat bit 15 for the respawn timer:
# getcode AAITEMS.CON:8, getweaponcode :58, quikweaponget :96,
# collectibles :4178, coinsget NOTENEMY.CON:15078, givecoin NOTENEMY.CON:809.
EPILOGUE_STATES = frozenset((
    "getcode", "getweaponcode", "quikweaponget",
    "collectibles", "coinsget", "givecoin",
))

# What those epilogues DO, for the pickups that inline it instead of calling
# one (SAWSPRITE, SHIELD, RADAWAY, the weapon upgrades...).
INLINE_FLASH = "palfrom"
INLINE_CONSUME = "killit"

# Block openers and the keyword that closes each. `state` at top level is a
# DECLARATION and inside a block it is a CALL -- gamedef.cpp:2586-2590 branches
# on exactly that (`if (!g_scriptActorOffset && g_processingState == 0)`), so
# this walker does too.
ACTOR_OPENERS = ("actor", "useractor", "eventloadactor")
EVENT_OPENERS = ("onevent", "appendevent", "prependevent")
STATE_OPENERS = ("state", "defstate")

# Only these tokens are kept from a block body; everything else is dropped, so
# a block's record stays small enough to reason about.
TRACKED = frozenset((RANGE_TOKEN, INLINE_FLASH, INLINE_CONSUME))

# Tokens in AA's own quote text that stay fully upper because they are
# initialisms rather than words. Found the way gen_esp_names_aa.py found its
# own pair: by scanning every body this generator emits -- no other all-caps
# token in the 2026-09-21 install reads as an initialism.
UPPER_TOKENS = frozenset(("RPG", "BMFG"))

# A LABEL HAS A WORD BUDGET, AND THIS ONE IS ARGUED FROM BOTH TABLES. The
# longest name in esp_items.h's curated table is three words ("Night vision
# goggles"); AA's own longest that still reads as a label is four ("AMMO FOR
# CHAINGUN CANNON"). Exactly one body in this install runs longer -- quote 711,
# "HEALTH ITEM ADDED TO MEDKIT" -- and it is a sentence about what happened, not
# a name for the thing. Over budget means NO name, never a truncated one: a
# cut-off name is a wrong name. (2026-09-21)
MAX_NAME_WORDS = 4

# TIER A'S ONE CURATED EXCEPTION.
# quote 1495 `^8RADIATION POISONING CURED!` is reachable only from RADAWAY and
# passes the word budget (3 words, under MAX_NAME_WORDS), but it names the
# EFFECT of taking the item, not the item -- RadAway is not called "Radiation
# poisoning cured". No mechanical rule separates it from quote 190
# `EXPLOSIVE SHELL KIT`, which is also three words and IS a name, so this is
# curated rather than derived, and it is the only entry.
MESSAGE_QUOTES = frozenset((1495,))

# TIER B (D10 lead 2): a symbol ending in UPGRADE_SUFFIX becomes
# `<expansion> upgrade`. A prefix absent here keeps the symbol -- no invented
# expansion, no raw-prefix fallback. Bases: CHAINGUN/PISTOL/FREEZE/VENDOR are
# the plain word; the carbine code stays an initialism; DE is corroborated twice (a
# `// DESERT EAGLE` comment and `define DEAGLEBULLET 19967`); INCIN
# (Incinerator) has NO CON corroboration -- AA's weapon slot 9 is the
# Incinerator by the user's own naming, but no `define` or comment spells it
# out, so this one entry is a disclosed assumption, not a proven one.
# COUNTING ONLY, NEVER EXCLUSION (2026-09-21): these five picnums are already
# named by esp_items.h's curated table, which `ekm_item_name()` tries first,
# so a tier-A name for one of them here is inert at runtime -- never wrong,
# just never seen. Recorded so the header can state the 10-named/12-visible
# split honestly instead of a single number a reader could mistake for either
# the wrong count or an error.
TIER_A_STOCK_OVERLAP = frozenset(("COLA", "SIXPAK", "FIRSTAID", "ACCESSCARD",
                                  "ATOMICHEALTH"))

UPGRADE_SUFFIX = "UPGRADE"
UPGRADE_PREFIXES = {
    "CHAINGUN": "Chaingun",
    "PISTOL": "Pistol",
    "FREEZE": "Freeze",
    "M4": "M4",
    "VENDOR": "Vendor",
    "DE": "Desert Eagle",
    "INCIN": "Incinerator",
}


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

    THE ORDER IS PART OF THE ANSWER. CON_DEFINE IGNORES a redefinition and
    keeps the FIRST value (gamedef.cpp:2898-2906), so which file is read first
    decides what a name means. A plain glob would be a different order AND a
    different set: this install has one orphan, 'SE90 temp.con', that nothing
    includes and the engine therefore never reads. Identical to
    gen_con_proj_aa.py's walk, and the suite checks the two agree."""
    out = []
    seen = set()

    def walk(rel, fromname, fromline):
        p = Path(rel)
        if not p.exists():
            raise SystemExit("%s:%d includes %s, which does not exist"
                             % (fromname, fromline, rel))
        seen.add(str(p.resolve()).lower())
        out.append(p)
        for lineno, toks in read_lines(p):
            if toks[0].lower() == 'include' and len(toks) >= 2:
                walk(toks[1], p.name, lineno)

    walk(root, "(root)", 0)
    return out


def parse_int(tok):
    """A CON literal, or None. `int(tok, 0)` would read forms CON does not have
    and would choke on a leading 0 it does."""
    return int(tok) if re.match(r'^[+-]?\d+$', tok) else None


# READ OFF THE RAW LINE, NOT THE COMMENT-STRIPPED ONE. CON_DEFINEQUOTE
# (gamedef.cpp:5877-5905) copies every character from after the id to the end of
# the line into apStrings[k] -- it never looks for `//`, so the engine's idea of
# the text includes a trailing comment. AAUSER.CON:539 is the one line where
# that shows (`definequote 285  // leave this blank`), and 285 is not a pickup's
# quote; reading the stripped line instead would silently disagree with the
# engine somewhere it did matter.
#
# THE SEPARATOR IS `[ \t]*` AND NOT `[ \t]+`, WHICH IS NOT PEDANTRY. AA has 172
# lines that are a bare `definequote 155` with nothing after the id -- blank
# quote slots -- and requiring a separator skipped every one of them silently.
# The count this emits then read 2773 against 2945 lines that start with the
# word, a count and an enumeration of the same thing disagreeing.
# A blank quote is now READ and then refused by name, with a total of its own.
QUOTE_LINE_RE = re.compile(r'^[ \t]*definequote[ \t]+(\d+)[ \t]*(.*?)[ \t]*$')


def parse_quotes(files):
    """`definequote <id> <text>` -> {id: (text, file, line)}, LAST WINS.

    THAT IS THE ENGINE'S RULE AND IT IS THE OPPOSITE OF `define`'S.
    CON_DEFINE keeps the first value (gamedef.cpp:2898-2906) and this script
    follows it for names; CON_DEFINEQUOTE just writes apStrings[k] again
    (:5890-5891), so a second definequote for one id overwrites. AA has no
    duplicate id today -- the ones it does have are counted and reported, so a
    future update cannot make the two rules diverge quietly."""
    quotes = {}
    redefined = []
    for p in files:
        for lineno, line in enumerate(p.read_text(encoding='latin-1').splitlines(), 1):
            m = QUOTE_LINE_RE.match(line)
            if not m:
                continue
            qid = int(m.group(1))
            if qid in quotes:
                redefined.append((p.name, lineno, qid, quotes[qid][0]))
            quotes[qid] = (m.group(2), p.name, lineno)
    return quotes, redefined


def sentence_case(all_caps_text):
    """AA's caps in, esp_items.h's house style out -- "Night vision goggles",
    not "Night Vision Goggles": the curated table is sentence case and this tier
    sits beside it. The WORDS stay AA's; only the case moves. Splits on
    letter/non-letter runs so a hyphen or slash inside one token (SPIDER-MINE,
    SHRINKER/EXPANDER) does not hide the rest of the word."""
    out = []
    first = True
    for run in re.split(r'([A-Za-z]+)', all_caps_text):
        if not run.isalpha():
            out.append(run)
            continue
        if run.upper() in UPPER_TOKENS:
            out.append(run.upper())
        elif first:
            out.append(run[0].upper() + run[1:].lower())
        else:
            out.append(run.lower())
        first = False
    return "".join(out)


def quote_label(raw):
    """One quote's text as a marker label, or (None, why it is not one).

    The `^NN` runs are EDuke32's colour codes (quote 2490 is `^23PENETRATOR`)
    and are stripped, as gen_esp_names_aa.py already strips them for enemies.
    The trailing `!` is punctuation, not a word: dropping it removes nothing
    that could change WHICH item is named, which is the line this function will
    not cross in either direction."""
    body = re.sub(r'\^\d+', '', raw)
    body = re.sub(r'\s+', ' ', body).strip().rstrip('!.').strip()
    if not body:
        return None, "its quote text is empty"
    if '^' in body:
        return None, "a `^` survived the colour-code strip: %r" % raw
    # AA's quote is a pickup MESSAGE, not a label -- no item is CALLED
    # "Got X". Whole-word match only: `GOTHIC RELIC` keeps every letter,
    # and only the LEADING occurrence is the prefix. sentence_case() below
    # then re-capitalises whatever word was behind it, the same as any
    # name that never carried the prefix. (2026-09-21)
    #
    # IT RUNS BEFORE THE WORD COUNT ON PURPOSE. The budget measures the
    # NAME, and "GOT" is not part of one -- counting it would reject a
    # four-word name for a word we are about to remove. The other order is
    # defensible and it is not the one we want, so do not "tidy" this line
    # below the check. What the corpus actually holds today: the longest
    # GOT-prefixed quote is "GOT RESCUE SPRAY REFILL!", four words before
    # the strip, so nothing crosses the budget either way and the 27/18/80
    # split is identical under both orders. That is a fact about this
    # install, not a reason the order does not matter.
    body = re.sub(r'^GOT\s+', '', body)
    words = body.split()
    if len(words) > MAX_NAME_WORDS:
        return None, ("its quote is %d words (%r) -- a message, not a label"
                      % (len(words), body))
    return sentence_case(body), None


def parse_names_h(path):
    """`#define NAME picnum` -> {NAME: picnum}. The canonical tile list, and
    the same file tilenames.cpp parses at runtime."""
    names = {}
    for line in path.read_text(encoding='latin-1').splitlines():
        m = re.match(r'#define\s+(\S+)\s+(-?\d+)\s*$', line.strip())
        if m:
            names[m.group(1)] = int(m.group(2))
    return names


# ---------------------------------------------------------------------------
# WHICH ARM A `userquote` SITS ON, DECIDED FROM THE LINE IT IS WRITTEN ON.
#
# THE WHOLE-BLOCK ANSWER IS THE WRONG ONE, and that is not a shortcut. EVERY
# pickup quote is inside `ifpdistl RETRIEVEDISTANCE ... { }` -- being near the
# item is the condition for taking it -- so an honest nesting analysis calls all
# 111 of them conditional and resolves nothing. What rule 2 has to separate is
# narrower: the if/else CHAIN that picks between several quotes for one actor,
# which AA writes one arm per line:
#
#     ife pchar 4 userquote 1270 else        <- a variant
#     ife pchar 3 userquote 1378 else        <- a variant
#     userquote 66                           <- the base item
#
# So: an `if`/`case` earlier ON THE SAME LINE means a variant, and an `else` or
# `default` immediately before it -- same line, or the whole of the line above
# -- means the base arm. A `switch` arm carries across lines instead, because
# `case`, `default`, `break` and `endswitch` delimit it exactly (SHIELD,
# AAITEMS.CON:247-259, is the one pickup that needs this).
#
# IT IS A HEURISTIC AND ITS FAILURES ARE SAFE ONES. Every way of misreading a
# line either leaves no base arm or leaves two, and both land on rule 3, which
# emits no name. It can only produce a WRONG name if the true base were hidden
# AND a false base invented in the same actor; no shape in this install does
# that, and each of the 18 rows rule 2 decides was read against its own CON
# lines. (2026-09-21)
def quote_arm(low, i, prev_last, switch_arm):
    """'cond' (a variant), 'base' (an else/default arm) or 'plain'."""
    pre = low[:i]
    if (switch_arm == 'case' or 'case' in pre
            or any(t.startswith('if') for t in pre)):
        return 'cond'
    if ('else' in pre or 'default' in pre or switch_arm == 'default'
            or prev_last in ('else', 'default')):
        return 'base'
    return 'plain'


class Block:
    __slots__ = ("name", "kind", "file", "line", "strength", "toks", "calls",
                 "quotes", "plain_quotes")

    def __init__(self, name, kind, file, line, strength):
        self.name = name
        self.kind = kind
        self.file = file
        self.line = line
        self.strength = strength
        self.toks = set()
        self.calls = set()
        self.quotes = []   # (lineno, quote id, arm) for every `userquote` here
        self.plain_quotes = []  # (lineno, quote id) for every plain `quote`
                                 # here -- D10 lead 2's tier A evidence, kept
                                 # separate from `quotes` because `quote` and
                                 # `userquote` are different opcodes and must
                                 # never be read as one (see this file's
                                 # header comment).


class Scan:
    def __init__(self):
        self.defines = {}        # name -> value, FIRST wins
        self.ignored_redef = []  # (file, line, name, new, kept)
        self.actors = {}         # name -> Block
        self.states = {}         # name -> Block
        self.dup_actors = []     # (name, kept_file, kept_line, file, line)
        self.stray_end = []      # (file, line, token) -- a close with no open
        self.odd_quote = []      # (file, line, token) -- a non-literal quote id
        self.unclosed = None
        self.files = []

    # -- pass 1 ------------------------------------------------------------
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

    # -- pass 2 ------------------------------------------------------------
    def walk_blocks(self, p):
        cur = None
        prev_last = None    # last token of the previous non-blank line
        switch_arm = None   # 'case' / 'default' / None, the switch arm in flight
        for lineno, toks in read_lines(p):
            low = [t.lower() for t in toks]
            i = 0
            n = len(toks)
            while i < n:
                t = low[i]
                if cur is None:
                    if t in ACTOR_OPENERS and i + 1 < n:
                        j = i + 2 if t == 'useractor' else i + 1
                        if j >= n:
                            i = n
                            continue
                        nm = toks[j]
                        st = toks[j + 1] if j + 1 < n else None
                        kind = 'load' if t == 'eventloadactor' else 'actor'
                        cur = Block(nm, kind, p.name, lineno, st)
                        switch_arm = None
                        i = j + 1
                        continue
                    if t in EVENT_OPENERS and i + 1 < n:
                        cur = Block(toks[i + 1], 'event', p.name, lineno, None)
                        switch_arm = None
                        i += 2
                        continue
                    if t in STATE_OPENERS and i + 1 < n:
                        cur = Block(toks[i + 1], 'state', p.name, lineno, None)
                        switch_arm = None
                        i += 2
                        continue
                    i += 1
                    continue

                # inside a block
                if t == 'enda' and cur.kind in ('actor', 'load'):
                    if cur.kind == 'actor':
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
                if t in ('switch', 'break', 'endswitch'):
                    switch_arm = None
                elif t == 'case':
                    switch_arm = 'case'
                elif t == 'default':
                    switch_arm = 'default'
                if t == 'state' and i + 1 < n:
                    cur.calls.add(toks[i + 1])
                    i += 2
                    continue
                if t == 'userquote' and i + 1 < n:
                    qid = parse_int(toks[i + 1])
                    if qid is None:
                        # `userquotevar` does not exist and AA has no symbolic
                        # quote id, so this would be a shape nothing here can
                        # resolve. Recorded so it cannot pass as "no quote".
                        self.odd_quote.append((p.name, lineno, toks[i + 1]))
                    else:
                        cur.quotes.append((lineno, qid,
                                           quote_arm(low, i, prev_last,
                                                     switch_arm)))
                if t == 'quote' and i + 1 < n:
                    qid = parse_int(toks[i + 1])
                    if qid is not None:
                        cur.plain_quotes.append((lineno, qid))
                if t in TRACKED:
                    cur.toks.add(t)
                i += 1
            prev_last = low[-1]
        if cur is not None:
            self.unclosed = (p.name, cur.line, cur.kind, cur.name)

    def close_actor(self, blk):
        prev = self.actors.get(blk.name)
        if prev is not None:
            # AA declares each actor once today, except for the duplicates
            # enumerated in the header. THE ENGINE DOES NOT ERROR ON ONE:
            # CON_ACTOR (gamedef.cpp:3195-3263) silently overwrites execPtr,
            # so the engine's rule is LAST WINS, not first and not a refusal.
            # (An earlier comment here named an ERROR_DUPLICATEDEFINITION that
            # does not exist in gamedef.cpp.)
            # We keep the FIRST and record the rest. That differs from the
            # engine, and it is safe only because every duplicate AA actually
            # has is a non-pickup either way: re-run under last-wins and the
            # 125 rows are identical. If that ever stops being true, follow
            # the engine rather than this convenience.
            self.dup_actors.append((blk.name, prev.file, prev.line,
                                    blk.file, blk.line))
            return
        self.actors[blk.name] = blk

    # -- closure -----------------------------------------------------------
    def closure(self, blk):
        """(tokens, states reached) for a block plus every state it calls,
        transitively. A state that is called but never declared is simply
        absent, which is what the engine would report as an error anyway."""
        toks = set(blk.toks)
        reached = set()
        stack = list(blk.calls)
        while stack:
            nm = stack.pop()
            if nm in reached:
                continue
            reached.add(nm)
            s = self.states.get(nm)
            if s is None:
                continue
            toks |= s.toks
            stack.extend(s.calls)
        return toks, reached

    def quotes_of(self, blk):
        """Every `userquote` over the SAME actor+state closure the membership
        rules use, in source order -- (file, line, quote id, arm). Half of AA's
        pickups say nothing in their own block, so the closure is where the
        quote lives too, not only the range test and the epilogue."""
        _, reached = self.closure(blk)
        out = [(blk.file, ln, qid, arm) for ln, qid, arm in blk.quotes]
        for nm in sorted(reached):
            s = self.states.get(nm)
            if s is None:
                continue
            out.extend((s.file, ln, qid, arm) for ln, qid, arm in s.quotes)
        return out

    def plain_quotes_of(self, blk):
        """Every plain `quote` (not `userquote`) over the same actor+state
        closure, in source order -- (file, line, quote id). D10 lead 2's tier
        A evidence (brief-d10-lead2-v2, 2026-09-21)."""
        _, reached = self.closure(blk)
        out = [(blk.file, ln, qid) for ln, qid in blk.plain_quotes]
        for nm in sorted(reached):
            s = self.states.get(nm)
            if s is None:
                continue
            out.extend((s.file, ln, qid) for ln, qid in s.plain_quotes)
        return out


# ---------------------------------------------------------------------------
class Verdict:
    def __init__(self, blk, r1, r2, r3, why3):
        self.blk = blk
        self.r1 = r1
        self.r2 = r2
        self.r3 = r3
        self.why3 = why3

    @property
    def pickup(self):
        return self.r1 and self.r2 and self.r3


def judge(scan, names_h):
    """Verdict per actor, plus the picnum each name resolves to."""
    out = {}
    for name, blk in scan.actors.items():
        toks, reached = scan.closure(blk)
        r1 = RANGE_TOKEN in toks
        r2 = bool(reached & EPILOGUE_STATES) or (
            INLINE_FLASH in toks and INLINE_CONSUME in toks)
        r3, why3 = strength_is_zero(blk.strength, scan.defines, names_h)
        out[name] = Verdict(blk, r1, r2, r3, why3)
    return out


class Naming:
    """How one row got its name. `rule` is 1 (one quote reachable), 2 (several,
    and one base arm among them) or 0 (none): 0 keeps the CON symbol and `why`
    says what stopped it, which is what the header enumerates."""

    def __init__(self, name, rule, quote_id, why, candidates):
        self.name = name
        self.rule = rule
        self.quote_id = quote_id
        self.why = why
        self.candidates = candidates


# WHAT ACTUALLY STOPPED THE NAME, where the mechanical answer is true and
# MISLEADING. `why` is derived from the quote walk alone, and for three rows
# that derivation reads as a failure when nothing failed:
#
#   RAILGUNSPRITE -- "3 quotes and 2 of them on a base arm" is a misreading of a
#     NESTED `else` chain. Every arm of `state pickup_railgun` is exactly
#     determined by live state, and esp_items_runtime_aa.h now resolves it.
#   SMALLSTACK / BIGSTACK -- listed under "no `userquote` is reachable" beside
#     154 others, which is true and misleading: they are money and fire no quote
#     BY DESIGN. There was never any text to harvest.
#
# An override rather than a widened rule: the rule is right, and a rule bent to
# accommodate three rows would start naming things it should not.
WHY_OVERRIDES = {
    "RAILGUNSPRITE":
        "its 3 quotes sit on a NESTED `else` chain that live state decides "
        "exactly (AAITEMS.CON:3809-3819, on `pchar` and "
        "`<char>upgrades[DEVISTATOR_WEAPON]`), so the name is resolved at "
        "runtime by esp_items_runtime_aa.h and this symbol is only the "
        "fallback for a failed read",
    "SMALLSTACK":
        "it is MONEY and fires no `userquote` by design -- named and priced "
        "from its palette by esp_items_runtime_aa.h "
        "(`state coinsget`, NOTENEMY.CON:15078)",
    "BIGSTACK":
        "it is MONEY and fires no `userquote` by design -- named and priced "
        "from its palette, at three times SMALLSTACK's value "
        "(`ifactor BIGSTACK mul B 3`, NOTENEMY.CON:15088)",
}


def name_row(occurrences, quotes):
    """The label for one pickup, by the rule the header states."""
    seen = []
    for _f, _l, qid, _arm in occurrences:
        if qid not in seen:
            seen.append(qid)
    if not seen:
        return Naming(None, 0, None, "no `userquote` is reachable", [])

    if len(seen) == 1:
        rule, chosen = 1, seen[0]
    else:
        base = sorted({qid for _f, _l, qid, arm in occurrences if arm != 'cond'})
        if len(base) != 1:
            return Naming(None, 0, None,
                          "%d quotes and %d of them on a base arm, so none is "
                          "THE base" % (len(seen), len(base)), sorted(seen))
        rule, chosen = 2, base[0]

    entry = quotes.get(chosen)
    if entry is None:
        return Naming(None, 0, chosen,
                      "quote %d has no `definequote`" % chosen, sorted(seen))
    label, why = quote_label(entry[0])
    if label is None:
        return Naming(None, 0, chosen, why, sorted(seen))
    return Naming(label, rule, chosen, None, sorted(seen))


def resolve_tier_a(pq_ids, owners, quotes):
    """TIER A (D10 lead 2): a plain `quote`, accepted only when exactly one is
    reachable in the pickup's own closure, no OTHER pickup actor anywhere
    reaches the same id, it is not the curated MESSAGE_QUOTES exception, and
    it passes the same label-vs-message rule (quote_label) userquote naming
    already uses. Returns (name_or_None, quote_id_or_None, reason_or_None) --
    the reason is only for the generated comment on a row that keeps its
    symbol, exactly as `Naming.why` is for the userquote tiers."""
    if not pq_ids:
        return None, None, "no plain `quote` is reachable"
    if len(pq_ids) > 1:
        return None, None, ("%d distinct plain quotes reachable, none is "
                             "THE one" % len(pq_ids))
    qid = pq_ids[0]
    if owners.get(qid, 0) > 1:
        return None, None, ("quote %d is also reached by another pickup "
                             "actor -- a shared id names neither" % qid)
    if qid in MESSAGE_QUOTES:
        return None, None, ("quote %d is the curated MESSAGE_QUOTES "
                             "exception -- see this file's header" % qid)
    entry = quotes.get(qid)
    if entry is None:
        return None, None, "quote %d has no `definequote`" % qid
    label, why = quote_label(entry[0])
    if label is None:
        return None, None, "quote %d: %s" % (qid, why)
    return label, qid, None


def resolve_tier_b(symbol):
    """TIER B (D10 lead 2): a symbol ending `UPGRADE_SUFFIX` becomes
    `<expansion> upgrade`, the expansion taken from UPGRADE_PREFIXES and
    nowhere else. Returns (name_or_None, reason_or_None)."""
    if not symbol.endswith(UPGRADE_SUFFIX):
        return None, "no `%s` suffix" % UPGRADE_SUFFIX
    prefix = symbol[:-len(UPGRADE_SUFFIX)]
    expansion = UPGRADE_PREFIXES.get(prefix)
    if expansion is None:
        return None, "prefix %r is not in the UPGRADE table" % prefix
    return "%s upgrade" % expansion, None


def strength_is_zero(tok, defines, names_h):
    """CON_ACTOR's optional strength. Absent means zero, and a token that is
    not a number and not a define is not a strength at all -- it is the next
    command, which C_ParseCommand stops on."""
    if tok is None:
        return True, "absent"
    lit = parse_int(tok)
    if lit is not None:
        return lit == 0, str(lit)
    if tok in names_h:
        return names_h[tok] == 0, "%s = %d" % (tok, names_h[tok])
    if tok in defines:
        return defines[tok] == 0, "%s = %d" % (tok, defines[tok])
    return True, "'%s' is not a number, so no strength was given" % tok


# D10. 51 of the 125 rows are `MINI*`
# collectible figurines (picnums 16010..16060) and all 51 share one pickup
# message -- `quote 1547` via `state collectibles` -- so name_row() above
# always lands them on rule 0 with "no `userquote` is reachable". A figurine
# OF a monster is named by that monster, and this repo already holds AA's own
# name for it: esp_names_aa.h's nameplate table (tools/gen_esp_names_aa.py).
MINI_PREFIX = "MINI"


def load_nameplate_table(game_dir):
    """{picnum: name} from esp_names_aa.h's OWN generator, run fresh against
    the same install rather than re-parsed from the generated header --
    one source of truth instead of two readings that could drift apart.
    Only the base table is used (no pal/mtype/team override): a figurine has
    no palette of its own to carry one."""
    here = Path(__file__).resolve().parent
    if str(here) not in sys.path:
        sys.path.insert(0, str(here))
    import gen_esp_names_aa
    table, _overrides, _civilian, _report = gen_esp_names_aa.build_table(game_dir)
    return table


# D10 LEAD 4. Three figurines whose named
# candidates disagree were put to the user anyway, because two of the three
# readings duplicate a row this table already has. The user chose to refuse
# all three explicitly on 2026-09-21 -- recorded here so a later, looser
# agreement rule does not quietly overturn a decision a human actually made.
# THE ABSENCE IS IN OUR TABLE, NOT IN AA -- which is a different fact from
# the other 13 no-candidate rows and the header should say so rather than
# making a reader cross-reference the status doc to learn it. Both have a
# real actor in names.h; neither is in esp_names_aa.h's nameplate table.
# MINIDUKE resolves ONLY because DUKEBOT happens to be nameplated, so these
# two are one enemy-naming pass away from naming themselves.
# (review-d10-lead4, 2026-09-21)
OUR_TABLE_MISSES = {
    "MINIWES": (
        "and the absence is in OUR nameplate table, not in AA: WESBOT is a "
        "real actor in names.h, it is simply not in esp_names_aa.h. "
        "MINIDUKE resolves only because DUKEBOT is nameplated. AA6 (ESP "
        "names for AA's enemy tiles) would name this row for free"),
    "MINIGRAVES": (
        "and the absence is in OUR nameplate table, not in AA: GRAVES is a "
        "real actor in names.h, it is simply not in esp_names_aa.h. AA6 "
        "would name this row for free"),
}

USER_REFUSED_FIGURINES = {
    "MINIPIG": (
        "put to the user and REFUSED on 2026-09-21. Candidates disagree: "
        "Pigcop (PIGCOP), Swinelord (PIGBOSS), Armored Pig (ARMPIG). "
        "MINIPIGBOSS (16041) is already \"Swinelord figurine\" and "
        "MINIARMPIG (16024) is already \"Armored Pig figurine\", so two of "
        "the three readings would have duplicated an existing row"),
    "MINIMECH": (
        "put to the user and REFUSED on 2026-09-21. Candidates disagree: "
        "Mechbrain (MECHBRAIN), Big Mech (NEWMECH)"),
    "MINIBEAST": (
        "put to the user and REFUSED on 2026-09-21. Candidates disagree: "
        "Drone (NEWBEAST), Cyberbeast (CYBERBEAST) -- the latter would have "
        "duplicated MINICYBER (16044)"),
}


def figurine_candidates(remainder, names_h, nameplate):
    """Every names.h symbol CONTAINING `remainder`, excluding any MINI*
    itself (or a figurine would match itself), paired with the nameplate
    name its picnum carries where it carries one.

    names.h ONLY -- never a CON `define`. AADEFS.CON's own `define` keyword
    also names sound ids and other non-tile constants, and NEWSOUNDS.CON's
    `define BULLPAIN1 1550` numerically collides with SHARK's own picnum
    1550: widening this search to scan.defines silently turns that sound id
    into a "candidate" figurine name (measured 2026-09-21, that is exactly
    how MINIBULL first came out disagreeing "Cosmotaur"/"Shark"). names.h
    has no second namespace, so it alone is safe to search by substring."""
    out = []
    for sym in sorted(names_h):
        if sym.startswith(MINI_PREFIX) or remainder not in sym:
            continue
        name = nameplate.get(names_h[sym])
        if name is not None:
            out.append((sym, names_h[sym], name))
    return out


def resolve_figurine(symbol, names_h, defines, nameplate):
    """One `MINI*` row's figurine name, or why it keeps its symbol.

    Returns (name_or_None, note, kind). `kind` is 'resolved', 'disagree' or
    'no_candidate' so the header can enumerate the two failure shapes
    separately, as the brief requires.

    STEP 1 (EXACT, and it still wins outright): strip `MINI`, then resolve
    the remainder to a base picnum through names.h (PRIMARY) then a CON
    `define` (SECONDARY, inert today -- see the D10 review note this
    replaced). If that EXACT symbol is itself nameplated, take its name and
    stop -- WITHOUT widening. This is what keeps MINISHARK naming itself
    after SHARK and not after ROBOSHARK, which also contains the substring
    "SHARK" but names a different enemy; widening unconditionally would
    regress every one of the 14 figurines this step already named exactly.

    STEP 2 (WIDENED, D10 LEAD 4, brief-d10-lead4, 2026-09-21): the exact
    remainder is not itself a named figure -- either no define matches it at
    all, or one does and the nameplate table does not name that picnum
    (MINIGRAVES -> GRAVES = picnum 8285, unnamed). Search every OTHER
    names.h symbol CONTAINING the remainder (figurine_candidates) -- a
    family of actor states, `TERMINATOR`/`TERMINATORDUCKING`/
    `TERMINATORCRAWLING`, or a compound name, `ARMYANT` for the remainder
    `ANT` -- and name the figurine ONLY if every candidate that IS
    nameplated agrees on ONE name. Candidates with no nameplate entry at all
    (props, gibs, sounds) are silently excluded rather than counted as
    disagreement -- MINIDUKE resolves only because DUKEBOT is nameplated;
    DUKEICON, DUKECAR and the rest of the DUKE* scenery symbols simply have
    no entry to disagree with."""
    remainder = symbol[len(MINI_PREFIX):]

    pic, src = None, None
    if remainder in names_h:
        pic, src = names_h[remainder], "names.h"
    elif remainder in defines:
        pic, src = defines[remainder], "CON define"
    if pic is not None:
        exact_name = nameplate.get(pic)
        if exact_name is not None:
            return ("%s figurine" % exact_name,
                    "figurine of %s -- remainder %r is itself the exact "
                    "picnum %d (%s), named by AA's own nameplate table "
                    "(esp_names_aa.h)" % (exact_name, remainder, pic, src),
                    "resolved")

    candidates = figurine_candidates(remainder, names_h, nameplate)
    distinct = sorted(set(name for _s, _p, name in candidates))
    if len(distinct) == 1:
        name = distinct[0]
        cand_desc = ", ".join(s for s, _p, _n in candidates)
        return ("%s figurine" % name,
                "figurine of %s -- remainder %r agrees across every "
                "nameplated names.h symbol containing it (%s)"
                % (name, remainder, cand_desc),
                "resolved")
    if len(distinct) > 1:
        cand_desc = ", ".join("%s=%s" % (s, n) for s, _p, n in candidates)
        note = ("its remainder %r has nameplated candidates that disagree "
                "(%s)" % (remainder, cand_desc))
        if symbol in USER_REFUSED_FIGURINES:
            note += "; %s" % USER_REFUSED_FIGURINES[symbol]
        return (None, note, "disagree")
    if pic is not None:
        note = ("its remainder %r resolves to picnum %d (%s), which AA's "
                "own nameplate table does not name, and no OTHER names.h "
                "symbol containing it is nameplated either"
                % (remainder, pic, src))
        if symbol in OUR_TABLE_MISSES:
            note += " -- %s" % OUR_TABLE_MISSES[symbol]
        return (None, note, "no_candidate")
    note = ("its remainder %r has no exact `define` anywhere (checked "
            "names.h, then a CON `define`), and no names.h symbol "
            "containing it is nameplated either" % remainder)
    if symbol in OUR_TABLE_MISSES:
        note += " -- %s" % OUR_TABLE_MISSES[symbol]
    return (None, note, "no_candidate")


def resolve(name, names_h, defines):
    """(picnum, source). names.h is PRIMARY -- it is the canonical tile list
    and the file tilenames.cpp already parses. A CON `define` is consulted only
    for a name names.h does not carry at all (AA has a handful: RADARSPRITE,
    RADAWAY, TOKYOJIHEN, MINICYBER, JETPACKUPGRADE). A literal-named actor
    resolves to itself."""
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

// GENERATED by tools/gen_esp_items_aa.py -- DO NOT HAND-EDIT. Second tier of the Items ESP (esp_items.h's curated stock
// table answers first); a picnum absent from BOTH returns null, so scenery is never labelled as loot. A pickup tests
// `ifpdistl RETRIEVEDISTANCE` (844), reaches a shared pickup epilogue and has no hit points.

"""

TOTALS = ("Walk totals: %(file_count)d CON files, %(actor_count)d actor and %(state_count)d state blocks; "
          "%(r12_count)d actors match R1+R2, %(row_count)d rows emitted (rule 1: %(rule1_count)d, "
          "rule 2: %(rule2_count)d, rule 3 MINI* figurines: %(rule3_count)d, rule 4: %(rule4_count)d, "
          "rule 5: %(rule5_count)d; %(rule0_count)d keep the CON symbol; %(visible_named_count)d names change on "
          "screen). A wrong name is worse than none, so an ambiguous row keeps its symbol.")


def cpp_name(text):
    return text.replace("\\", "\\\\").replace('"', '\\"')


def render(scan, rows, verdicts, unresolved, oob, collide, quote_redef, root,
           game_dir, date, counts, mini_no_candidate, mini_disagree):
    out = [PROLOGUE]
    out.append(textwrap.fill(TOTALS % counts, width=120, initial_indent="// ", subsequent_indent="// ") + "\n")

    out.append("""
// Emitted, not counted at runtime, so the suite can check it against the enumeration.
const int EKM_ITEM_AA_COUNT = %d;

// Per-rule counts, so a rule that quietly stopped firing shows (the suite counts rows by `rule`).
const int EKM_ITEM_AA_NAMED_R1       = %d;   // one quote reachable
const int EKM_ITEM_AA_NAMED_R2       = %d;   // several, one base arm among them
const int EKM_ITEM_AA_NAMED_FIGURINE = %d;   // MINI* named from the nameplate table
const int EKM_ITEM_AA_NAMED_QUOTE    = %d;   // a plain `quote`, strictly filtered
const int EKM_ITEM_AA_NAMED_UPGRADE  = %d;   // `*UPGRADE` symbol, de-camelled
const int EKM_ITEM_AA_UNRESOLVED     = %d;   // none: the row keeps its CON symbol

// One row. `name` is never null or empty. `rule`: 1 or 2 (a `userquote`), 3 (a MINI* figurine named from
// esp_names_aa.h's nameplates), 4 (a plain `quote`, strictly filtered), 5 (a `*UPGRADE` symbol de-camelled), or 0
// (`name` is the CON's symbol).
struct EkItemAaRow { short picnum; const char* name; unsigned char rule; };

// Sorted by picnum. Function-local so including translation units do not each carry a copy.
inline const EkItemAaRow* ekm_item_aa_rows(void) {
  static const EkItemAaRow kRows[EKM_ITEM_AA_COUNT] = {
""" % (len(rows), counts['rule1_count'], counts['rule2_count'],
       counts['rule3_count'], counts['rule4_count'], counts['rule5_count'],
       counts['rule0_count']))

    for pic, name, src, rule, symbol, why, qid in rows:
        if rule in (1, 2, 4):
            note = "%s, quote %d" % (symbol, qid)
            if rule == 4 and symbol in TIER_A_STOCK_OVERLAP:
                note += " (curated table wins)"
        else:
            note = symbol
        out.append('    { %5d, %-28s %d },   // %s\n'
                   % (pic, '"%s",' % cpp_name(name), rule, note))

    out.append("""  };
  return kRows;
}

// The i-th row, or a miss shaped like a picnum miss.
struct EkItemAa { bool found; int picnum; const char* name; };

inline EkItemAa ekm_item_aa_at(int index) {
  EkItemAa r;
  r.found = false;
  r.picnum = -1;
  r.name = "";
  if (index < 0 || index >= EKM_ITEM_AA_COUNT) return r;
  const EkItemAaRow* const row = ekm_item_aa_rows() + index;
  r.found = true;
  r.picnum = (int)row->picnum;
  r.name = row->name;
  return r;
}

// AA's name for a picnum, or null. Binary search over a sorted table: picnums are sparse (%d..%d over %d rows).
inline const char* ekm_item_name_aa(int picnum) {
  const EkItemAaRow* const rows = ekm_item_aa_rows();
  int lo = 0, hi = EKM_ITEM_AA_COUNT - 1;
  while (lo <= hi) {
    const int mid = lo + ((hi - lo) >> 1);
    const int at = (int)rows[mid].picnum;
    if (at == picnum) return rows[mid].name;
    if (at < picnum) lo = mid + 1;
    else             hi = mid - 1;
  }
  return 0;
}
""" % (rows[0][0], rows[-1][0], len(rows)))

    out.append("""
// The files read, in the engine's INCLUDE order, with sizes: a frozen reading of one install. The suite checks them
// against con_stops_aa.h; constops.cpp compares at runtime. Size, never mtime.
struct EkItemAaFile { const char* name; long size; };
""")
    out.append("const int EKM_ITEM_AA_FILE_COUNT = %d;\n\n" % len(scan.files))
    out.append("inline const EkItemAaFile* ekm_item_aa_files(void) {\n"
               "  static const EkItemAaFile kFiles[EKM_ITEM_AA_FILE_COUNT] = {\n")
    out.append(",\n".join('    { "%s", %d }' % (p.name, p.stat().st_size)
                          for p in scan.files))
    out.append("\n  };\n  return kFiles;\n}\n")
    header_parts = len(out)

    # THE NEAR MISSES, as comments. Each is a thing a future AA update could
    # move, and a reader who cannot see them has to re-run the generator to
    # know which rule did the work.
    #
    # THESE LISTS ARE NOT THE WHOLE STORY AND MUST NOT BE READ AS ONE. R1 is
    # a TOKEN test for `ifpdistl RETRIEVEDISTANCE`, so a pickup that spells
    # its own range as a literal is missed before it can be a near miss and
    # appears nowhere below. Review found six on 2026-09-21: DATACRYSTAL
    # (WARMAGEDDON.CON:2075, `ifpdistl 1280`) and BOMBPART1..5
    # (AAITEMS.CON:4548-4552, via `state mcguffincode`). Missing a pickup is
    # the safe direction -- it draws nothing rather than mislabelling
    # scenery -- so they are recorded here and not chased by widening R1,
    # which would take the literal in `ifpdistl 1280` as a licence.
    # THE ROWS THAT KEPT THEIR SYMBOL, with a total beside the enumeration
    # A reader who sees a code name on a marker can find out here
    # WHY it is one, without re-running anything.
    out.append("\n// ROWS STILL CARRYING THE CON'S SYMBOL, and what stopped a "
               "name: %d\n" % counts['rule0_count'])
    quiet = sorted((r[4], r[5]) for r in rows if r[3] == 0)
    for sym, why in quiet:
        out.append("//   %s -- %s\n" % (sym, why))

    # D10 FIGURINE TIER, its own enumeration beside its own total.
    # A row here is a SUBSET of the "still carrying the symbol" list above --
    # every MINI* failure appears in both -- but grouped so a reader does not
    # have to grep the general list for the prefix, and split by WHICH step
    # failed: no names.h symbol containing the remainder is nameplated at
    # all, versus several are and they disagree. 
    # widened to candidate agreement)
    out.append("\n// MINI* FIGURINES THAT KEPT THEIR SYMBOL, split by which "
               "step failed: %d\n"
               % (counts['mini_no_candidate'] + counts['mini_disagree']))
    out.append("//   no named candidate anywhere in names.h (%d):\n"
               % counts['mini_no_candidate'])
    for sym, why in sorted(mini_no_candidate):
        out.append("//     %s -- %s\n" % (sym, why))
    out.append("//   named candidates disagree (%d):\n"
               % counts['mini_disagree'])
    for sym, why in sorted(mini_disagree):
        out.append("//     %s -- %s\n" % (sym, why))

    out.append("\n// MATCHED R1 AND R2 BUT NOT R3 -- it has hit points, so it "
               "is an actor and\n// not loot:\n")
    near3 = sorted((v.blk.name, v.blk.file, v.blk.line, v.why3)
                   for v in verdicts.values() if v.r1 and v.r2 and not v.r3)
    if near3:
        for nm, fn, ln, why in near3:
            out.append("//   %s (%s:%d) -- strength %s\n" % (nm, fn, ln, why))
    else:
        out.append("//   (none)\n")

    out.append("\n// MATCHED R1 BUT NOT R2 -- it uses the pickup radius for "
               "something that is\n// not a pickup:\n")
    near2 = sorted((v.blk.name, v.blk.file, v.blk.line)
                   for v in verdicts.values() if v.r1 and not v.r2)
    if near2:
        for nm, fn, ln in near2:
            out.append("//   %s (%s:%d)\n" % (nm, fn, ln))
    else:
        out.append("//   (none)\n")

    out.append("\n// `define` REDEFINITIONS THE ENGINE IGNORED (first value kept):\n")
    if scan.ignored_redef:
        for fn, ln, name, new, kept in scan.ignored_redef:
            out.append("//   %s:%d  %s -> %d ignored, %d kept\n"
                       % (fn, ln, name, new, kept))
    else:
        out.append("//   (none)\n")

    # CON_DEFINEQUOTE OVERWRITES where CON_DEFINE keeps the first value, so a
    # quote id written twice is a real difference in how the two are read, not a
    # curiosity. AA has none today; the list is here so a future one is loud.
    out.append("\n// `definequote` IDS WRITTEN TWICE (the engine keeps the "
               "LAST): %d\n" % counts['quote_redef_count'])
    if quote_redef:
        for fn, ln, qid, was in quote_redef:
            out.append("//   %s:%d  quote %d replaces %r\n" % (fn, ln, qid, was))
    else:
        out.append("//   (none)\n")

    out.append("\n// DROPPED, and why -- an unresolvable name, a picnum outside\n"
               "// 0..MAXTILES, two pickups on one picnum, or a duplicate actor:\n")
    if unresolved or oob or collide or scan.dup_actors:
        for name in unresolved:
            out.append("//   %s -- no names.h entry, no `define`, not a literal\n"
                       % name)
        for name, pic in oob:
            out.append("//   %s -- picnum %d is outside 0..%d\n"
                       % (name, pic, MAXTILES - 1))
        for pic, first, second in collide:
            out.append("//   %s -- picnum %d already taken by %s\n"
                       % (second, pic, first))
        for name, kf, kl, f, l in scan.dup_actors:
            out.append("//   %s -- declared again at %s:%d, %s:%d kept\n"
                       % (name, f, l, kf, kl))
    else:
        out.append("//   (none)\n")

    # The committed header carries none of the enumerations below; they go to stdout instead.
    return "".join(out[:header_parts]) + "\n", "".join(out[header_parts:])


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
                    help="the tile-name header, relative to --game-dir")
    ap.add_argument("--out", default="src/ekmod/esp_items_aa.h",
                    help="output header path, relative to the repo root")
    ap.add_argument("--date", default="2026-09-21")
    args = ap.parse_args()

    repo = Path.cwd()
    game_dir = Path(args.game_dir)
    root = game_dir / args.script
    if not root.exists():
        print("no root script at %s" % root, file=sys.stderr)
        return 1
    names_path = game_dir / args.names
    if not names_path.exists():
        print("no names header at %s" % names_path, file=sys.stderr)
        return 1

    names_h = parse_names_h(names_path)
    out_path = (repo / args.out).resolve()

    # The include paths in the CON are relative to the game folder, so the walk
    # runs with that as the working directory.
    os.chdir(str(game_dir))
    files = include_order(args.script)

    scan = Scan()
    scan.run(files)

    if scan.stray_end:
        for fn, ln, tok, kind, nm in scan.stray_end[:10]:
            print("%s:%d: '%s' closes a %s block (%s) -- the block walk is "
                  "wrong" % (fn, ln, tok, kind, nm), file=sys.stderr)
        print("nothing written", file=sys.stderr)
        return 1
    if scan.unclosed is not None:
        print("%s: %s block '%s' opened at line %d and never closed -- nothing "
              "written" % (scan.unclosed[0], scan.unclosed[2], scan.unclosed[3],
                           scan.unclosed[1]), file=sys.stderr)
        return 1

    if scan.odd_quote:
        for fn, ln, tok in scan.odd_quote[:10]:
            print("%s:%d: `userquote %s` is not a literal id -- nothing written"
                  % (fn, ln, tok), file=sys.stderr)
        return 1

    quotes, quote_redef = parse_quotes(files)
    verdicts = judge(scan, names_h)
    nameplate = load_nameplate_table(game_dir)

    # NO SCOPE GUARD AGAINST esp_items.h's CURATED PICNUMS, AND NONE OF THE
    # OTHER RULES HAVE EVER HAD ONE (2026-09-21). The AA table has always been
    # the FALLBACK tier -- ekm_item_name() tries the curated table first and
    # only reaches this one on a miss -- so a row named here for a picnum the
    # curated table also names is inert at runtime, never wrong. Excluding
    # those picnums at generation time would duplicate that priority decision
    # in a second place it could drift out of sync with. Tier A finds five
    # such rows (COLA, SIXPAK, FIRSTAID, ACCESSCARD, ATOMICHEALTH all have
    # their own unique AA quote); they are left in, exactly as rule 1/2 have
    # always left the stock-shared rows they name.
    #
    # TIER A GLOBALS. Rule 2 --
    # "no other pickup actor reaches the same quote id" -- needs every
    # pickup's reachable plain-quote ids gathered FIRST, before any row can be
    # judged, because a row's own answer depends on every OTHER pickup's.
    pickup_plain_ids = {}    # actor name -> its distinct plain quote ids
    plain_quote_owners = {}  # quote id -> how many distinct pickups reach it
    for pq_name, pq_v in verdicts.items():
        if not pq_v.pickup:
            continue
        seen_ids = []
        for _f, _l, qid in scan.plain_quotes_of(pq_v.blk):
            if qid not in seen_ids:
                seen_ids.append(qid)
        pickup_plain_ids[pq_name] = seen_ids
        for qid in seen_ids:
            plain_quote_owners[qid] = plain_quote_owners.get(qid, 0) + 1

    rows, unresolved, oob, collide = [], [], [], []
    by_pic = {}
    src_tally = {"names.h": 0, "CON define": 0, "literal": 0}
    mini_no_candidate, mini_disagree = [], []
    for name in sorted(verdicts):
        v = verdicts[name]
        if not v.pickup:
            continue
        pic, src = resolve(name, names_h, scan.defines)
        if pic is None:
            unresolved.append(name)
            continue
        if not (0 <= pic < MAXTILES):
            oob.append((name, pic))
            continue
        if pic in by_pic:
            collide.append((pic, by_pic[pic], name))
            continue
        by_pic[pic] = name
        src_tally[src] += 1
        # THE SYMBOL IS THE FALLBACK, NOT THE DEFAULT. A row whose quotes do not
        # settle on one name keeps `name` -- the CON's own symbol -- so it still
        # draws; nothing here can emit an empty string or a number.
        nm = name_row(scan.quotes_of(v.blk), quotes)
        # D10 FIGURINE TIER (rule 3): tried only when quote naming found
        # nothing (rule 0) and the symbol is a `MINI*` row -- a row already
        # named by its OWN userquote is left alone. Every MINI* row in this
        # install lands here today (they share quote 1547), but the gate is
        # symbol-shaped, not install-shaped, so a future actor of its own
        # would still win on its own quote.
        fig_name, fig_note, fig_kind = None, None, None
        if nm.rule == 0 and name.startswith(MINI_PREFIX):
            fig_name, fig_note, fig_kind = resolve_figurine(
                name, names_h, scan.defines, nameplate)
            if fig_kind == "no_candidate":
                mini_no_candidate.append((name, fig_note))
            elif fig_kind == "disagree":
                mini_disagree.append((name, fig_note))
        # TIER A THEN TIER B (D10 lead 2), tried only when the figurine tier
        # also found nothing: ORDER IS EXPLICIT IN CODE, NOT INCIDENTAL. Tier
        # A can claim an `*UPGRADE` symbol on its own player-visible quote --
        # SHOTGUNUPGRADE (190) and TRIPBOMBUPGRADE (222) both do -- so tier B
        # never gets a turn for those two.
        tier_a_name, tier_a_qid, tier_a_why = None, None, None
        tier_b_name, tier_b_why = None, None
        if fig_name is None and nm.rule == 0:
            tier_a_name, tier_a_qid, tier_a_why = resolve_tier_a(
                pickup_plain_ids.get(name, []), plain_quote_owners, quotes)
            if tier_a_name is None:
                tier_b_name, tier_b_why = resolve_tier_b(name)

        row_qid = nm.quote_id
        if fig_name is not None:
            row_name, row_rule, why = fig_name, 3, fig_note
        elif tier_a_name is not None:
            row_name, row_rule, why, row_qid = tier_a_name, 4, None, tier_a_qid
        elif tier_b_name is not None:
            row_name, row_rule, why = tier_b_name, 5, None
        else:
            # THE OVERRIDE TOUCHES THE REASON ONLY, never the name or the
            # rule: a row that says why it kept its symbol is still a row
            # that kept it, and the totals must go on counting it that way.
            # A row that reached tier A and tier B and failed both states
            # ALL of why, so a reader never has to re-run the generator to
            # know.
            base_why = WHY_OVERRIDES.get(name, nm.why) if nm.rule == 0 else nm.why
            if nm.rule == 0:
                why = ("%s; plain quote -- %s; UPGRADE tier -- %s"
                       % (base_why, tier_a_why, tier_b_why))
            else:
                why = base_why
            row_name, row_rule = nm.name or name, nm.rule
        rows.append((pic, row_name, src, row_rule, name, why, row_qid))
    rows.sort(key=lambda r: r[0])

    pickups = [v for v in verdicts.values() if v.pickup]
    counts = {
        'root': args.script,
        'file_count': len(scan.files),
        'define_count': len(scan.defines),
        'redef_count': len(scan.ignored_redef),
        'actor_count': len(scan.actors),
        'state_count': len(scan.states),
        'r1_count': sum(1 for v in verdicts.values() if v.r1),
        'r2_count': sum(1 for v in verdicts.values() if v.r2),
        'r12_count': sum(1 for v in verdicts.values() if v.r1 and v.r2),
        'row_count': len(rows),
        'from_names_h': src_tally["names.h"],
        'from_con': src_tally["CON define"],
        'from_literal': src_tally["literal"],
        'unresolved_count': len(unresolved),
        'oob_count': len(oob),
        'collide_count': len(collide),
        'dup_count': len(scan.dup_actors),
        'quote_count': len(quotes),
        'quote_blank_count': sum(1 for t, _f, _l in quotes.values()
                                 if not t.strip()),
        'quote_redef_count': len(quote_redef),
        'rule1_count': sum(1 for r in rows if r[3] == 1),
        'rule2_count': sum(1 for r in rows if r[3] == 2),
        'rule3_count': sum(1 for r in rows if r[3] == 3),
        'rule4_count': sum(1 for r in rows if r[3] == 4),
        'rule5_count': sum(1 for r in rows if r[3] == 5),
        'rule0_count': sum(1 for r in rows if r[3] == 0),
        # THE 10-NAMED/12-VISIBLE SPLIT (2026-09-21): see TIER_A_STOCK_OVERLAP.
        'rule4_stock_count': sum(1 for r in rows if r[3] == 4
                                 and r[4] in TIER_A_STOCK_OVERLAP),
        # D10 FIGURINE TIER: the 51 MINI* rows, split three ways -- resolved,
        # or kept for one of two DIFFERENT failure shapes a future reader must
        # be able to tell apart (widened to
        # candidate agreement).
        'mini_total': sum(1 for r in rows if r[4].startswith(MINI_PREFIX)),
        'mini_resolved': sum(1 for r in rows if r[3] == 3),
        'mini_no_candidate': len(mini_no_candidate),
        'mini_disagree': len(mini_disagree),
    }
    counts['visible_named_count'] = (counts['rule4_count'] +
                                     counts['rule5_count'] -
                                     counts['rule4_stock_count'])

    for k in ('file_count', 'actor_count', 'state_count', 'r1_count',
              'r2_count', 'r12_count', 'row_count'):
        print("%-34s %d" % (k + ":", counts[k]))
    print("%-34s %d / %d / %d (names.h / CON / literal)"
          % ("name sources:", counts['from_names_h'], counts['from_con'],
             counts['from_literal']))
    print("%-34s %d / %d / %d / %d"
          % ("dropped unres/oob/dup-pic/dup-actor:", len(unresolved), len(oob),
             len(collide), len(scan.dup_actors)))
    print("%-34s %d / %d / %d / %d / %d / %d "
          "(rule 1 / rule 2 / figurine / quote / upgrade / symbol kept)"
          % ("names:", counts['rule1_count'], counts['rule2_count'],
             counts['rule3_count'], counts['rule4_count'],
             counts['rule5_count'], counts['rule0_count']))
    print("%-34s %d named (rule 4+5), %d of them inert curated-stock "
          "overlaps, %d change on screen"
          % ("D10 lead 2 split:", counts['rule4_count'] + counts['rule5_count'],
             counts['rule4_stock_count'], counts['visible_named_count']))
    print("%-34s %d, blank %d, written twice %d"
          % ("definequote ids:", len(quotes), counts['quote_blank_count'],
             len(quote_redef)))
    print("%-34s %d total, %d resolved, %d no candidate, %d disagree"
          % ("MINI* figurines:", counts['mini_total'], counts['mini_resolved'],
             counts['mini_no_candidate'], counts['mini_disagree']))

    if counts['mini_total'] != 51:
        print("expected 51 MINI* rows, the walk found %d -- SAY SO, do not "
              "round: nothing written" % counts['mini_total'], file=sys.stderr)
        return 1
    if counts['mini_resolved'] + counts['mini_no_candidate'] + \
            counts['mini_disagree'] != counts['mini_total']:
        print("MINI* total %d but resolved+no-candidate+disagree is %d -- "
              "nothing written"
              % (counts['mini_total'], counts['mini_resolved'] +
                 counts['mini_no_candidate'] + counts['mini_disagree']),
              file=sys.stderr)
        return 1
    # The figurine reconciliation: the spec
    # names an EXACT set of 19 newly-resolved figurines against this install
    # and says to stop rather than tune the rule if a run differs. 14 were
    # already resolved before this lead, so 14+19=33 resolved, 15 with no
    # named candidate, 3 where named candidates disagree (15+3=18 still
    # refused, matching 51-33). If a future AA update moves any of these,
    # this SAYS SO instead of silently emitting a different table.
    if (counts['mini_resolved'], counts['mini_no_candidate'],
            counts['mini_disagree']) != (33, 15, 3):
        print("D10 lead 4 expected 33 resolved / 15 no-candidate / 3 "
              "disagree, this install's walk found %d / %d / %d -- SAY SO, "
              "do not tune the rule to match: nothing written"
              % (counts['mini_resolved'], counts['mini_no_candidate'],
                 counts['mini_disagree']), file=sys.stderr)
        return 1

    # A COUNT AND AN ENUMERATION OF THE SAME THING MUST AGREE, and
    # that holds for the naming rules as well as for the rows.
    named = (counts['rule1_count'] + counts['rule2_count'] +
             counts['rule3_count'] + counts['rule4_count'] +
             counts['rule5_count'] + counts['rule0_count'])
    if named != len(rows):
        print("rows %d but rules account for %d -- nothing written"
              % (len(rows), named), file=sys.stderr)
        return 1
    blank = [r[4] for r in rows if not r[1]]
    if blank:
        print("rows with no name at all: %s -- nothing written" % blank,
              file=sys.stderr)
        return 1

    accounted = len(rows) + len(unresolved) + len(oob) + len(collide)
    if accounted != len(pickups):
        print("pickups %d but rows+dropped %d -- nothing written"
              % (len(pickups), accounted), file=sys.stderr)
        return 1
    if not rows:
        print("no pickup matched all three rules -- nothing written",
              file=sys.stderr)
        return 1

    text, report = render(scan, rows, verdicts, unresolved, oob, collide, quote_redef,
                  args.script, game_dir, args.date, counts,
                  mini_no_candidate, mini_disagree)
    # CRLF explicitly: every source file in this repo is CRLF and a plain
    # text-mode write emits LF, which rewrites the whole file's endings.
    with open(str(out_path), "w", encoding="utf-8", newline="") as fh:
        fh.write(text.replace("\n", "\r\n"))
    print(report)
    print("wrote %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
