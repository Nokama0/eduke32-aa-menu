#!/usr/bin/env python3
# Generates src/ekmod/esp_names_aa.h from Alien Armageddon's OWN nameplate
# data -- no wiki, no invented names. Reads four files under the AA install:
#
#   1. scripts/weapons_hud.CON, the `switch sprite[monid].picnum` block that
#      picks a nameplate quote id for a monster (~lines 2836-2931 as of the
#      2026-09-20 install).
#   2. scripts/AAUSER.CON and scripts/NPCQUOTES.CON, the `definequote <id>
#      <text>` lines that hold the nameplate text itself.
#   3. names.h, the `#define NAME picnum` lines mapping the CON's symbolic
#      actor names to picnums -- the same file tilenames.cpp already parses.
#      PRIMARY: this is the canonical tile list.
#   4. scripts/AADEFS.CON, `define NAME picnum` lines -- the CON-side
#      equivalent of names.h's mapping, same mod and same authority, just a
#      different file. SECONDARY: consulted only for a case name names.h does
#      not define at all. Authorised 2026-09-20 after the first pass reported
#      22 unresolved case names that all turned out to live only here.
#
# THE SWITCH KEYS ON FOUR THINGS, NOT ONE. The first version of this script
# extracted the picnum and dropped `sprite[monid].pal`, `actorvar[monid].mtype`
# and the `default` arm's `actorvar[monid].team` -- and since not one civilian
# in the game is an explicit `case`, every civilian fell through to the
# code-name tier. parse_switch_block now keeps the condition as well as the
# value.
#
# RUN BY HAND, NOT BY build.ps1: the build must not depend on the game being
# installed. Re-run after an AA update with:
#
#   python tools/gen_esp_names_aa.py
#
# from the repo root (or with an explicit --game-dir). It overwrites
# src/ekmod/esp_names_aa.h. Do not hand-edit that file; fix this script and
# re-run it instead.

import argparse
import os
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Title-casing: AA's own quote text is ALL CAPS ("EDF TROOPER"); the house
# style (esp_names.h) is title case ("Assault Trooper"). The WORDS stay AA's,
# only the case changes. A short list of tokens stay fully upper because they
# are initialisms, not words: EDF (Earth Defense Force) and SUV. Found by
# scanning every extracted quote text below -- no other all-caps token in the
# 2026-09-20 AA install reads as an initialism rather than an ordinary word.
UPPER_TOKENS = {"EDF", "SUV"}

# The left-hand sides the switch's `ife`/`ifvarand` lines test, mapped to the
# name this generator and the header call each dimension by. A left-hand side
# that is not here (AA uses one: the episode var `VOLUME`) is reported as an
# unemitted conditional rather than guessed at.
COND_LHS = {
    "sprite[monid].pal": "pal",
    "actorvar[monid].team": "team",
    "actorvar[monid].mtype": "mtype",
}

# Which dimensions reach the generated header as a per-picnum override. `pal`
# comes off the sprite record; `mtype` and `team` are per-actor gamevars this
# port can now read (gamevars.h). `team` appears TWICE in AA's switch and the
# two uses are different things: a `case` arm keyed on team is a picnum case
# and lands here, while the `default` arm's team rule is a fallback and is
# emitted separately as ekm_enemy_name_aa_team. Anything left (AA branches two
# arms on the episode global `VOLUME`) is reported, not guessed at.
EMITTED_COND = ("pal", "mtype", "team")


def title_case(all_caps_text):
    """All-caps in, house style out. Splits on letter/non-letter runs so a
    hyphen, apostrophe or ampersand inside one quote-text token (ARMY-ANT,
    LI'LY', BIGBOI&LITTLEMAN) still gets each letter-run capitalized rather
    than only the first character of the whole token."""
    out = []
    for run in re.split(r'([A-Za-z]+)', all_caps_text):
        if run.isalpha():
            out.append(run if run.upper() in UPPER_TOKENS else run[0].upper() + run[1:].lower())
        else:
            out.append(run)
    return "".join(out)


# ---------------------------------------------------------------------------
def parse_names_h(path):
    """`#define NAME picnum` -> {NAME: picnum}. Same shape tilenames.cpp
    parses at runtime (tilenames.h's ekm_tilename_parse_line); this script
    only needs the plain-literal case since it is reading a known-good file
    once, not guarding a runtime parse."""
    names = {}
    text = path.read_text(encoding="latin-1")
    for line in text.splitlines():
        m = re.match(r'#define\s+(\S+)\s+(-?\d+)\s*$', line.strip())
        if m:
            names[m.group(1)] = int(m.group(2))
    return names


def parse_defs_con(path):
    """AADEFS.CON's `define NAME picnum` -- names.h's shape without the `#`,
    and with trailing `// comment` to strip first (GRAVESNPC has one).
    Returns {NAME: picnum}, the SECONDARY resolver: build_table only looks a
    name up here once names.h has already said it does not know it."""
    names = {}
    text = path.read_text(encoding="latin-1")
    for line in text.splitlines():
        stripped = re.sub(r'//.*', '', line).strip()
        m = re.match(r'define\s+(\S+)\s+(-?\d+)\s*$', stripped)
        if m:
            names[m.group(1)] = int(m.group(2))
    return names


def parse_quotes(paths):
    """`definequote <id> <text>` -> {id: text}, across every file given."""
    quotes = {}
    for p in paths:
        text = p.read_text(encoding="latin-1")
        for line in text.splitlines():
            m = re.match(r'definequote\s+(\d+)\s+(.*)', line)
            if m:
                quotes[int(m.group(1))] = m.group(2)
    return quotes


QUOTE_TEXT_RE = re.compile(r'^L%d\s+(.*?)\s*\^23%d\s*$')


def extract_quote_body(raw_text):
    """Strips the leading `L%d `, the trailing ` ^23%d`, and any `^NN` colour
    code, then trims. Returns None if the quote does not hold this shape --
    the caller reports that rather than mangling it."""
    m = QUOTE_TEXT_RE.match(raw_text.strip())
    if not m:
        return None
    body = re.sub(r'\^\d+', '', m.group(1)).strip()
    return re.sub(r'\s+', ' ', body)


def parse_arm(rest):
    """One switch arm's tokens, after its `case NAME` pairs (or its `default`)
    have been taken off. Returns (base_quote_id, condition) where condition is
    None or a dict {op, lhs, kind, rhs, value}:

      op    'ife' (equality) or 'ifvarand' (a BIT TEST -- `ifvarand x 1` is
            true for 1 and for 3, and writing it as `== 1` would name a
            damaged gunship undamaged)
      lhs   the CON's own left-hand side, verbatim
      kind  COND_LHS[lhs], or None when this script does not know that lhs
      rhs   the integer the CON compares against
      value the quote id the condition selects when it holds

    Two shapes appear in the 2026-09-20 CON and both are handled by reading
    the tokens in order rather than by special-casing an actor:

      `set temp A  ife <lhs> <n> set temp B break`
          A is set before any condition is tested, so A is the base and B the
          override. This is the LIZTROOP family's pal-21 Captain.
      `ife <lhs> <n> set temp A else set temp B break`
          B is the `else`, so B is the base and A the override.

    An arm with no condition at all has one `set temp` and that is the base.
    """
    cond_at = next((i for i, t in enumerate(rest)
                    if t in ("ife", "ifvarand")), None)

    def settemps(toks):
        return [toks[j + 2] for j in range(len(toks))
                if j + 2 < len(toks) and toks[j] == "set" and toks[j + 1] == "temp"]

    if cond_at is None:
        return int(settemps(rest)[0]), None

    op = rest[cond_at]
    lhs = rest[cond_at + 1]
    rhs = int(rest[cond_at + 2])
    after = rest[cond_at + 3:]
    taken = settemps(after)
    if "else" in after:
        # `... set temp A else set temp B` -- A is taken when the condition
        # holds, B otherwise.
        base, chosen = int(taken[1]), int(taken[0])
    else:
        base, chosen = int(settemps(rest[:cond_at])[0]), int(taken[0])

    return base, {"op": op, "lhs": lhs, "kind": COND_LHS.get(lhs),
                  "rhs": rhs, "value": chosen}


def parse_switch_block(con_path):
    """Reads the `switch sprite[monid].picnum` ... `endswitch` block and
    returns (cases_map, default_info, total_case_tokens):

      cases_map: {CASE_NAME: (base_quote_id, condition)} for every non-default
                 case, one entry per case token (a multi-case line fans out to
                 each name). `condition` is parse_arm's dict or None.
      default_info: (base_quote_id, condition) for the `default` arm, or None
                 if the block had no default. Its base is the `else` value,
                 which AA writes as -1: no nameplate at all.
    """
    lines = con_path.read_text(encoding="latin-1").splitlines()

    start = next((i for i, l in enumerate(lines) if 'switch sprite[monid].picnum' in l), None)
    if start is None:
        raise RuntimeError(f"'switch sprite[monid].picnum' not found in {con_path}")
    end = next((i for i in range(start + 1, len(lines)) if 'endswitch' in lines[i]), None)
    if end is None:
        raise RuntimeError(f"no matching 'endswitch' after line {start + 1} in {con_path}")

    # Comments are stripped PER LINE, before the lines are joined -- joining
    # first and stripping `//...` after would eat everything past the first
    # `//` in the whole block, including the "civilian cases" comment partway
    # through this one. (found and fixed while writing this generator)
    stripped = [re.sub(r'//.*', '', l) for l in lines[start + 1:end]]
    tokens = " ".join(s.strip() for s in stripped).split()

    blocks, cur = [], []
    for t in tokens:
        cur.append(t)
        if t == 'break':
            blocks.append(cur)
            cur = []
    if cur:
        blocks.append(cur)

    cases_map = {}
    default_info = None
    total_case_tokens = 0
    for b in blocks:
        idx = 0
        names = []
        is_default = (b[0] == 'default')
        if is_default:
            idx = 1
        else:
            while idx + 1 < len(b) and b[idx] == 'case':
                names.append(b[idx + 1])
                idx += 2
        total_case_tokens += len(names)

        arm = parse_arm(b[idx:])
        if is_default:
            default_info = arm
        else:
            for n in names:
                cases_map[n] = arm

    return cases_map, default_info, total_case_tokens


# ---------------------------------------------------------------------------
def build_table(game_dir):
    con_path = game_dir / "scripts" / "weapons_hud.CON"
    names_path = game_dir / "names.h"
    defs_path = game_dir / "scripts" / "AADEFS.CON"
    quote_paths = [game_dir / "scripts" / "AAUSER.CON", game_dir / "scripts" / "NPCQUOTES.CON"]

    cases_map, default_info, total_case_tokens = parse_switch_block(con_path)
    names_map = parse_names_h(names_path)
    defs_map = parse_defs_con(defs_path)
    quotes = parse_quotes(quote_paths)

    # A DISAGREEMENT CHECK ACROSS BOTH MAPS, not just the names this switch
    # happened to use -- names.h stays PRIMARY either way (a caller never
    # sees the AADEFS.CON value where names.h also has an answer), but a
    # silent disagreement between two of the mod's own files is exactly the
    # thing a future reader needs told rather than finding by accident.
    disagreements = sorted(
        (n, names_map[n], defs_map[n])
        for n in (set(names_map) & set(defs_map))
        if names_map[n] != defs_map[n]
    )

    report = {
        "total_case_tokens": total_case_tokens,
        "default_info": default_info,
        "unresolved_case_names": [],
        "aadefs_resolved": [],
        "disagreements": disagreements,
        "quote_ids_lost_entirely": [],
        "bad_quote_format": [],
        "cond_counts": {},
        "unemitted_case_conditionals": [],
        "override_collisions": [],
    }

    def quote_name(qid, where):
        """A quote id to its house-style nameplate text, or None with the
        failure recorded -- never a mangled string."""
        raw = quotes.get(qid)
        if raw is None:
            report["bad_quote_format"].append((qid, f"no definequote at all ({where})"))
            return None
        body = extract_quote_body(raw)
        if body is None:
            report["bad_quote_format"].append((qid, raw))
            return None
        return title_case(body)

    # id -> set of picnums. names.h first (PRIMARY); AADEFS.CON only for a
    # name names.h does not define at all (SECONDARY).
    id_to_picnums = {}
    picnum_conds = {}
    for cname, (qid, cond) in cases_map.items():
        pic = names_map.get(cname)
        if pic is None:
            pic = defs_map.get(cname)
            if pic is not None:
                report["aadefs_resolved"].append((cname, pic))
        if pic is None:
            report["unresolved_case_names"].append(cname)
            continue
        id_to_picnums.setdefault(qid, set()).add(pic)
        if cond:
            kind = cond["kind"] or cond["lhs"]
            report["cond_counts"][kind] = report["cond_counts"].get(kind, 0) + 1
            if cond["kind"] in EMITTED_COND:
                if pic in picnum_conds and picnum_conds[pic] != cond:
                    report["override_collisions"].append((pic, picnum_conds[pic], cond))
                picnum_conds[pic] = cond
            else:
                # NOT EMITTED, AND SAID OUT LOUD. What is left is the two
                # arms keyed on the episode global `VOLUME`: it is not a
                # per-actor gamevar, so reading it is a different mechanism for
                # one name. They keep their unconditional name and are listed
                # here rather than dropped.
                report["unemitted_case_conditionals"].append(
                    (cname, pic, cond["op"], cond["lhs"], cond["rhs"],
                     cond["value"], quote_name(cond["value"], cname)))

    report["quote_ids_lost_entirely"] = sorted(
        set(q for q, _ in cases_map.values()) - set(id_to_picnums.keys()))

    picnum_to_id = {}
    collisions = []
    for qid, pics in id_to_picnums.items():
        for p in pics:
            if p in picnum_to_id and picnum_to_id[p] != qid:
                collisions.append((p, picnum_to_id[p], qid))
            picnum_to_id[p] = qid
    report["picnum_id_collisions"] = collisions

    table = {}
    for p, qid in picnum_to_id.items():
        nm = quote_name(qid, f"picnum {p}")
        if nm is not None:
            table[p] = nm

    # The emitted overrides, one dict per dimension: {picnum: (op, rhs, name)}.
    overrides = {kind: {} for kind in EMITTED_COND}
    for pic, cond in picnum_conds.items():
        nm = quote_name(cond["value"], f"override on picnum {pic}")
        if nm is not None:
            overrides[cond["kind"]][pic] = (cond["op"], cond["rhs"], nm)

    # The `default` arm. Its base is the `else` value (-1, no nameplate), so
    # only the conditioned half of it becomes a name.
    civilian = None
    if default_info is not None:
        _, dcond = default_info
        if dcond and dcond["kind"] == "team" and dcond["op"] == "ife":
            nm = quote_name(dcond["value"], "the default arm")
            if nm is not None:
                civilian = (dcond["rhs"], nm)

    report["unresolved_case_names"].sort()
    report["aadefs_resolved"].sort()
    report["unemitted_case_conditionals"].sort()
    return table, overrides, civilian, report


# ---------------------------------------------------------------------------
def cond_expr(var, op, rhs):
    """The C++ test for one CON condition. `ifvarand` is a bit test and must
    stay one: `ifvarand mtype 1` holds for 3 as well as for 1."""
    if op == "ifvarand":
        return f"({var} & {rhs}) != 0"
    return f"{var} == {rhs}"


HEADER_TEMPLATE = """#pragma once

// Enemy names read off AA's own in-game nameplates (weapons_hud.CON's switch, AAUSER/NPCQUOTES quotes; names.h is the
// primary picnum source, AADEFS.CON only for names names.h lacks). GENERATED by tools/gen_esp_names_aa.py: DO NOT
// HAND-EDIT. The words are AA's; only the case is ours (title case; EDF, SUV stay upper).

inline const char* ekm_enemy_name_aa(int picnum) {{
  switch (picnum) {{
{cases}
    default: return 0;
  }}
}}

// The pal override, or null when this picnum has none at THIS pal. Apply AFTER ekm_enemy_name_aa: the CON's own order
// (the base `set temp` first, the pal/mtype/team line of the same case overwrites it).
inline const char* ekm_enemy_name_aa_pal(int picnum, int pal) {{
  switch (picnum) {{
{pal_cases}
    default: return 0;
  }}
}}

// The mtype override. `mtype` is a per-actor CON gamevar read through gamevars_mtype, so unlike pal it can answer
// "no answer"; the caller must then keep the unconditional name.
inline const char* ekm_enemy_name_aa_mtype(int picnum, int mtype) {{
  switch (picnum) {{
{mtype_cases}
    default: return 0;
  }}
}}

// The cost gate for that read: gamevars_mtype is a guarded engine read per label per frame, so ask this first.
inline bool ekm_enemy_name_aa_has_mtype(int picnum) {{
  switch (picnum) {{
{mtype_gate}
    default: return false;
  }}
}}

// The team override ON A `case` ARM, not the `default` arm: a matched case beats the default even on team 3, so it
// applies BEFORE the default.
inline const char* ekm_enemy_name_aa_team_case(int picnum, int team) {{
  switch (picnum) {{
{team_cases}
    default: return 0;
  }}
}}

inline bool ekm_enemy_name_aa_has_team(int picnum) {{
  switch (picnum) {{
{team_gate}
    default: return false;
  }}
}}

// The `default` arm is not keyed on picnum, so it is a function of team alone: weapons_hud.CON:2930 names anything
// unmatched "{civ_name}" on team {civ_team} (the neutral non-combatants) and gives no nameplate otherwise.
{civ_block}
"""

CIV_BLOCK = """const int EKM_AA_CIVILIAN_TEAM = {civ_team};

inline const char* ekm_enemy_name_aa_team(int team) {{
  return team == EKM_AA_CIVILIAN_TEAM ? "{civ_name}" : 0;
}}"""

CIV_BLOCK_NONE = """// NOT EMITTED: this install's switch has no `default ife actorvar[monid].team`
// arm, so there is no civilian fallback to express."""


def cpp_string(text):
    """Escape a stray double-quote or backslash, neither of which appears in
    AA's nameplate text today, so this is a safety net rather than a case
    exercised by the current data."""
    return text.replace("\\", "\\\\").replace('"', '\\"')


def render_header(table, overrides, civilian, report, game_dir):
    cases_text = "\n".join(
        f'    case {p}: return "{cpp_string(table[p])}";' for p in sorted(table))

    def override_cases(kind, var):
        rows = overrides.get(kind, {})
        if not rows:
            return "    // (none in this install's switch)"
        out = []
        for p in sorted(rows):
            op, rhs, nm = rows[p]
            out.append(f'    case {p}: return {cond_expr(var, op, rhs)} '
                       f'? "{cpp_string(nm)}" : 0;')
        return "\n".join(out)

    def override_gate(kind):
        rows = overrides.get(kind, {})
        if not rows:
            return "    // (none in this install's switch)"
        return "\n".join(f"    case {p}: return true;" for p in sorted(rows))

    mtype_rows = overrides.get("mtype", {})
    team_rows = overrides.get("team", {})

    unresolved = report["unresolved_case_names"]
    unresolved_text = ", ".join(unresolved) if unresolved else "(none)"

    aadefs = report["aadefs_resolved"]
    aadefs_text = ", ".join(f"{n}={p}" for n, p in aadefs) if aadefs else "(none)"

    disagreements = report["disagreements"]
    disagreement_note = (
        "none -- every name defined in both files agrees on its picnum."
        if not disagreements else
        "; ".join(f"{n}: names.h says {a}, AADEFS.CON says {b} -- names.h's "
                  f"value was used" for n, a, b in disagreements)
    )

    unemitted = report["unemitted_case_conditionals"]
    unemitted_note = "(none)" if not unemitted else "\n//     ".join(
        f"{n} ({p}): `{op} {lhs} {rhs}` would name it \"{nm}\"."
        for n, p, op, lhs, rhs, _qid, nm in unemitted)

    if civilian is None:
        civ_team, civ_name, civ_quote = -1, "(none)", -1
        civ_block = CIV_BLOCK_NONE
    else:
        civ_team, civ_name = civilian
        civ_quote = report["default_info"][1]["value"]
        civ_block = CIV_BLOCK.format(civ_team=civ_team, civ_name=cpp_string(civ_name))

    return HEADER_TEMPLATE.format(
        game_dir=str(game_dir).replace("\\", "/"),
        date="2026-09-20",
        picnum_count=len(table),
        case_token_count=report["total_case_tokens"],
        pal_count=len(overrides.get("pal", {})),
        mtype_count=len(mtype_rows),
        team_count=len(team_rows),
        upper_tokens=", ".join(sorted(UPPER_TOKENS)),
        unresolved_count=len(unresolved),
        unresolved_names=unresolved_text,
        aadefs_resolved_count=len(aadefs),
        aadefs_resolved_names=aadefs_text,
        disagreement_note=disagreement_note,
        unemitted_count=len(unemitted),
        unemitted_note=unemitted_note,
        cases=cases_text,
        pal_cases=override_cases("pal", "pal"),
        mtype_cases=override_cases("mtype", "mtype"),
        mtype_gate=override_gate("mtype"),
        team_cases=override_cases("team", "team"),
        team_gate=override_gate("team"),
        civ_team=civ_team,
        civ_quote=civ_quote,
        civ_name=civ_name,
        civ_block=civ_block,
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-dir", default=os.environ.get("AA_GAME_DIR"),
                    required="AA_GAME_DIR" not in os.environ,
                    help="<AA game folder>, the AA install root (or set AA_GAME_DIR)")
    ap.add_argument("--out", default="src/ekmod/esp_names_aa.h",
                     help="output header path, relative to the repo root")
    args = ap.parse_args()

    game_dir = Path(args.game_dir)
    table, overrides, civilian, report = build_table(game_dir)

    print(f"case tokens in switch:        {report['total_case_tokens']}")
    print(f"picnums emitted:              {len(table)}")
    print(f"conditionals seen, by key:    {report['cond_counts']}")
    print(f"pal overrides emitted:        {len(overrides['pal'])} {sorted(overrides['pal'].items())}")
    print(f"mtype overrides emitted:      {len(overrides['mtype'])} {sorted(overrides['mtype'].items())}")
    print(f"team CASE overrides emitted:  {len(overrides['team'])} {sorted(overrides['team'].items())}")
    print(f"default arm (team):           {civilian}")
    print(f"case conditionals NOT emitted:{len(report['unemitted_case_conditionals'])} {report['unemitted_case_conditionals']}")
    print(f"override collisions:          {report['override_collisions']}")
    print(f"unresolved case names:        {len(report['unresolved_case_names'])} {report['unresolved_case_names']}")
    print(f"resolved via AADEFS.CON:      {len(report['aadefs_resolved'])} {report['aadefs_resolved']}")
    print(f"names.h/AADEFS.CON disagree:  {report['disagreements']}")
    print(f"quote ids lost entirely:      {report['quote_ids_lost_entirely']}")
    print(f"bad-format quotes:            {report['bad_quote_format']}")
    print(f"picnum/id collisions:         {report['picnum_id_collisions']}")

    out_text = render_header(table, overrides, civilian, report, game_dir)

    out_path = Path(args.out)
    # CRLF explicitly: every other source file in this repo is CRLF, and the
    # Write path (and a plain text-mode write) emits LF. newline='' plus '\r\n'
    # line endings keep the file `w/crlf`, per the repo's line-ending rule.
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        f.write(out_text.replace("\n", "\r\n"))

    print(f"wrote {out_path}")


if __name__ == "__main__":
    sys.exit(main())
