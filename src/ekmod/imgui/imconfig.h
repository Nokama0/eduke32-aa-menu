#pragma once
// Stock Dear ImGui configuration.
//
// NOT the imconfig.h that ships in EDuke32's tree. Theirs defines
// IM_VEC2_CLASS_EXTRA / IM_VEC4_CLASS_EXTRA to convert to and from Build's own
// vec2_t and vec4_t, which are engine types this DLL does not have and has no
// reason to grow. The rest of their copy of ImGui 1.92.6 is stock, so this file
// is the only divergence and it exists to remove one, not add one.
//
// Every #define here changes ImGui's ABI, and this DLL renders its own instance into
// someone else's GL context: add one only deliberately, as the two below were.

// The menu uses no gamepad and no shell links. Without these the Win32 backend loads
// xinput1_4.dll by bare name (a copy in the game folder would run) and links ShellExecuteW. (2026-10-12)
#define IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
