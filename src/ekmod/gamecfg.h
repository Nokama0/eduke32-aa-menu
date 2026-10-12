// EDuke32's own key bindings, so a bind row can warn when its key is already the game's. Pure half. EDuke32 writes bind
// KEY FUNC one per line in settings.cfg, so World Tour's key-name table is not needed.
#pragma once
#include <string.h>

// The game's spelling for a virtual key, or null. EDuke32's abbreviations, read from a real settings.cfg. A missed
// warning costs nothing, a wrong one sends the player renaming a fine binding.
inline const char* ekm_gamecfg_key_name(int vk) {
  static const char kLetters[26][2] = {
    "A","B","C","D","E","F","G","H","I","J","K","L","M",
    "N","O","P","Q","R","S","T","U","V","W","X","Y","Z" };
  static const char kDigits[10][2] = {
    "0","1","2","3","4","5","6","7","8","9" };
  if (vk >= 0x41 && vk <= 0x5A) return kLetters[vk - 0x41];
  if (vk >= 0x30 && vk <= 0x39) return kDigits[vk - 0x30];
  if (vk >= 0x70 && vk <= 0x7B) {
    static const char* const kF[12] = { "F1","F2","F3","F4","F5","F6",
                                        "F7","F8","F9","F10","F11","F12" };
    return kF[vk - 0x70];
  }
  if (vk >= 0x60 && vk <= 0x69) {
    static const char* const kKp[10] = { "Kpad0","Kpad1","Kpad2","Kpad3","Kpad4",
                                         "Kpad5","Kpad6","Kpad7","Kpad8","Kpad9" };
    return kKp[vk - 0x60];
  }
  switch (vk) {
    case 0x08: return "BakSpc";   case 0x09: return "Tab";
    case 0x0D: return "Enter";    case 0x14: return "CapLck";
    case 0x1B: return "Escape";   case 0x20: return "Space";
    case 0x21: return "PgUp";     case 0x22: return "PgDn";
    case 0x23: return "End";      case 0x24: return "Home";
    case 0x25: return "Left";     case 0x26: return "Up";
    case 0x27: return "Right";    case 0x28: return "Down";
    case 0x2D: return "Insert";   case 0x2E: return "Delete";
    case 0x6A: return "Kpad*";    case 0x6B: return "Kpad+";
    case 0x6D: return "Kpad-";    case 0x6E: return "Kpad.";
    case 0x6F: return "Kpad/";    case 0x91: return "ScrLck";
    case 0xA0: return "LShift";   case 0xA1: return "RShift";
    case 0xA2: return "LCtrl";    case 0xA3: return "RCtrl";
    case 0xA4: return "LAlt";     case 0xA5: return "RAlt";
    case 0xBD: return "-";        case 0xBB: return "=";
    case 0xDB: return "[";        case 0xDD: return "]";
    case 0xBA: return ";";        case 0xDE: return "'";
    case 0xBF: return "/";        case 0xBC: return ",";
    case 0xBE: return ".";        case 0xDC: return "\\";
    default:   return 0;
  }
}

// KpdEnt is deliberately absent: keypad Enter arrives as VK_RETURN with an extended flag bind.h does not carry, so it
// cannot be told from Enter. The warning may be missed, never wrongly raised.

// Case-insensitive, load-bearing: the file is inconsistent about case and a sensitive compare would silently disable
// the warning for whole rows.
inline int ekm_gamecfg_same_key(const char* a, const char* b) {
  if (!a || !b) return 0;
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
    if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
    if (ca != cb) return 0;
    a++; b++;
  }
  return (*a == 0 && *b == 0) ? 1 : 0;
}

// 1 only for a complete bind "KEY" "FUNC"; any truncation answers 0, since a partial parse would claim a binding the
// game lacks.
inline int ekm_gamecfg_parse_line(const char* line, char* keyOut, int keyCap,
                                  char* fnOut, int fnCap) {
  if (!line || keyCap <= 0 || fnCap <= 0) return 0;
  const char* p = line;
  while (*p == ' ' || *p == '\t') p++;
  static const char kBind[] = "bind";
  int i = 0;
  for (; kBind[i]; i++) if (p[i] != kBind[i]) return 0;
  p += i;
  if (*p != ' ' && *p != '\t') return 0;
  while (*p == ' ' || *p == '\t') p++;

  if (*p != '"') return 0;
  p++;
  const char* keyStart = p;
  while (*p && *p != '"') p++;
  if (*p != '"') return 0;
  int keyLen = (int)(p - keyStart);
  if (keyLen <= 0 || keyLen >= keyCap) return 0;
  p++;

  while (*p == ' ' || *p == '\t') p++;
  if (*p != '"') return 0;
  p++;
  const char* fnStart = p;
  while (*p && *p != '"') p++;
  if (*p != '"') return 0;
  int fnLen = (int)(p - fnStart);
  if (fnLen <= 0 || fnLen >= fnCap) return 0;

  memcpy(keyOut, keyStart, (size_t)keyLen);
  keyOut[keyLen] = 0;
  memcpy(fnOut, fnStart, (size_t)fnLen);
  fnOut[fnLen] = 0;
  return 1;
}

// Part of an fread safe to parse. got == cap may have cut the file mid-line, so keep up to the last line break (nothing
// if there is none). `cap` is sizeof(buf) - 1.
inline size_t ekm_cfg_complete_len(const char* buf, size_t got, size_t cap) {
  if (got < cap) return got;
  for (size_t i = got; i > 0; i--) {
    if (buf[i - 1] == '\n' || buf[i - 1] == '\r') return i;
  }
  return 0;
}

#ifndef EK_GAMECFG_PURE_ONLY
void gamecfg_load(void);
const char* gamecfg_function_for(int vk);
#endif
