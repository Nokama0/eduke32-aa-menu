#pragma once

// Fail-closed: false on any failure. Window thread only.

bool clipboard_read_text(char* out, int cap);

bool clipboard_write_text(const char* s);
