"""Pure model of the fire-rate advance, old rule (park nxt-1 everywhere) vs the
split (park ON engine stops, nxt-1 on CON stops, TotalTime-1 always CON).
Held fire, one advance per tic after the tic's observation. Analysis only.
  python tools/firerate_split_model.py <AA game folder>   (or set AA_GAME_DIR)"""
import os, re, math, sys

_game = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("AA_GAME_DIR")
if not _game:
    sys.exit("usage: firerate_split_model.py <AA game folder>  (or set AA_GAME_DIR)")
LOG = os.path.join(_game, "ekmod.log")
CON = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "ekmod", "con_stops_aa.h")
RESET, AUTOMATIC, SEMIAUTO = 0x10000, 0x4, 0x4000  # firestops.h:45,57,59

rows, stops = {}, {}
for line in open(LOG, encoding="utf-8", errors="replace"):
    m = re.search(r"weapondata: w(\d+) AA live FIREDELAY=(\d+) TOTALTIME=(\d+) .*?HOLDDELAY=(\d+) FLAGS=(\d+)", line)
    if m: rows[int(m.group(1))] = dict(tt=int(m.group(3)), hd=int(m.group(4)), fl=int(m.group(5)))
    m = re.search(r"firestops: w(\d+) derived \d+ stop\(s\): ([\d,]+)", line)
    if m: stops[int(m.group(1))] = [int(x) for x in m.group(2).split(",")]
con = {}
for line in open(CON, encoding="utf-8"):
    m = re.search(r"kW(\d+)\[\d+\] = \{([^}]*)\}", line)
    if m: con[int(m.group(1))] = {int(x) for x in m.group(2).split(",")}

def cycle_end(r):
    tt, hd, fl = r["tt"], r["hd"], r["fl"]
    if (fl & RESET) and (fl & AUTOMATIC) and not (fl & SEMIAUTO):
        return min(tt - hd, tt - 1)
    return tt - 1

def advance(events, k, steps, split):
    if k <= 0 or steps <= 0: return k
    nxt = next(((v, c) for v, c in events if v >= k), None)
    if nxt is None: return k
    v, is_con = nxt
    stop = v - 1 if (not split or is_con) else v
    if stop <= k: return k
    return min(k + steps, stop)

def always_observed(events, end, rate, split, cycles=24):
    carry, seen_all = 0.0, None
    for _ in range(cycles):
        k, seen = 1, {1}
        while True:
            carry += (rate - 100) / 100.0
            n = math.floor(carry); carry -= n
            k = advance(events, k, n, split) + 1
            if k > end: break
            seen.add(k)
        seen_all = seen if seen_all is None else seen_all & seen
    return seen_all

print("wNN end  rate  old-observed            new-observed        LOST (CON frames only)")
for w in sorted(stops):
    if w not in rows: continue
    r, tt = rows[w], rows[w]["tt"]
    end = cycle_end(r)
    ev = sorted({(s, s > tt) for s in stops[w]} | {(tt - 1, True)})
    ev = sorted({v: max(c for vv, c in ev if vv == v) for v, _ in ev}.items())
    for rate in (200, 300, 400):
        old = always_observed(ev, end, rate, False)
        new = always_observed(ev, end, rate, True)
        lost = sorted((old - new) & con.get(w, set()))
        print(f"w{w:02d} {end:>3} {rate:>5}  {str(sorted(old)):<24}{str(sorted(new)):<20}{lost}  "
              f"{end}/{len(old)}={end/len(old):.2f}x -> {end/len(new):.2f}x")
# --- D15 Part A (2026-09-24): provenance FLAGS, identity check -----------------
# ENGINE parks ON, any CON flag parks one below, and only a CON_FRAME-only value
# at k is consumed. TotalTime-1 is ENGINE|CON_FRAME and every other `c` stop is
# CON_EDGE, so no live value is CON_FRAME-only: the speed-up must not move.
ENGINE, CON_EDGE, CON_FRAME = 4, 1, 2
live, live_flags = {}, {}

def parse_stop(x):
    """One logged stop. Since D15 Part B (2026-09-24) the letters are every flag:
    E engine beside a CON flag, F CON_FRAME, c CON_EDGE; bare is ENGINE alone.
    An older log's bare `c` on TotalTime-1 is rebuilt by flags_of below."""
    m = re.fullmatch(r"(\d+)(E?)(F?)(c?)", x)
    v, e, f, c = int(m.group(1)), m.group(2), m.group(3), m.group(4)
    fl = (CON_FRAME if f else 0) | (CON_EDGE if c else 0)
    return v, (fl | ENGINE) if (e or not fl) else fl

for line in open(LOG, encoding="utf-8", errors="replace"):
    m = re.search(r"firestops: w(\d+) derived \d+ stop\(s\): ([\dcEF,]+)", line)
    if m:
        w, xs = int(m.group(1)), m.group(2).split(",")
        live[w] = [(int(re.match(r"\d+", x).group(0)), bool(re.search(r"[cF]", x))) for x in xs]
        live_flags[w] = ([parse_stop(x) for x in xs]
                         if any(("E" in x or "F" in x) for x in xs) else None)

def flags_of(w):
    if live_flags.get(w): return live_flags[w]
    tt = rows[w]["tt"]
    return [(v, (ENGINE | CON_FRAME) if (c and v == tt - 1) else (CON_EDGE if c else ENGINE))
            for v, c in live[w]]

def advance_flags(events, k, steps):
    if k <= 0 or steps <= 0: return k
    i = 0
    while i < len(events) and events[i][0] < k: i += 1
    if i < len(events) and events[i][0] == k and events[i][1] == CON_FRAME: i += 1
    if i >= len(events): return k
    v, f = events[i]
    stop = v - 1 if f & (CON_EDGE | CON_FRAME) else v
    return k if stop <= k else min(k + steps, stop)

def advance_split(events, k, steps):
    return advance(events, k, steps, True)

def mean_tics(adv, events, end, rate, cycles=48):
    carry, total = 0.0, 0
    for _ in range(cycles):
        k, tics = 1, 0
        while k <= end and tics < 64:
            carry += (rate - 100) / 100.0
            n = math.floor(carry); carry -= n
            k = adv(events, k, n) + 1
            tics += 1
        total += tics
    return total / cycles

print()
print("D15 Part A identity: normalised speed-up (100% = 1.00x), split rule -> flags rule")
for w in sorted(live):
    if w not in rows: continue
    end = cycle_end(rows[w])
    split_ev = [(v, c) for v, c in live[w]]
    flag_ev = flags_of(w)
    cells, same = [], True
    base_old = mean_tics(advance_split, split_ev, end, 100)
    base_new = mean_tics(advance_flags, flag_ev, end, 100)
    for rate in (150, 200, 300, 400):
        a = base_old / mean_tics(advance_split, split_ev, end, rate)
        b = base_new / mean_tics(advance_flags, flag_ev, end, rate)
        same &= abs(a - b) < 1e-9
        cells.append("%d%% %.2f->%.2f" % (rate, a, b))
    only = [v for v, f in flag_ev if f == CON_FRAME]
    print("w%02d end %2d  %s  %s  CON_FRAME-only: %s" % (w, end, "  ".join(cells),
          "IDENTICAL" if same else "CHANGED", only or "none"))

# --- D15 Part B (2026-09-24): shot rows by configuration ----------------------
# Derives each configuration's set the way firestops.h does, then plays cycles.
# FASTFIRE IS MODELLED: AAPLAYER.CON:8848-8930 adds one to kickback_pic every
# tic while fastfire > 0, unless the engine left it at FIREDELAY-1, before the
# shot tests read it -- so the CON sees the hook's k plus two.
RELOAD_TIMING, STANDSTILL, EVERYOTHER, EVERYTHIRD = 0x8000, 0x800, 0x8, 0x10

def derive(fd, tt, rl, st, s2, hd, fl, w, extra):
    ev = {}
    def add(v, f=ENGINE):
        if v >= 1: ev[v] = ev.get(v, 0) | f
    for v in (fd - 1, tt - 1, rl - 1, st - 1, s2 - 1): add(v)
    if fl & RESET: add(max(min(tt - hd, tt - 1), fd - 1))
    if fl & STANDSTILL:
        for k in range(1, fd): add(k)
    if (fl & AUTOMATIC) and (fl & EVERYTHIRD):
        for f in range(fd, tt):
            if f % 3 == 0: add(f - 1)
    if (fl & AUTOMATIC) and (fl & EVERYOTHER):
        for f in range(fd, tt): add(f - 1)
    if rl > tt:
        add(tt)
        rt = rl - tt
        add((rl - rt + 4 if fl & RELOAD_TIMING else rl - rt // 3) - 1)
    add(tt - 1, CON_FRAME)
    for s in con.get(w, ()):
        if s > tt: add(s, CON_EDGE)
    for v, f in extra: add(v, f)
    return sorted(ev.items())

def shot_extra(idle, fast, ff, safe):
    """ekm_shot_apply's placement for one open gate."""
    if ff: return [(fast - 1, CON_EDGE)]
    return [(idle, CON_FRAME | (0 if safe else CON_EDGE))]

def play(events, fd, tt, rate, ff, cycles=48):
    end = tt - 1
    def tic(k):
        k += 1
        return k + 1 if (ff and k != fd - 1) else k
    carry, total, seen_all = 0.0, 0, None
    for _ in range(cycles):
        k = tic(0); seen, tics = {k}, 0
        while k <= end and tics < 64:
            carry += (rate - 100) / 100.0
            n = math.floor(carry); carry -= n
            k = tic(advance_flags(events, k, n))
            tics += 1
            seen.add(k)
        total += tics
        seen_all = seen if seen_all is None else seen_all & seen
    return total / cycles, seen_all

CONFIGS = [  # name, (fd, tt, rl, st, s2, hd, fl), weapon, shot frame, ff, row
    ("plain pistol",        (2, 9, 34, 2, 0, 0, RELOAD_TIMING), 1, 2, 0, None),
    ("dual Deagles ff 0",   (2, 9, 58, 2, 0, 0, RELOAD_TIMING), 1, 6, 0, (6, 5, True)),
    ("dual Deagles ff 1",   (2, 9, 58, 2, 0, 0, RELOAD_TIMING), 1, 5, 1, (6, 5, True)),
    ("Duke dual 1911",      (2, 6, 48, 2, 0, 0, RELOAD_TIMING), 1, 5, 0, (5, 5, False)),
    ("Wes revolvers",       (2, 9, 34, 2, 0, 0, RELOAD_TIMING), 1, 7, 0, (7, 7, False)),
    ("Mando shotgun (noDB)", (3, 26, 12, 18, 12, 0, 1024),      2, 6, 0, (6, 6, True)),
]
print()
print("D15 Part B: normalised speed-up (100% = 1.00x), and whether the shot frame")
print("is seen in 100% of cycles -- Part A alone -> with the shot row")
for name, (fd, tt, rl, st, s2, hd, fl), w, shot, ff, row in CONFIGS:
    before = derive(fd, tt, rl, st, s2, hd, fl, w, [])
    extra = shot_extra(row[0], row[1], ff, row[2]) if row else []
    after = derive(fd, tt, rl, st, s2, hd, fl, w, extra)
    b100, _ = play(before, fd, tt, 100, ff)
    a100, _ = play(after, fd, tt, 100, ff)
    cells = []
    for rate in (150, 200, 300, 400):
        bt, bs = play(before, fd, tt, rate, ff)
        at, as_ = play(after, fd, tt, rate, ff)
        cells.append("%d%% %.2f%s->%.2f%s" % (rate, b100 / bt, "" if shot in bs else "!",
                                             a100 / at, "" if shot in as_ else "!"))
    added = ",".join("%d%s%s" % (v, "F" if f & CON_FRAME else "", "c" if f & CON_EDGE else "")
                     for v, f in extra) or "none"
    print("%-21s frame %2d  +%-4s %s" % (name, shot, added, "  ".join(cells)))
print("('!' marks a cycle set where the shot frame was NOT seen in every cycle)")
