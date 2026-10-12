#pragma once

// AA volume 4 worm boss: three `useractor notenemy` parts, so g_tile flags none; each part's damage is switched by two
// per-actor vars, so it is not in ekm_aa_hostile_notenemy's list.
const int EK_AA_PODHEAD = 22323;
const int EK_AA_PODBODY = 22324;
const int EK_AA_PODTAIL = 22325;

enum { EKM_WORM_PART_NONE = 0, EKM_WORM_PART_HEAD, EKM_WORM_PART_BODY,
       EKM_WORM_PART_TAIL };

// The tail's picnum moves (killing it turns its spawner body into PODTAIL): read the part off it every frame.
inline int ekm_worm_part(int picnum) {
  switch (picnum) {
    case EK_AA_PODHEAD: return EKM_WORM_PART_HEAD;
    case EK_AA_PODBODY: return EKM_WORM_PART_BODY;
    case EK_AA_PODTAIL: return EKM_WORM_PART_TAIL;
    default:            return EKM_WORM_PART_NONE;
  }
}

// Head and tail, never the body: PODBODY has `cstat 0` and no `ifhitweapon`, so nothing can hit it.
inline bool ekm_worm_admitted(int picnum) {
  const int part = ekm_worm_part(picnum);
  return part == EKM_WORM_PART_HEAD || part == EKM_WORM_PART_TAIL;
}

enum { EKM_WORM_NONE = 0, EKM_WORM_VULNERABLE, EKM_WORM_STUN_IT,
       EKM_WORM_STUNNED, EKM_WORM_RECOVERING, EKM_WORM_SHIELDED };

const int EKM_WORM_HEAD_EXPOSED = -99;

struct EkWormVerdict {
  int state;
  int target;
};

inline EkWormVerdict ekm_worm_state(int part, int countvar, int dashtime) {
  EkWormVerdict v = { EKM_WORM_NONE, 0 };
  if (part == EKM_WORM_PART_HEAD) {
    // Head stopped for the player: a hit kills. Checked first; a stunned head's dashtime stays frozen above 0.
    if (countvar == EKM_WORM_HEAD_EXPOSED) { v.state = EKM_WORM_VULNERABLE; v.target = 1; }
    else if (countvar >= 0 && dashtime == 0) { v.state = EKM_WORM_STUN_IT; v.target = 1; }
    else if (dashtime > 0) v.state = EKM_WORM_STUNNED;
    else v.state = EKM_WORM_RECOVERING;
  } else if (part == EKM_WORM_PART_TAIL) {
    if (dashtime > 0) { v.state = EKM_WORM_VULNERABLE; v.target = 1; }
    else v.state = EKM_WORM_SHIELDED;
  }
  return v;
}

// A failed read is no verdict: callers refuse the part as before, so a CON without these vars changes nothing.
inline EkWormVerdict ekm_worm_state_read(int part, bool gotCountvar, int countvar,
                                         bool gotDashtime, int dashtime) {
  const bool enough = part == EKM_WORM_PART_HEAD ? (gotCountvar && gotDashtime)
                    : part == EKM_WORM_PART_TAIL ? gotDashtime
                                                 : false;
  if (!enough) {
    EkWormVerdict none = { EKM_WORM_NONE, 0 };
    return none;
  }
  return ekm_worm_state(part, countvar, dashtime);
}
