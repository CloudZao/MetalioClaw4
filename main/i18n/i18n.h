#pragma once

// ---------------------------------------------------------------------------
// Runtime i18n (UI strings). Extensible: add locales in catalog.json, regenerate.
//
// Preferred (typed):
//   lv_label_set_text(lbl, I18n::Tr(I18n::Str::SETTINGS));
//
// Gettext-style (msgid = Chinese source text):
//   lv_label_set_text(lbl, I18n::T("设置"));
//
// Format helpers:
//   char buf[64];
//   I18n::Tf(buf, sizeof(buf), I18n::Str::BATTERY_PCT, level);
//
// Locale persistence: NVS namespace "ui", key "locale" (e.g. "zh-CN" / "en-US").
// After SetLocale(), rebuild UI (HomeScreen::Create()) like theme switch.
// ---------------------------------------------------------------------------

#include "i18n_strings_gen.h"

#include <cstdarg>
#include <cstddef>

namespace I18n {

// Load locale from NVS (call once at boot, before first UI create).
void Init();

Locale GetLocale();
const char* GetLocaleCode();  // "zh-CN" / "en-US" / ...

// Persist and switch. Returns true if locale changed.
bool SetLocale(Locale locale);
bool SetLocaleByCode(const char* code);  // "zh-CN", "en-US", ...

const LocaleInfo* GetLocaleInfo(Locale locale);
size_t GetLocaleCount();

// Typed lookup (O(1)).
const char* Tr(Str id);

// Gettext-style: msgid is the default-locale (zh-CN) source string.
// Unknown msgid returns msgid itself (safe for gradual migration).
const char* T(const char* msgid);

// snprintf into buf using Tr(id) as format. Returns bytes written (excl. NUL).
int Tf(char* buf, size_t buf_size, Str id, ...);

}  // namespace I18n
