// Input fields the carried panel reads, bridged from ImGui's edge-detected event queue (no sequence-number
// machinery). The cursor is input.cpp's virtual one: SDL warps the real pointer to centre every frame.

#include <windows.h>
#include <float.h>
#include "imgui/imgui.h"
#include "input.h"
#include "config.h"
#include "log.h"
#include "ui/ui.h"
#include "ui/input.h"
#include "ui/common.h"
#include "ui/wallclock.h"

UiInput g_ui;

static void uibridge_log_cursor(const ImGuiIO& io) {
  if (!g_cfg.debuglog) return;
  static unsigned lastMs = 0;
  static bool logged = false;
  const unsigned now = (unsigned)GetTickCount();
  if (logged && now - lastMs < 1000) return;
  logged = true; lastMs = now;

  int rawSeen = 0, rawLive = 0, rawN = 0, moveN = 0;
  input_debug_take(&rawSeen, &rawLive, &rawN, &moveN);

  if (io.MousePos.x <= -FLT_MAX * 0.5f)
    ek_log("cursor: rawseen=%d rawlive=%d virt=%d,%d io=unset raw=%d move=%d",
           rawSeen, rawLive, g_ui.mx, g_ui.my, rawN, moveN);
  else
    ek_log("cursor: rawseen=%d rawlive=%d virt=%d,%d io=%d,%d raw=%d move=%d",
           rawSeen, rawLive, g_ui.mx, g_ui.my,
           (int)io.MousePos.x, (int)io.MousePos.y, rawN, moveN);
}

void uibridge_begin_frame(void) {
  float mx = 0.0f, my = 0.0f;
  input_mouse_pos(&mx, &my);
  g_ui.mx = (int)mx;
  g_ui.my = (int)my;

  // Closed means no input at all: a carried-over held button would drag a row the player cannot see.
  if (!input_panel_open()) {
    g_ui.down = g_ui.pressed = g_ui.rdown = g_ui.rpressed = false;
    return;
  }

  const ImGuiIO& io = ImGui::GetIO();
  g_ui.down     = io.MouseDown[0];
  g_ui.rdown    = io.MouseDown[1];
  g_ui.pressed  = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  g_ui.rpressed = ImGui::IsMouseClicked(ImGuiMouseButton_Right);

  uibridge_log_cursor(io);
}

unsigned wallclock_ms(void) { return (unsigned)GetTickCount(); }
