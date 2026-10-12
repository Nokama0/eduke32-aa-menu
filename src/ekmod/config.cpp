// ekmod.ini, beside eduke32.exe. Written key by key (WritePrivateProfileStringA), not rewritten whole: that would drop
// the player's comments and keys a NEWER build wrote. Read by a hand-rolled key=value parse that enforces the int-only
// shape.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "config.h"
#include "esp.h"
#include "tbot.h"
#include "autosave.h"
#include "weaponspeed.h"
#include "freecam.h"
#include "cutaway.h"
#include "nocull.h"
#include "fov.h"
#include "coinmagnet.h"
#include "pickuprange.h"
#include "skipintro.h"

Config g_cfg;
int g_cfgDirty = 0;

static Config s_onDisk;
static char s_path[MAX_PATH];

static bool ini_path(void) {
  if (s_path[0]) return true;
  const DWORD n = GetModuleFileNameA(NULL, s_path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) { s_path[0] = 0; return false; }
  char* slash = strrchr(s_path, '\\');
  if (!slash) { s_path[0] = 0; return false; }
  slash[1] = 0;
  if (strlen(s_path) + 11 >= MAX_PATH) { s_path[0] = 0; return false; }
  strcat_s(s_path, MAX_PATH, "ekmod.ini");
  return true;
}

// One table walked by reader and writer: two lists would drift, and a field readable but not writable only shows up
// after a restart.
struct Field { const char* key; int Config::* member; };
static const Field kFields[] = {
  { "menukey",     &Config::menukey     },
  { "uiscale",     &Config::uiscale     },
  { "panelx",      &Config::panelx      },
  { "panely",      &Config::panely      },
  { "panelw",      &Config::panelw      },
  { "panelh",      &Config::panelh      },
  { "paneltab",    &Config::paneltab    },
  { "animations",  &Config::animations  },
  { "skipintro",   &Config::skipintro   },
  { "esp",         &Config::esp         },
  { "espboxes",    &Config::espboxes    },
  { "esphealthbar",&Config::esphealthbar},
  { "espname",     &Config::espname     },
  { "espdistance", &Config::espdistance },
  { "esplevel",    &Config::esplevel    },
  { "espfriendlies",        &Config::espfriendlies        },
  { "espfriendlyboxes",     &Config::espfriendlyboxes     },
  { "espfriendlyhealthbar", &Config::espfriendlyhealthbar },
  { "espfriendlyname",      &Config::espfriendlyname      },
  { "espfriendlydistance",  &Config::espfriendlydistance  },
  { "espfriendlylevel",     &Config::espfriendlylevel     },
  { "espitems",    &Config::espitems    },
  { "espitemshidefull", &Config::espitemshidefull },
  { "espfigurines",    &Config::espfigurines    },
  { "figurinecounter", &Config::figurinecounter },
  { "espchests",       &Config::espchests       },
  { "cornerscale", &Config::cornerscale },
  { "hivis",       &Config::hivis       },
  { "espsecrets",  &Config::espsecrets  },
  { "secretvolume",   &Config::secretvolume   },
  { "secrettriggers", &Config::secrettriggers },
  { "secretpath",     &Config::secretpath     },
  { "dmgnumbers",     &Config::dmgnumbers     },
  { "espexplodables", &Config::espexplodables },
  { "espbreakables",  &Config::espbreakables  },
  { "espcash",        &Config::espcash        },
  { "espswitches",    &Config::espswitches    },
  { "esptint",        &Config::esptint        },
  { "espdrawdist", &Config::espdrawdist },
  { "espnolimit",  &Config::espnolimit  },
  { "aimon",       &Config::aimon       },
  { "aimhold",     &Config::aimhold     },
  { "aimsmooth",   &Config::aimsmooth   },
  { "aimtarget",   &Config::aimtarget   },
  { "aimpoint",    &Config::aimpoint    },
  { "aimcone",     &Config::aimcone     },
  { "aimrange",    &Config::aimrange    },
  { "aimlead",     &Config::aimlead     },
  { "aimleadscale",&Config::aimleadscale},
  { "aimdebug",    &Config::aimdebug    },
  { "accpistol",   &Config::accpistol   },
  { "accchaingun", &Config::accchaingun },
  { "accshotgun",  &Config::accshotgun  },
  { "accexpander", &Config::accexpander },
  { "accincinerator", &Config::accincinerator },
  { "norecoil",    &Config::norecoil    },
  { "firerate",    &Config::firerate    },
  { "switchspeed", &Config::switchspeed },
  { "reloadrate",  &Config::reloadrate  },
  { "coinmagnet",  &Config::coinmagnet  },
  { "coinspeed",   &Config::coinspeed   },
  { "coinheal",    &Config::coinheal    },
  { "pickuprange", &Config::pickuprange },
  { "silenton",    &Config::silenton    },
  { "silentmaxangle", &Config::silentmaxangle },
  { "tboton",      &Config::tboton      },
  { "tbotwhen",    &Config::tbotwhen    },
  { "tbotdelay",   &Config::tbotdelay   },
  { "autosavemins",&Config::autosavemins},
  { "fovon",       &Config::fovon       },
  { "fov",         &Config::fov         },
  { "fovstashheld",   &Config::fovstashheld   },
  { "fovstashsaved",  &Config::fovstashsaved  },
  { "fovstashforced", &Config::fovstashforced },
  { "freecamon",   &Config::freecamon   },
  { "freecamspeed",&Config::freecamspeed},
  { "freecamwholemap", &Config::freecamwholemap },
  { "freecamcutawaysky", &Config::freecamcutawaysky },
  { "freecamtrornodraw", &Config::freecamtrornodraw },
  { "freecamsweepbudget", &Config::freecamsweepbudget },
  { "freecamsweeporder", &Config::freecamsweeporder },
  { "freecamsweeppasses", &Config::freecamsweeppasses },
  { "freecamhideceilings", &Config::freecamhideceilings },
  { "freecamcutawayparts", &Config::freecamcutawayparts },
  { "freecamcutawayunclamp", &Config::freecamcutawayunclamp },
  { "freecamcutawaytallwall", &Config::freecamcutawaytallwall },
  { "freecamcutawaymaskwall", &Config::freecamcutawaymaskwall },
  { "freecamsubst", &Config::freecamsubst },
  { "freecamud",   &Config::freecamud    },
  { "freecamfog",  &Config::freecamfog   },
  { "freecamfullbright", &Config::freecamfullbright },
  { "freecamshot", &Config::freecamshot  },
  { "freecamtransport", &Config::freecamtransport },
  { "freecamshowplayer", &Config::freecamshowplayer },
  { "freecamdebug", &Config::freecamdebug },
  { "freecamteleportmarker", &Config::freecamteleportmarker },
  { "shotstashheld",      &Config::shotstashheld      },
  { "shotstashmask",      &Config::shotstashmask      },
  { "shotstashcrosshair", &Config::shotstashcrosshair },
  { "shotstashweapon",    &Config::shotstashweapon    },
  { "shotstashstats",     &Config::shotstashstats     },
  { "debuglog",    &Config::debuglog    },
  { "spiketotaltime", &Config::spiketotaltime },
  { "tracers",     &Config::tracers     },
  { "perfzones",   &Config::perfzones   },
  { "perfdetail2", &Config::perfdetail2 },
  { "perftable",   &Config::perftable   },
  { "perfskip",    &Config::perfskip    },
  { "bindaim",     &Config::bindaim     },
  { "bindcamera",  &Config::bindcamera  },
  { "bindteleport",&Config::bindteleport},
  { "bindcamcopy", &Config::bindcamcopy  },
  { "bindcampaste",&Config::bindcampaste },
};
static const int kFieldCount = (int)(sizeof(kFields) / sizeof(kFields[0]));

void config_defaults(void) {
  g_cfg.menukey    = 0x2D;   // VK_INSERT, the same key on all three games.
  g_cfg.uiscale    = 100;
  g_cfg.panelx = -1; g_cfg.panely = -1;
  g_cfg.panelw = 0;  g_cfg.panelh = 0;
  g_cfg.paneltab   = 0;
  g_cfg.animations = 1;
  g_cfg.skipintro  = 0;

  // Features default OFF: out of the box the mod changes nothing. The ESP's sub-rows default ON so switching the parent
  // on shows something rather than an empty screen.
  g_cfg.esp          = 0;
  g_cfg.espboxes     = 1;
  g_cfg.esphealthbar = 1;
  g_cfg.espname      = 1;
  g_cfg.espdistance  = 0;
  // Sub-row of an off parent is presentation, not an exception to the rule above; it shows nothing where the game has
  // no such gamevar.
  g_cfg.esplevel     = 1;
  // The four sub-rows deliberately mirror the enemy group's defaults: do not "fix" them to 0, or switching Friendlies
  // on would draw nothing.
  g_cfg.espfriendlies        = 0;
  g_cfg.espfriendlyboxes     = 1;
  g_cfg.espfriendlyhealthbar = 1;
  g_cfg.espfriendlyname      = 1;
  g_cfg.espfriendlydistance  = 0;
  g_cfg.espfriendlylevel     = 1;
  g_cfg.espitems     = 0;
  g_cfg.espitemshidefull = 0;
  g_cfg.espfigurines    = 0;
  g_cfg.figurinecounter = 0;
  g_cfg.espchests       = 0;
  g_cfg.cornerscale     = 150;
  g_cfg.hivis           = 0;
  g_cfg.espsecrets   = 0;
  g_cfg.secretvolume = 0;
  g_cfg.secrettriggers = 0;
  g_cfg.secretpath     = 0;
  g_cfg.dmgnumbers     = 0;
  g_cfg.espexplodables = 0;
  g_cfg.espbreakables  = 0;
  g_cfg.espcash        = 0;
  g_cfg.espswitches    = 0;
  g_cfg.esptint        = 0;
  g_cfg.espdrawdist  = ESP_DRAWDIST_DEFAULT;
  g_cfg.espnolimit   = 0;

  // Aimbot off, shaped as a player switching it on would want: hold to aim, a snap onto the crosshair target.
  g_cfg.aimon = 0; g_cfg.aimhold = 1; g_cfg.aimsmooth = 0;
  g_cfg.aimtarget = 0; g_cfg.aimpoint = 0;
  g_cfg.aimcone = 30; g_cfg.aimrange = 32768;
  g_cfg.aimlead = 1; g_cfg.aimleadscale = 70;
  g_cfg.aimdebug = 0;

  // All four off: P_PreFireHitscan's detour is a pass-through.
  g_cfg.accpistol = 0; g_cfg.accchaingun = 0;
  g_cfg.accshotgun = 0; g_cfg.accexpander = 0;
  g_cfg.accincinerator = 0;
  g_cfg.norecoil = 0;

  // For these rows "off" is the identity value: at 100 percent weaponspeed.cpp writes no byte.
  g_cfg.firerate = FIRERATE_DEFAULT;
  g_cfg.switchspeed = FIRERATE_DEFAULT;
  g_cfg.reloadrate = FIRERATE_DEFAULT;
  g_cfg.coinmagnet = EK_CM_OFF;
  g_cfg.coinspeed  = EK_CS_100;
  g_cfg.coinheal   = 0;
  g_cfg.pickuprange = EK_PR_OFF;

  // 45 degrees: wide enough to cover what the player could mean, narrow enough that a shot at a wall stays one.
  g_cfg.silenton = 0; g_cfg.silentmaxangle = 45;

  // The only feature that presses a key. Crosshair mode and no delay: fire at what I am pointing at, now.
  g_cfg.tboton = 0;
  g_cfg.tbotwhen = TBOT_WHEN_CROSSHAIR;
  g_cfg.tbotdelay = TBOT_DELAY_MS_DEFAULT;

  g_cfg.autosavemins = AUTOSAVE_MINS_DEFAULT;

  // With the FOV row off the cvar is never read, so the player's own fov is untouched (fov.h); the default is the stock
  // 90.
  g_cfg.fovon = 0;
  g_cfg.fov = EK_FOV_DEFAULT;
  g_cfg.fovstashheld = 0;
  g_cfg.fovstashsaved = 0;
  g_cfg.fovstashforced = 0;

  g_cfg.freecamon = 0;
  g_cfg.freecamspeed = FREECAM_SPEED_DEFAULT;
  // The whole-map sweep is its own row, off while flying: it draws every sector a second time.
  g_cfg.freecamwholemap = 0;
  // The cutaway's sky is its own row; 0 is the suppressing state (the sky's domost calls never reach the engine): the
  // clean black sky.
  g_cfg.freecamcutawaysky = 0;
  g_cfg.freecamtrornodraw = 1;
  // Full is safe on this binary because polymost.cpp:6673 bounds numscans; Reference's half-of-free rule stopped short
  // of a map needing almost all of it (the flicker). Reference stays selectable.
  g_cfg.freecamsweepbudget = EK_NOCULL_BUDGET_FULL;
  g_cfg.freecamsweeporder = EK_NOCULL_SWEEPORDER_NEAREST;
  g_cfg.freecamsweeppasses = FREECAM_PASSES_TWO_WHEN_NEEDED;
  // Out of bounds a ceiling is only seen from under its extended slope and pops at that line.
  g_cfg.freecamhideceilings = FREECAM_CEILINGS_HIDE;
  g_cfg.freecamcutawayparts = CUTAWAY_PARTS_BOTH;
  g_cfg.freecamcutawayunclamp = 0;
  g_cfg.freecamcutawaytallwall = CUTAWAY_TALL_WALL_OFF;
  // Off forwards to the engine's polymost_drawmaskwallinternal; on, the masked geometry is gone (a measurement, not a
  // setting).
  g_cfg.freecamcutawaymaskwall = 0;
  g_cfg.freecamsubst = FREECAM_SUBST_ALL;
  g_cfg.freecamud = FREECAM_UD_ALL;
  g_cfg.freecamfog = 0;
  g_cfg.freecamfullbright = 0;
  g_cfg.freecamshot = 0;
  g_cfg.freecamtransport = 0;
  g_cfg.freecamshowplayer = 0;
  g_cfg.freecamdebug = 0;
  g_cfg.freecamteleportmarker = 0;
  g_cfg.shotstashheld = 0;
  g_cfg.shotstashmask = 0;
  g_cfg.shotstashcrosshair = 0;
  g_cfg.shotstashweapon = 0;
  g_cfg.shotstashstats = 0;

  g_cfg.debuglog = 0;
  g_cfg.spiketotaltime = 0;

  g_cfg.tracers = 0;

  g_cfg.perfzones = 0;
  g_cfg.perfdetail2 = 0;
  g_cfg.perftable = 0;
  g_cfg.perfskip = 0;
  g_cfg.secvolnodepth = 0;

  // 0 is "unbound", rendered empty by bind.h; bindteleport ships unbound so it takes no key from the game.
  g_cfg.bindaim = g_cfg.bindteleport = 0;

  // T is World Tour's default. input.cpp takes it only while freecamon is set, so a player not using the camera keeps
  // T.
  g_cfg.bindcamera = 0x54;

  // B and V: the panel has no modifier in a binding, so Ctrl+C/V cannot be expressed, and C (Toggle_Crouch) and X
  // (Last_Used_Weapon) are taken in the shipped binds. V and B are unbound and sit under the left hand on WASD; taken
  // only while freecamon is set.
  g_cfg.bindcamcopy  = 0x42;
  g_cfg.bindcampaste = 0x56;
}

bool config_load(void) {
  config_defaults();
  if (!ini_path()) { s_onDisk = g_cfg; return false; }

  FILE* f = NULL;
  if (fopen_s(&f, s_path, "r") != 0 || !f) {
    s_onDisk = g_cfg;
    return false;
  }

  char line[CONFIG_LINE_BUFSIZE];
  int read = 0, lineNo = 0;
  for (;;) {
    const int got = config_read_line([f] { return fgetc(f); }, line, CONFIG_LINE_BUFSIZE);
    if (got == CONFIG_LINE_END) break;
    lineNo++;
    if (got == CONFIG_LINE_OVERLONG) {
      ek_log("config: line %d is over %d bytes; skipped", lineNo, CONFIG_LINE_BUFSIZE - 1);
      continue;
    }
    char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 0 || *p == ';' || *p == '#' || *p == '[') continue;

    char* eq = strchr(p, '=');
    if (!eq) continue;
    *eq = 0;
    char* key = p;
    char* val = eq + 1;

    char* end = key + strlen(key);
    while (end > key && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;

    for (int i = 0; i < kFieldCount; i++) {
      if (_stricmp(key, kFields[i].key) != 0) continue;
      g_cfg.*(kFields[i].member) = (int)strtol(val, NULL, 0);
      read++;
      break;
    }
  }
  fclose(f);

  // Clamped so a hand-edited index cannot reach a table lookup; the ini is rewritten only on a real change.
  g_cfg.coinmagnet = coinmagnet_clamp(g_cfg.coinmagnet);
  g_cfg.coinspeed  = coinspeed_clamp(g_cfg.coinspeed);
  g_cfg.coinheal   = g_cfg.coinheal ? 1 : 0;
  g_cfg.pickuprange = pickuprange_clamp(g_cfg.pickuprange);

  s_onDisk = g_cfg;
  g_cfgDirty = 0;
  ek_log("config: read %d key(s) from %s", read, s_path);
  return true;
}

// One key read at DLL attach, because G_DisplayLogo reads g_noLogo before config_load runs; same line rules as there
// (skipintro.h), last matching line wins.
int config_peek_skipintro(void) {
  if (!ini_path()) return 0;
  FILE* f = NULL;
  if (fopen_s(&f, s_path, "r") != 0 || !f) return 0;
  char line[CONFIG_LINE_BUFSIZE];
  int on = 0;
  for (;;) {
    // An over-long line is skipped whole here too, unlogged: this runs under the loader lock, before config_load.
    const int got = config_read_line([f] { return fgetc(f); }, line, CONFIG_LINE_BUFSIZE);
    if (got == CONFIG_LINE_END) break;
    if (got == CONFIG_LINE_OVERLONG) continue;
    const char* const v = ekm_ini_value_for(line, "skipintro");
    if (v) on = ekm_skipintro_on(v);
  }
  fclose(f);
  return on;
}

void config_write_changed(void) {
  if (!g_cfgDirty) return;
  g_cfgDirty = 0;
  if (!ini_path()) return;

  int wrote = 0;
  for (int i = 0; i < kFieldCount; i++) {
    const int now = g_cfg.*(kFields[i].member);
    if (now == s_onDisk.*(kFields[i].member)) continue;
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", now);
    if (WritePrivateProfileStringA("ekmod", kFields[i].key, buf, s_path)) {
      s_onDisk.*(kFields[i].member) = now;
      wrote++;
    } else {
      ek_log("config: could not write %s (%lu)", kFields[i].key, GetLastError());
      g_cfgDirty = 1;
    }
  }
  if (wrote) ek_log("config: wrote %d changed key(s)", wrote);
}

// bind.h addresses binds by row; row 0 is the panel key (menukey, the field the ini always used).
int config_bind_get(int row) {
  switch (row) {
    case 0:  return g_cfg.menukey;
    case 1:  return g_cfg.bindaim;
    case 2:  return g_cfg.bindcamera;
    case 3:  return g_cfg.bindteleport;
    case 4:  return g_cfg.bindcamcopy;
    case 5:  return g_cfg.bindcampaste;
    default: return 0;
  }
}

void config_bind_set(int row, int vk) {
  switch (row) {
    case 0:  g_cfg.menukey = vk; break;
    case 1:  g_cfg.bindaim = vk; break;
    case 2:  g_cfg.bindcamera = vk; break;
    case 3:  g_cfg.bindteleport = vk; break;
    case 4:  g_cfg.bindcamcopy = vk; break;
    case 5:  g_cfg.bindcampaste = vk; break;
    default: return;
  }
  g_cfgDirty = 1;
}
