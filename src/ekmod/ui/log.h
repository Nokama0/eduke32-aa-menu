#pragma once
// Shim: LOGE folds onto LOGI.
#include "../log.h"

#define LOGI(...) ek_log(__VA_ARGS__)
#define LOGE(...) ek_log(__VA_ARGS__)
