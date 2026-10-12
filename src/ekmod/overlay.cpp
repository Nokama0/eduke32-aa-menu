// The panel: our own ImGui, drawn inside GDI32!SwapBuffers on the render thread with the game's GL context current and
// the frame drawn. EDuke32's own ImGui is statically linked and unexported. Input lives in input.cpp on the window
// thread; this file reads the panel's open state and cursor as interlocked scalars.

#include <windows.h>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_opengl3.h"
#include "imgui/imgui_impl_win32.h"
#include "log.h"
#include "overlay.h"
#include "cvars.h"
#include "console.h"
#include "input.h"
#include "gamestate.h"
#include "esp.h"
#include "ui/menu.h"
#include "config.h"
#include "ui/input.h"
#include "ui/canvas.h"
#include "project.h"
#include "cansee.h"
#include "hitscan.h"
#include "aim.h"
#include "tbot.h"
#include "tracer.h"
#include "dmgnum.h"
#include "gamehooks.h"
#include "localinput.h"
#include "weaponspeed.h"
#include "coinmagnet.h"
#include "pickuprange.h"
#include "norecoil.h"
#include "spiketotaltime.h"
#include "autosave.h"
#include "playerstate.h"
#include "camera.h"
#include "shot.h"
#include "fov.h"
#include "maskscan.h"
#include "nocull.h"
#include "cutaway.h"
#include "perf.h"

static bool s_ready = false;
static bool s_failed = false;
static HWND s_wnd = NULL;

typedef const unsigned char*(WINAPI* GetStringFn)(unsigned int);
static void log_gl_identity(void) {
  const HMODULE gl = GetModuleHandleA("opengl32.dll");
  if (!gl) { ek_log("overlay: opengl32.dll not in the process"); return; }
  const GetStringFn get = (GetStringFn)GetProcAddress(gl, "glGetString");
  if (!get) { ek_log("overlay: no glGetString"); return; }
  const unsigned char* ver = get(0x1F02);
  const unsigned char* ren = get(0x1F01);
  ek_log("overlay: GL_VERSION=%s", ver ? (const char*)ver : "(null)");
  ek_log("overlay: GL_RENDERER=%s", ren ? (const char*)ren : "(null)");
}

static bool overlay_init(HDC hdc) {
  s_wnd = WindowFromDC(hdc);
  if (!s_wnd) { ek_log("overlay: WindowFromDC gave no window; cannot init"); return false; }

  log_gl_identity();

  IMGUI_CHECKVERSION();
  if (!ImGui::CreateContext()) { ek_log("overlay: CreateContext failed"); return false; }

  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = NULL;
  io.LogFilename = NULL;

  // The backend, not SDL, was showing the OS cursor: ImGui_ImplWin32_UpdateMouseCursor calls SetCursor whenever
  // io.MouseDrawCursor is false (imgui_impl_win32.cpp:274), the state the instant the panel closes. NoMouseCursorChange
  // stops that; our software cursor is unaffected.
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

  if (!ImGui_ImplWin32_Init(s_wnd)) { ek_log("overlay: ImplWin32_Init failed"); return false; }
  // NULL lets the backend pick a GLSL version from the live context (Polymer and Polymost set it up differently).
  if (!ImGui_ImplOpenGL3_Init(NULL)) { ek_log("overlay: ImplOpenGL3_Init failed"); return false; }

  ek_log("overlay: ImGui %s initialised on hwnd=%p", IMGUI_VERSION, (void*)s_wnd);

  config_load();

  input_install(s_wnd);

  // Scanned here, not at load: at DllMain the engine has not yet parsed eduke32.cfg into the cvars, so values would be
  // defaults.
  cvars_scan();

  console_locate();

  // The crash stash replay goes here and nowhere else: the earliest point cvar_get_int answers, and strictly before
  // camera_frame can reach another ON edge that zeroes these cvars again (shot.h's shot_stash_replay).
  shot_replay_stash();

  fov_replay_stash();

  project_check_assumptions();

  // Cross-check: reading 204 records proves the scan found something shaped like cvars, not that the pointers are
  // right. These names have known values in settings.cfg; a mismatch means the strategy failed and must be loud.
  static const char* const kCheck[] = {
    "r_borderless", "r_hightile", "r_texfilter", "r_anisotropy", "r_downsize",
    "r_models", "r_texcompr", "r_useindexedcolortextures", "r_pr_lighting",
    "r_pr_shadowcount", "r_pr_shadowdetail", "r_maxfps", "fov", "cl_autoaim",
    "r_maxspritesonscreen", "cl_weaponswitch",
  };
  for (int i = 0; i < (int)(sizeof(kCheck) / sizeof(kCheck[0])); i++) {
    int v = 0;
    const EkCvar* c = cvars_find(kCheck[i]);
    if (!c) { ek_log("cvars:   %-26s NOT FOUND", kCheck[i]); continue; }
    if (!cvar_get_int(kCheck[i], &v)) {
      ek_log("cvars:   %-26s found (flags=0x%X) but not int-readable", kCheck[i], c->flags);
      continue;
    }
    ek_log("cvars:   %-26s = %-6d [%d,%d] at %p", kCheck[i], v, c->vmin, c->vmax, c->ptr);
  }
  return true;
}

void overlay_on_swap(HDC hdc) {
  if (s_failed) return;
  if (!s_ready) {
    if (!overlay_init(hdc)) {
      // One attempt: retrying every frame would fill the log and keep poking a bad context.
      s_failed = true;
      ek_log("overlay: initialisation failed; overlay disabled for this session");
      return;
    }
    s_ready = true;
  }

  // The hunt starts here, before any early-out: it once started inside the panel block, so a launch without opening the
  // panel never began (the ESP waited on a scan that waited on the panel).
  gamestate_scan();

  // cansee is scanned here only: its self-test CALLS the engine's cansee, which writes clipsectorlist[] and can
  // reallocate reachablesectors (cansee.h); inside SwapBuffers the tic is finished, the only moment those globals are
  // ours.
  cansee_scan();

  // hitscan, same rule: its self-test writes engine scratch (clipsectorlist, clipsectormap, both yax bitmaps), so it
  // may only run here (hitscan.h). A refusal costs the features, not the game.
  hitscan_scan();

  // The origin sector the engine recomputes before every shot cast: after hitscan, whose rva is one of the two
  // witnesses that locate it; updatesector writes the engine's neighbour list and bitmap (hitscan.h).
  updatesector_scan();

  // G_DrawRooms, the draw call the free camera brackets: this locate only reads the image and needs no player or idle
  // engine. A refusal costs the free camera only.
  camera_scan();

  // Both firing hooks go in from here only: patching live code is safe without suspending anything because the writing
  // thread runs the patched functions, and the single-threaded loop is inside our SwapBuffers hook, not inside
  // P_PreFireHitscan or A_ShootWithZvel. Never from the window thread or the scan worker.
  gamehooks_install();

  // The free camera's detour, same argument (not inside G_DrawRooms). Unconditional so the Settings switch knows
  // whether it can act; the detour's first branch is one flag read.
  camera_install();

  shot_install();

  maskscan_install();

  nocull_install();

  // The cutaway's hook on polymost_domost, same argument; on the hottest seam in the engine, so four scalar reads and a
  // forwarded call until the free camera flies with the row on.
  cutaway_install();

  camera_frame();

  fov_frame();

  // The localInput scan, before the early-out: the Triggerbot row's Unavailable label and aim_frame's cost gate need it
  // before anything writes.
  localinput_locate();

  weaponspeed_frame();

  norecoil_frame();

  coinmagnet_frame();

  // Spike-totaltime: throwaway diagnostic on the CON VM's thread, gated on g_cfg.spiketotaltime, no menu row.
  spiketotaltime_frame();

  level_epoch_tick();

  // Pickup range: the VM's own thread with the VM suspended, the only place AA's script operands may be rewritten.
  pickuprange_frame();

  autosave_frame();

  // The shot tracers' casts, ahead of the early-out and of tbot_frame: a round fired on the tic that just finished must
  // be in the ring before tbot_frame's impact scan or every one pairs a frame late. ek_hitscan may be called from here
  // (hitscan.h); the detour that queued the round may not.
  tracer_frame();

  // The ESP draws whether or not the panel is open. Screenshot mode turns all of our overlay off through this one gate
  // (a false returns above ImGui::NewFrame). It does NOT stand the bots down: each carries its own camera term
  // (freecam_bots_run in aim.cpp and tbot.cpp).
  const bool wantEsp = gamestate_ready() && !shot_hides_overlays(shot_active_now());
  if (!input_panel_open() && !wantEsp) return;

  perf_zone_begin(EK_PZ_OVERLAY_BUILD);
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplWin32_NewFrame();

  // Our cursor, pushed after the backend's: ImGui_ImplWin32_NewFrame queues GetCursorPos, the warp point, constant
  // while SDL holds the mouse in relative mode; ours is queued after it and so lands.
  {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = input_panel_open();
    float mx = 0.0f, my = 0.0f;
    input_mouse_pos(&mx, &my);
    io.AddMousePosEvent(mx, my);
  }

  ImGui::NewFrame();

  extern void canvas_set_drawlist(ImDrawList* dl);
  extern void uibridge_begin_frame(void);

  const ImGuiIO& cio = ImGui::GetIO();
  canvas_set_drawlist(ImGui::GetBackgroundDrawList());
  canvas_begin((int)cio.DisplaySize.x, (int)cio.DisplaySize.y);
  uibridge_begin_frame();

  aim_frame();

  // Immediately after, and that order is the contract: the triggerbot's condition is the lock aim_frame publishes and
  // clears at the top of every call, so ahead of it the read is one frame stale.
  tbot_frame();

  int espNotDrawn = 0, espRefused = 0, espWallOverflow = 0;
  perf_zone_begin(EK_PZ_OVERLAY_ESP);
  const int espCount = wantEsp ? esp_draw(&espNotDrawn, &espRefused, &espWallOverflow) : 0;
  // Behind wantEsp, the screenshot gate, not the ESP row: the calibration marker and the tracers keep their own
  // switches, but with the panel OPEN the early-out does not fire, so they would draw into screenshot mode's clean
  // frame.
  if (wantEsp) {
    aim_draw_marker();
    tracer_draw();
  }
  perf_zone_end(EK_PZ_OVERLAY_ESP, espCount);

  // Inside overlay.cpp's own canvas frame; dmgnum_draw must not open one. Unconditional: it zeroes its counter when the
  // gate is closed, so the diagnostics row reads 0 instead of latching.
  dmgnum_draw(wantEsp);

  // The free camera's "You" marker, last so nothing sits on top. Behind wantEsp, not g_cfg.esp: it is our overlay
  // (shots hide it) but not an ESP sub-row, since a player flying with the ESP off still needs to find it.
  if (wantEsp) camera_draw_player_marker();
  // The teleport column and notice, inside this canvas frame; unconditional like dmgnum_draw: it zeroes its counters
  // when the gate is closed.
  esp_draw_teleport(wantEsp ? 1 : 0, (int)cio.DisplaySize.x, (int)cio.DisplaySize.y);
  if (input_panel_open())
    menu_draw((int)cio.DisplaySize.x, (int)cio.DisplaySize.y);

  perf_zone_end(EK_PZ_OVERLAY_BUILD, 1);

  perf_zone_begin(EK_PZ_OVERLAY_PERF);
  perf_draw();
  perf_zone_end(EK_PZ_OVERLAY_PERF, 1);

  canvas_end();
  canvas_set_drawlist(NULL);

  if (!input_panel_open()) {
    perf_zone_begin(EK_PZ_OVERLAY_PANEL);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    perf_zone_end(EK_PZ_OVERLAY_PANEL, 1);
    return;
  }


  perf_zone_begin(EK_PZ_OVERLAY_PANEL);
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  perf_zone_end(EK_PZ_OVERLAY_PANEL, 1);
}
