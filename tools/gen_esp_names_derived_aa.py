#!/usr/bin/env python3
# Generates src/ekmod/esp_names_derived_aa.h -- AA6 step 2. Scratch copy,
# tested here before it is moved to tools/.
#
# CANDIDATE SET (source 1): every `useractor enemy <SYMBOL>` or
# `useractor enemystayput <SYMBOL>` in scripts/*.CON whose picnum (names.h,
# then AADEFS.CON `define`, same precedence as gen_esp_names_aa.py) has NO
# curated (esp_names.h) name and NO AA-nameplate (esp_names_aa.h) name. Each
# such candidate is tried against these rules, first match wins:
#
#   a. curated block -- the user's five 2026-09-25 choices, by SYMBOL.
#   b. civilian -- the useractor line runs `state femcivcode` or
#      `state malecivcode` -> "Civilian".
#   c. base tile -- strip one known AA pose suffix (from a candidate
#      superset, tried longest-first; only the ones that actually resolved
#      something for THIS install are kept in the generated header's comment
#      and printed as "suffixes used"), then if that remainder does not
#      itself carry a curated/nameplate name, also strip trailing digits;
#      the final remainder must be a names.h/AADEFS.CON symbol whose picnum
#      HAS a curated or nameplate name. Emits a BASE-PICNUM row.
#   d. otherwise: no row.
#
# CANDIDATE SET (source 2, source 2): a pose tile a cactor
# switches to at RUNTIME is declared by no `useractor` line at all -- only
# its BASE is (WESBOT 19481 has a useractor line; WESBOTCROUCH 19521, the
# pose the game switches Wes to while he crouches, does not). Source 1's scan
# can never see such a symbol, so source 2 scans every names.h (then
# AADEFS.CON, same precedence) symbol instead, keeping the ones of the form
# <BASE><SUFFIX> (SUFFIX from the same pose-suffix list rule c uses, no bare
# trailing-digit form). A candidate is skipped if its OWN picnum already
# carries a row (from source 1) or an earlier-tier name; otherwise it emits a
# rule-c BASE-PICNUM row exactly as rule c does, provided BASE resolves to a
# picnum that is ALREADY NAMED -- by an earlier tier or by this table's own
# (source-1) rows. BASE unnamed/undefined: no row (counted).
#
# RUN BY HAND, NOT BY build.ps1, same reason as gen_esp_names_aa.py: the
# build must not depend on the game being installed.
#
#   python tools/gen_esp_names_derived_aa.py
#
# from the repo root (or with an explicit --game-dir). Overwrites
# src/ekmod/esp_names_derived_aa.h. Do not hand-edit that file.

import argparse
import os
import re
import sys
from pathlib import Path

# The user's curated choices. Checked BEFORE the
# civilian and suffix rules, in a fixed order.
CURATED_CHOICES = {
    "OCTATROOP": "Octatrooper",
    "WESBOT": "Wes bot",
    "DEABOT": "Dea bot",  # DEABOTCROUCH follows via source 2
    "POLYMORPHPAD": "Polymorph pad",
    "POLYMORPHPOD": "Polymorph pod",
    "POLYPOLE": "Polymorph pole",
    # TERMTORSO: what a Dukinator becomes when its
    # legs are shot off (`espawn TERMTORSO`, TERMINATOR.CON:369). "TORSO" is
    # no pose suffix (SUFFIX_CANDIDATES), so rule c can never resolve it --
    # named by hand instead of widening the suffix list for one symbol.
    "TERMTORSO": "Dukinator torso",
    # STRIPPER: DOES carry a `useractor enemy`
    # line (CIVILIAN.CON:3207) -- an earlier note that it has none was
    # wrong, disproven by grepping the install -- so it IS a source-1
    # candidate already. What rule b's CIVILIAN_RE actually requires is
    # `state femcivcode`/`state malecivcode` on that line; STRIPPER's line
    # runs `state strippercode` instead (AA's own separate civ-like state,
    # also used by KBABE/CHEERLEADER/HOOKER per AAGAME.CON:1716's `civlist`
    # switch), so rule b never classifies it and it fell to rule d
    # (unresolved) until this curated entry named it by hand. The label
    # reuses rule b's own string so every civilian reads alike regardless of
    # which rule named it.
    "STRIPPER": "Civilian",
    # AA6e (2026-09-29): "I found an enemy with the name 22819". Census by
    # tools/aa6e_unnamed_enemies.py; each name cites its CON line beside it.
    "ARMLIZJETPACK": "Armored Trooper",   # ARMLIZTROOP.CON:950, sibling of ARMLIZTROOP
    "ARMLIZSTAYPUT": "Armored Trooper",   # ARMLIZTROOP.CON:939, cactor ARMLIZTROOP
    "GRAVESNOGUN": "General Graves",      # GRAVES.CON:1067; GRAVESNPC's cases list it (AAGAME.CON:13487)
    "GIANTGUY": "Giant Guy",              # CIVILIAN.CON:4800, its own define
    "ZOMBIEHEADSHOT2": "Zombie",          # ZOMBIE.CON:494/974, a Zombie pose
    "NIPCHEER": "Civilian",               # CIVILIAN.CON:3978; AAGAME.CON:1716 civlist with STRIPPER
    "NIPSABERDEATH": "Trooper",           # NEWTROOP.CON:532, NIPTROOP's saber death
    "JIKURAI": "Jikurai",                 # JIKURAI.CON:1 header, its own define
    "SEAGULL": "Seagull",                 # NEWVACA.CON:739/839, its own define
}

# A SUPERSET of candidate AA/Duke pose-and-state suffixes, tried longest
# first so e.g. STAYPUT is not shadowed by a shorter accidental match. Only
# the ones that actually resolve a candidate in THIS install are reported as
# "used" -- this list is deliberately wider than the six the 2026-09-25
# evidence log named (STAYPUT, GUARD, JUSTSIT, ONTOILET, CRAWLPUT, JUMP) so
# the generator, not a human guess, decides which additional ones the full
# 91-file CON set actually needs.
SUFFIX_CANDIDATES = sorted([
    "ONTOILET", "CRAWLPUT", "JUSTSIT", "STAYPUT", "HANGDEAD", "DUCKING",
    "RUNNING", "JETPACK", "CROUCH", "GUARD", "SLEEP", "STAND", "SHOOT", "DUCK", "JUMP",
    "HANG", "DEAD", "WALK", "IDLE", "SIT",
], key=len, reverse=True)


# ---------------------------------------------------------------------------
def parse_names_h(path):
    """`#define NAME picnum` -> {NAME: picnum}. Same shape
    gen_esp_names_aa.py's parser and tilenames.cpp's runtime parser use."""
    names = {}
    text = path.read_text(encoding="latin-1")
    for line in text.splitlines():
        m = re.match(r'#define\s+(\S+)\s+(-?\d+)\s*$', line.strip())
        if m:
            names[m.group(1)] = int(m.group(2))
    return names


def parse_defs_con(path):
    """AADEFS.CON's `define NAME picnum` -- the SECONDARY resolver, same as
    gen_esp_names_aa.py."""
    names = {}
    text = path.read_text(encoding="latin-1")
    for line in text.splitlines():
        stripped = re.sub(r'//.*', '', line).strip()
        m = re.match(r'define\s+(\S+)\s+(-?\d+)\s*$', stripped)
        if m:
            names[m.group(1)] = int(m.group(2))
    return names


def parse_names_h_lines(path):
    """Same match as parse_names_h, but SYMBOL -> its 1-based line number --
    source 2's own candidates are names.h symbols directly, so its rows need
    a citation line the way source 1's useractor scan already has one."""
    lines = {}
    text = path.read_text(encoding="latin-1")
    for lineno, line in enumerate(text.splitlines(), start=1):
        m = re.match(r'#define\s+(\S+)\s+(-?\d+)\s*$', line.strip())
        if m and m.group(1) not in lines:
            lines[m.group(1)] = lineno
    return lines


def parse_defs_con_lines(path):
    """Same match as parse_defs_con, SYMBOL -> line number (source 2)."""
    lines = {}
    text = path.read_text(encoding="latin-1")
    for lineno, line in enumerate(text.splitlines(), start=1):
        stripped = re.sub(r'//.*', '', line).strip()
        m = re.match(r'define\s+(\S+)\s+(-?\d+)\s*$', stripped)
        if m and m.group(1) not in lines:
            lines[m.group(1)] = lineno
    return lines


def resolve_symbol(symbol, names_map, defs_map):
    """names.h PRIMARY, AADEFS.CON SECONDARY -- returns (picnum, source) or
    (None, None)."""
    if symbol in names_map:
        return names_map[symbol], "names.h"
    if symbol in defs_map:
        return defs_map[symbol], "AADEFS.CON"
    return None, None


# ---------------------------------------------------------------------------
def extract_body(text, signature):
    """The braced body of the first function whose text contains
    `signature`, matched by brace depth so a nested `{`/`}` (there is none
    here, but the method should not assume) cannot truncate it early."""
    i = text.index(signature)
    brace = text.index('{', i)
    depth = 0
    j = brace
    while True:
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                break
        j += 1
    return text[brace:j + 1]


def picnums_from_cases(body):
    return set(int(m.group(1)) for m in re.finditer(r'case\s+(-?\d+)\s*:', body))


def picnums_from_ranges(body):
    """`picnum >= A && picnum <= B` style range guards (GREENSLIME)."""
    out = set()
    for m in re.finditer(r'>=\s*(\d+)\s*&&\s*\w+\s*<=\s*(\d+)', body):
        out.update(range(int(m.group(1)), int(m.group(2)) + 1))
    return out


def load_curated_set(esp_names_h_path):
    text = esp_names_h_path.read_text(encoding="utf-8")
    enemy_body = extract_body(text, "inline const char* ekm_enemy_name(int picnum)")
    trooper_body = extract_body(text, "inline bool ekm_picnum_is_trooper(int picnum)")
    return (picnums_from_cases(enemy_body) | picnums_from_cases(trooper_body)
            | picnums_from_ranges(enemy_body))


def load_nameplate_set(esp_names_aa_h_path):
    text = esp_names_aa_h_path.read_text(encoding="utf-8")
    body = extract_body(text, "inline const char* ekm_enemy_name_aa(int picnum)")
    return picnums_from_cases(body)


# ---------------------------------------------------------------------------
USERACTOR_RE = re.compile(r'^useractor\s+(enemy|enemystayput)\s+(\S+)')
CIVILIAN_RE = re.compile(r'state\s+(femcivcode|malecivcode)\b')


def scan_candidates(scripts_dir):
    """Every `useractor enemy`/`useractor enemystayput` SYMBOL, first
    occurrence only (a symbol re-declared in a second file, none observed in
    the 2026-09-25 install, would keep its first evidence line). Returns
    {SYMBOL: (rel_path, lineno, is_civilian)}."""
    out = {}
    for p in sorted(scripts_dir.glob("*.CON")):
        text = p.read_text(encoding="latin-1")
        for lineno, line in enumerate(text.splitlines(), start=1):
            m = USERACTOR_RE.match(line.strip())
            if not m:
                continue
            symbol = m.group(2)
            if symbol in out:
                continue
            out[symbol] = (f"scripts/{p.name}", lineno, bool(CIVILIAN_RE.search(line)))
    return out


# ---------------------------------------------------------------------------
def try_base(symbol, names_map, defs_map, named):
    """Rule c. `named(picnum) -> bool` decides what counts as an
    already-named base -- source 1 passes curated/nameplate only (unchanged
    behaviour); source 2 (brief-aa6b) also counts this table's own source-1
    rows, since a pose tile's base is usually a source-1 row, not a curated
    or nameplate entry itself. Returns (base_symbol, base_picnum,
    suffix_or_None, digits_stripped_bool) on success, or None."""
    suffix_used = None
    remainder = symbol
    for suf in SUFFIX_CANDIDATES:
        if symbol.endswith(suf) and len(symbol) > len(suf):
            remainder = symbol[:-len(suf)]
            suffix_used = suf
            break

    pic, _ = resolve_symbol(remainder, names_map, defs_map)
    if pic is not None and named(pic):
        return remainder, pic, suffix_used, False

    m = re.match(r'^(.*?)(\d+)$', remainder)
    if m:
        remainder2 = m.group(1)
        pic2, _ = resolve_symbol(remainder2, names_map, defs_map)
        if pic2 is not None and named(pic2):
            return remainder2, pic2, suffix_used, True

    return None


def find_suffix(symbol):
    """Longest-first match against SUFFIX_CANDIDATES, or None. Source 2 only
    accepts a literal <BASE><SUFFIX> symbol -- unlike rule c's fallback,
    there is no bare trailing-digit form here (brief-aa6b)."""
    for suf in SUFFIX_CANDIDATES:
        if symbol.endswith(suf) and len(symbol) > len(suf):
            return suf
    return None


def scan_pose_symbols(names_map, defs_map, names_lines, defs_lines):
    """Source 2's candidate set: every names.h (PRIMARY) then AADEFS.CON
    (SECONDARY, symbols not already in names.h) symbol, each with a citation
    line for the generated header -- brief-aa6b, 2026-09-28."""
    out = {}
    for sym in names_map:
        out[sym] = ("names.h", names_lines.get(sym, 0))
    for sym in defs_map:
        if sym not in out:
            out[sym] = ("scripts/AADEFS.CON", defs_lines.get(sym, 0))
    return out


def add_pose_tile_rows(pose_symbols, names_map, defs_map, named,
                        base_rows, suffixes_used, counts):
    """Source 2 (brief-aa6b, 2026-09-28): for every <BASE><SUFFIX> symbol
    (sorted, so a chain onto an earlier source-2 row is deterministic) whose
    own picnum has no row/earlier-tier name yet, try rule c's base-tile
    match with `named` widened to also cover this table's own rows. Mutates
    `base_rows`/`suffixes_used`/`counts` in place; returns the list of rows
    added, for the header's own listing (symbol, picnum, base_symbol,
    base_picnum)."""
    added = []
    counts["source2_candidates"] = 0
    counts["source2_already_named"] = 0
    counts["source2_base_unnamed"] = 0
    counts["source2_added"] = 0
    for symbol in sorted(pose_symbols):
        if find_suffix(symbol) is None:
            continue  # not of the form <BASE><SUFFIX>; not a source-2 candidate
        counts["source2_candidates"] += 1

        # pose_symbols is built FROM names_map/defs_map, so this always resolves.
        pic, _ = resolve_symbol(symbol, names_map, defs_map)
        if named(pic):
            counts["source2_already_named"] += 1
            continue

        found = try_base(symbol, names_map, defs_map, named)
        if not found:
            counts["source2_base_unnamed"] += 1
            continue

        base_symbol, base_pic, suffix, digits = found
        where, lineno = pose_symbols[symbol]
        base_rows[pic] = (base_symbol, base_pic, symbol, f"{where}:{lineno}", suffix, digits)
        suffixes_used[suffix] = suffixes_used.get(suffix, 0) + 1
        counts["source2_added"] += 1
        added.append((symbol, pic, base_symbol, base_pic))
    return added


def find_define_in_scripts(scripts_dir, symbol):
    """`define SYMBOL n` in any scripts/*.CON, first hit, or None (AA6e)."""
    pat = re.compile(r'^define\s+' + re.escape(symbol) + r'\s+(-?\d+)\s*$')
    for p in sorted(scripts_dir.glob("*.CON")):
        for line in p.read_text(encoding="latin-1").splitlines():
            m = pat.match(re.sub(r'//.*', '', line).strip())
            if m:
                return int(m.group(1))
    return None


def build_table(game_dir, curated_set, nameplate_set):
    names_map = parse_names_h(game_dir / "names.h")
    defs_map = parse_defs_con(game_dir / "scripts" / "AADEFS.CON")
    names_lines = parse_names_h_lines(game_dir / "names.h")
    defs_lines = parse_defs_con_lines(game_dir / "scripts" / "AADEFS.CON")
    candidates = scan_candidates(game_dir / "scripts")

    string_rows = {}   # picnum -> (name, symbol, where, rule)
    base_rows = {}      # picnum -> (base_symbol, base_picnum, symbol, where, suffix, digits)
    suffixes_used = {}
    counts = {"curated": 0, "civilian": 0, "suffix": 0, "unresolved": 0,
              "symbol_unresolved": 0, "already_named": 0}
    named_source1 = lambda p: p in curated_set or p in nameplate_set

    for symbol in sorted(candidates):
        where, lineno, is_civ = candidates[symbol]
        pic, src = resolve_symbol(symbol, names_map, defs_map)
        if pic is None and symbol in CURATED_CHOICES:
            # A hand-curated symbol may be defined only in its own actor file
            # (SEAGULL, NEWVACA.CON:739); no other rule ever reads this.
            pic = find_define_in_scripts(game_dir / "scripts", symbol)
        if pic is None:
            counts["symbol_unresolved"] += 1
            continue
        if pic in curated_set or pic in nameplate_set:
            counts["already_named"] += 1
            continue
        if pic in string_rows or pic in base_rows:
            # A second SYMBOL sharing an already-resolved picnum: keep the
            # first row, same "first occurrence wins" rule as scan_candidates.
            continue

        if symbol in CURATED_CHOICES:
            string_rows[pic] = (CURATED_CHOICES[symbol], symbol, f"{where}:{lineno}", "curated")
            counts["curated"] += 1
            continue

        if is_civ:
            string_rows[pic] = ("Civilian", symbol, f"{where}:{lineno}", "civilian")
            counts["civilian"] += 1
            continue

        found = try_base(symbol, names_map, defs_map, named_source1)
        if found:
            base_symbol, base_pic, suffix, digits = found
            base_rows[pic] = (base_symbol, base_pic, symbol, f"{where}:{lineno}", suffix, digits)
            counts["suffix"] += 1
            if suffix:
                suffixes_used[suffix] = suffixes_used.get(suffix, 0) + 1
            continue

        counts["unresolved"] += 1

    # Source 2: runs after source 1 has finished, so
    # `named_source2` sees the complete set of source-1 rows -- a pose tile's
    # BASE is usually one of those, not a curated/nameplate entry itself
    # (WESBOTCROUCH -> WESBOT, a rule-a row). Its own suffix tally is kept in
    # a SEPARATE dict -- sharing `suffixes_used` would silently fold source
    # 2's hits into rule c's "suffixes used" line, misreporting a suffix
    # (e.g. CROUCH) as used one more time by rule c than it actually was.
    named_source2 = lambda p: (p in curated_set or p in nameplate_set
                                or p in string_rows or p in base_rows)
    pose_symbols = scan_pose_symbols(names_map, defs_map, names_lines, defs_lines)
    suffixes_used_source2 = {}
    source2_added = add_pose_tile_rows(pose_symbols, names_map, defs_map,
                                        named_source2, base_rows, suffixes_used_source2, counts)

    return string_rows, base_rows, suffixes_used, counts, len(candidates), source2_added


# ---------------------------------------------------------------------------
HEADER_TEMPLATE = """#pragma once

// Derived names for AA enemy picnums with no curated (esp_names.h) or nameplate (esp_names_aa.h) entry: pose/spawn
// variants (*STAYPUT, *GUARD, ...) and civilians. GENERATED by tools/gen_esp_names_derived_aa.py: DO NOT HAND-EDIT.
// Sources: `useractor enemy` lines in scripts/*.CON, names.h (primary; also a candidate set), AADEFS.CON (secondary).

// A fixed derived name, or null if `picnum` has no such entry (rules a, b).
inline const char* ekm_enemy_name_derived_aa_string(int picnum) {{
  switch (picnum) {{
{string_cases}
    default: return 0;
  }}
}}

// The BASE PICNUM to rerun the chain on, or 0 (rule c): the caller reruns the tier-1/2 rules on the base with the
// SPRITE'S OWN pal, so AA's pal/mtype/team overrides still apply; a pose variant draws as its base's model, so this
// is a name correction only. Never both this and the string function non-null for one picnum.
inline int ekm_enemy_name_derived_aa_base(int picnum) {{
  switch (picnum) {{
{base_cases}
    default: return 0;
  }}
}}
"""


def cpp_string(text):
    return text.replace("\\", "\\\\").replace('"', '\\"')


def render_header(string_rows, base_rows, suffixes_used, counts, candidate_count,
                   game_dir, source2_added):
    string_cases = "\n".join(
        f'    case {p}: return "{cpp_string(name)}";'
        for p, (name, sym, where, rule) in sorted(string_rows.items())
    ) or "    // (none)"

    def base_case_line(p, row):
        base_symbol, base_pic, sym, where, suffix, digits = row
        note = f'suffix "{suffix}"' if suffix else "no suffix"
        if digits:
            note += ", trailing digits stripped"
        return f'    case {p}: return {base_pic};'

    base_cases = "\n".join(
        base_case_line(p, base_rows[p]) for p in sorted(base_rows)
    ) or "    // (none)"

    suffixes_text = ", ".join(f"{s} ({n})" for s, n in
                               sorted(suffixes_used.items(), key=lambda kv: (-kv[1], kv[0]))) or "(none)"

    source2_rows_comment = "\n".join(
        f'//   {sym} ({pic}) -> {base_sym} ({base_pic})'
        for sym, pic, base_sym, base_pic in sorted(source2_added, key=lambda r: r[1])
    ) or "//   (none)"

    return HEADER_TEMPLATE.format(
        game_dir=str(game_dir).replace("\\", "/"),
        date="2026-09-28",
        candidate_count=candidate_count,
        already_named_count=counts["already_named"],
        symbol_unresolved_count=counts["symbol_unresolved"],
        curated_count=counts["curated"],
        civilian_count=counts["civilian"],
        suffix_count=counts["suffix"],
        suffixes_used=suffixes_text,
        unresolved_count=counts["unresolved"],
        source2_candidate_count=counts["source2_candidates"],
        source2_already_named_count=counts["source2_already_named"],
        source2_base_unnamed_count=counts["source2_base_unnamed"],
        source2_added_count=counts["source2_added"],
        source2_rows_comment=source2_rows_comment,
        string_cases=string_cases,
        base_cases=base_cases,
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-dir", default=os.environ.get("AA_GAME_DIR"),
                    required="AA_GAME_DIR" not in os.environ,
                    help="<AA game folder>, the AA install root (or set AA_GAME_DIR)")
    ap.add_argument("--repo-dir", default=".",
                     help="repo root, for esp_names.h/esp_names_aa.h")
    ap.add_argument("--out", default="src/ekmod/esp_names_derived_aa.h")
    args = ap.parse_args()

    game_dir = Path(args.game_dir)
    repo_dir = Path(args.repo_dir)

    curated_set = load_curated_set(repo_dir / "src" / "ekmod" / "esp_names.h")
    nameplate_set = load_nameplate_set(repo_dir / "src" / "ekmod" / "esp_names_aa.h")

    string_rows, base_rows, suffixes_used, counts, candidate_count, source2_added = build_table(
        game_dir, curated_set, nameplate_set)

    print(f"candidates scanned:       {candidate_count}")
    print(f"already named:            {counts['already_named']}")
    print(f"symbol unresolved:        {counts['symbol_unresolved']}")
    print(f"rule a (curated):         {counts['curated']}")
    print(f"rule b (civilian):        {counts['civilian']}")
    print(f"rule c (suffix):          {counts['suffix']}, suffixes used: {suffixes_used}")
    print(f"rule d (unresolved):      {counts['unresolved']}")
    for sym, pic, base_sym, base_pic in sorted(source2_added, key=lambda r: r[1]):
        print(f"source 2 row: {sym} ({pic}) -> {base_sym} ({base_pic})")
    print(f"string rows:              {len(string_rows)}")
    print(f"base rows:                {len(base_rows)}")
    print(f"source2 candidates:       {counts['source2_candidates']}")
    print(f"source2 already named:    {counts['source2_already_named']}")
    print(f"source2 base unnamed:     {counts['source2_base_unnamed']}")
    print(f"source2 rows added:       {counts['source2_added']}: {source2_added}")

    out_text = render_header(string_rows, base_rows, suffixes_used, counts,
                              candidate_count, game_dir, source2_added)

    out_path = repo_dir / args.out
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        f.write(out_text.replace("\n", "\r\n"))
    print(f"wrote {out_path}")


if __name__ == "__main__":
    sys.exit(main())
