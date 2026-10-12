#pragma once
#include <windows.h>

// Returns the original, or NULL.
void* iat_hook(HMODULE mod, const char* dll, const char* fn, void* replacement);

bool iat_install_swap_hook(void);

long iat_swap_frames(void);
