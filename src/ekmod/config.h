#pragma once

// The mod's own settings file, ekmod.ini beside eduke32.exe, scoped to what exists. Every feature defaults off unless
// a row says otherwise.

const int CONFIG_LINE_BUFSIZE = 512;

enum { CONFIG_LINE_END = -1, CONFIG_LINE_OVERLONG = -2 };

// One line from next() (a getc: a byte, or -1 at the end) into line[cap], '\n' dropped. Returns its length,
// CONFIG_LINE_END, or CONFIG_LINE_OVERLONG for a line over cap - 1 bytes, read to its end and dropped: cut, its tail
// would parse as a line of its own.
template <class Next> inline int config_read_line(Next next, char* line, int cap) {
  if (!line || cap < 1) return CONFIG_LINE_END;
  int len = 0;
  bool any = false, over = false;
  for (;;) {
    const int c = next();
    if (c < 0) break;
    any = true;
    if (c == '\n') break;
    if (len < cap - 1) line[len++] = (char)c;
    else over = true;
  }
  line[len] = 0;
  if (!any) return CONFIG_LINE_END;
  return over ? CONFIG_LINE_OVERLONG : len;
}

struct Config {
  int menukey;    // virtual-key code that opens the panel; default VK_INSERT (0x2D)
  int uiscale;  // percent, UI_SCALE_MIN..UI_SCALE_MAX (ui.h), default 100
  // Stored panel box, logical pixels at UI scale 100. -1/-1/0/0 is "unset" (0,0 is a real corner; no panel is
  // zero-sized).
  int panelx, panely, panelw, panelh;
  int paneltab;
  int animations;
  // The engine's own -nologo, a persistent preference read alone at the next DLL attach (skipintro.h); the row only
  // stores it.
  int skipintro;

  int esp;
  int espboxes;
  int esphealthbar;
  int espname;
  int espdistance;
  // AA's rolled monster level, before the name as AA's nameplate has it. Defaults ON (the parent still defaults off,
  // and the row shows nothing under stock Duke).
  int esplevel;
  // Own parent, not a sub-row of enemies: allies and civilians leave the hostile list entirely (targets.cpp), so with
  // this off nothing draws them, and friendlies-only must work with Enemies off.
  int espfriendlies;
  int espfriendlyboxes;
  int espfriendlyhealthbar;
  int espfriendlyname;
  int espfriendlydistance;
  int espfriendlylevel;
  int espitems;
  int espitemshidefull;
  // Own parent, cut out of Items: figurines are removed from the Items row and drawn here even when AA's cstat 32768
  // hides them pending approach (finding the hidden ones is the point).
  int espfigurines;
  int figurinecounter;
  int espchests;
  // Corner text size, percent (ESP_CORNER_SCALE_MIN..MAX in esp_layout.h), clamped where used; default 150.
  int cornerscale;
  int hivis;
  int espsecrets;
  // Sub-row of espsecrets: the translucent panels refine the secret cross and are pushed only while that walk's row
  // is on.
  int secretvolume;
  // Second sub-row of espsecrets: the hidden-trigger line is drawn inside the same secret pass.
  int secrettriggers;
  // Third sub-row of espsecrets: the route to a secret is drawn inside the same secret pass.
  int secretpath;
  int dmgnumbers;
  int espexplodables;
  int espbreakables;
  // Own parent, not a sub-row of espbreakables: that one is walls from the wall scan, this a sprite walk with a
  // per-actor gamevar read, so neither gates the other.
  int espcash;
  int espswitches;
  // Sub-row of espswitches: the combination lock's tint is collected inside the same walk.
  int esptint;
  int espdrawdist;  // Build units, ESP_DRAWDIST_MIN..ESP_DRAWDIST_MAX (esp.h)
  int espnolimit;  // 0/1, ignores espdrawdist when set

  int aimon;
  int aimhold;  // 0/1, only while the bound key is held
  int aimsmooth;  // 0..100 percent, 0 snaps
  int aimtarget;  // 0 crosshair, 1 closest
  int aimpoint;  // 0 centre, 1 head (AA's headshot band where one exists)
  int aimcone;  // 1..90 degrees
  int aimrange;  // 512..32768 Build units
  int aimlead;
  int aimleadscale;  // 0..100 percent
  int aimdebug;

  int accpistol;
  int accchaingun;
  int accshotgun;
  int accexpander;
  int accincinerator;
  // 0/1. Takes AA's aim kick back the frame it is seen (norecoil.h); with it off nothing is read.
  int norecoil;

  int firerate;
  int switchspeed;
  int reloadrate;

  int coinmagnet;  // radius: Off / 4096 / 8192 / 16384 / Whole map (EK_CM_*, coinmagnet.h; 0 is AA's behaviour)
  int coinspeed;  // 100% / 200% / 400% / 800% / Instant (EK_CS_*)
  int coinheal;
  int pickuprange;  // Off / 1024 / 1536 / 2048 / 4096 / 8192 (EK_PR_*, pickuprange.h; 0 is AA's own 844)

  int silenton;
  int silentmaxangle;  // 1..180 degrees; a shot further off than this is left alone

  int tboton;
  int tbotwhen;  // TBOT_WHEN_CROSSHAIR (0) or TBOT_WHEN_AIMBOT (1), tbot.h
  int tbotdelay;  // 0..500 ms, converted to whole tics by tbot_delay_tics

  int autosavemins;

  // Field of view as a row over the engine's own cvar (osdcmds registers fov; the viewing range is recomputed each
  // frame), not a renderer hook. Off restores whatever the player had.
  int fovon;
  int fov;
  int fovstashheld, fovstashsaved, fovstashforced;

  // Free camera. With freecamon 0 the G_DrawRooms detour's first branch is the pass-through, input.cpp records no
  // toggle edge, and camera_frame returns on two int reads.
  int freecamon;
  int freecamspeed;  // FREECAM_SPEED_MIN..MAX percent (freecam.h), default 100
  // 0/1. Draws every sector after the engine's draw so out of bounds the frame has the whole map and the sky. Costs
  // roughly twice the geometry, so it is its own row.
  int freecamwholemap;
  int freecamcutawaysky;
  // 0/1/2, default 1, the "TROR support" row: the cutaway forwards the engine's g_nodraw domost passes
  // (hole-recording, no paint) to the engine, and the camera arms the base-level draw yax_drawrooms makes afterwards.
  int freecamtrornodraw;
  int freecamsweepbudget;
  int freecamsweeporder;
  int freecamsweeppasses;
  int freecamhideceilings;
  int freecamcutawayparts;
  int freecamcutawayunclamp;
  int freecamcutawaytallwall;
  int freecamcutawaymaskwall;
  int freecamsubst;
  int freecamud;
  int freecamfog;
  int freecamfullbright;
  int freecamshot;
  int freecamshowplayer;
  int freecamdebug;
  int freecamtransport;
  int freecamteleportmarker;
  // Screenshot-mode crash stash: what the ON edge saved, written before the cvars are zeroed and cleared after they
  // are restored, so Alt+F4 or a crash while flying (the path DLL_PROCESS_DETACH cannot reach) restores crosshair and
  // weapon on the next launch. Replayed only if the live cvars agree it is trustworthy (shot_stash_valid).
  int shotstashheld;
  int shotstashmask;  // which of SHOT_CVARS were held; 0 = nothing to replay
  int shotstashcrosshair, shotstashweapon, shotstashstats;

  int debuglog;
  // THROWAWAY spike: writes the held weapon's TotalTime - 1 each frame and reads it back to learn whether AA takes
  // the write back (spiketotaltime.h). No ini row belongs here if the spike says yes.
  int spiketotaltime;
  // 0/1, the shot tracers (tracer.h), independent of debuglog and aimdebug. Off costs one int read per shot and one
  // per frame.
  int tracers;
  // 0/1. The frame-timing table on screen and in the log every five seconds. Off by default; the measurement is
  // always on so the numbers are in the log when something goes wrong (perf.h).
  int perfzones;
  // 0/1. Adds the five per-call zones (cull, updatesector, drawsector, updatewall, drawwall); a sub-row of the above.
  int perfdetail2;
  // 0/1. Draws the on-screen table, separate from perfzones because the table costs ~0.7 ms at 3440x1440, far more
  // than the zones it reports.
  int perftable;
  // Bitmask, ini only, no row: 1 skips the sprite pass, 2 the wall draws, 4 the updates, 8 makes the frustum test
  // reject nothing. Draws the map wrong on purpose to price a phase (perf.h, EK_PERF_SKIP_*).
  int perfskip;
  int secvolnodepth;

  int bindaim, bindcamera, bindteleport;
  int bindcamcopy, bindcampaste;
};

extern Config g_cfg;

extern int g_cfgDirty;

// The bind store bind.h reads and writes through: six rows (panel, aimbot, camera, teleport, copy view, paste view).
// Rows nothing acts on yet still read and write their value honestly, rather than make menu.h diverge.
int  config_bind_get(int row);
void config_bind_set(int row, int vk);

void config_defaults(void);
bool config_load(void);
void config_write_changed(void);

// ekmod.ini's `skipintro` key alone, 0 when absent or unreadable; for DllMain, which runs before config_load.
int config_peek_skipintro(void);
