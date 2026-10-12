#pragma once

// Render thread only: patches live code.
bool gamehooks_install(void);

// Unpatches on DLL_PROCESS_DETACH.
void gamehooks_remove(void);

bool gamehooks_shoot_ready(void);
bool gamehooks_spread_ready(void);

unsigned gamehooks_bullets_fired(void);

unsigned gamehooks_shoot_rva(void);
unsigned gamehooks_spread_rva(void);

// 0 unless exactly one function matched.
unsigned gamehooks_incurdamage_rva(void);
