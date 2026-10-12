#pragma once

// Draw the ESP; returns markers that reached the canvas. Render thread, inside SwapBuffers. notDrawn counts gathered
// markers not drawn, refused the part the collection ceiling turned away (a silent drop is a hazard); wallOverflow is
// the wall cache's overflow. Zeroed on every path.
int esp_draw(int* notDrawn, int* refused, int* wallOverflow);

int esp_combination_seen(void);
int esp_combination_pushed(void);
int esp_combination_boxed(void);
int esp_combination_flat(void);
int esp_combination_quads(void);

int esp_secretvol_staged(void);
int esp_secretvol_drawn(void);
int esp_secretvol_skipped(void);
int esp_secretvol_unreadable(void);
int esp_secretvol_inexact(void);
int esp_secretvol_quads(void);

// refused (chain cap) and dropped (scratch capacity) are separate limits (secrettrig.h).
int esp_secrettrig_seen(void);
int esp_secrettrig_staged(void);
int esp_secrettrig_drawn(void);
int esp_secrettrig_refused(void);
int esp_secrettrig_dropped(void);

int esp_secretpath_points(void);
int esp_secretpath_segments(void);
int esp_secretpath_crosses(void);

// Damage numbers cull on the same quantity; two distance rules can drift.
int esp_dist2d_public(int cx, int cy, int x, int y);

// 512 matches the other ports.
const int BUILD_UNITS_PER_METRE = 512;

// World Tour's bounds. Default is the MAXIMUM: a cull active by default reads as a broken projection.
const int ESP_DRAWDIST_MIN     = 512;
const int ESP_DRAWDIST_MAX     = 131072;
const int ESP_DRAWDIST_DEFAULT = ESP_DRAWDIST_MAX;

// Called on level change: sprite indices are reused.
void esp_forget_health(void);

int aim_draw_marker(void);

// Gated on the LIVE camera (esp_layout.h freecam_marker_on), behind overlay.cpp's screenshot gate.
int camera_draw_player_marker(void);

// True if the level has a SECRET exit, answered as the switch walks classify, even if unfound.
bool esp_level_has_secret_exit(void);

// Teleport landing marker and refusal notice. The column takes the live plan, the notice the latched outcome
// (camera.h). Called unconditionally so the counters zero when the screenshot gate is closed.
void esp_draw_teleport(int active, int screenW, int screenH);

int esp_teleport_quads(void);
int esp_teleport_notice(void);
