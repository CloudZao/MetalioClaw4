#include "i18n.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <esp_log.h>

#include "settings.h"

namespace I18n {
namespace {

constexpr const char* TAG = "I18n";
constexpr const char* kSettingsNs = "ui";
constexpr const char* kKeyLocale = "locale";

Locale s_locale = kDefaultLocale;
bool s_lookup_ready = false;
uint16_t s_msgid_order[kStringCount];

int CmpMsgIdIndex(const void* a, const void* b) {
    const auto ia = *static_cast<const uint16_t*>(a);
    const auto ib = *static_cast<const uint16_t*>(b);
    return std::strcmp(kMsgIds[ia], kMsgIds[ib]);
}

void EnsureLookup() {
    if (s_lookup_ready) {
        return;
    }
    for (size_t i = 0; i < kStringCount; ++i) {
        s_msgid_order[i] = static_cast<uint16_t>(i);
    }
    std::qsort(s_msgid_order, kStringCount, sizeof(uint16_t), CmpMsgIdIndex);
    s_lookup_ready = true;
}

int FindMsgIdIndex(const char* msgid) {
    if (msgid == nullptr) {
        return -1;
    }
    EnsureLookup();
    // Manual binary search against kMsgIds[s_msgid_order[i]]
    int lo = 0;
    int hi = static_cast<int>(kStringCount) - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const int idx = s_msgid_order[mid];
        const int cmp = std::strcmp(msgid, kMsgIds[idx]);
        if (cmp == 0) {
            return idx;
        }
        if (cmp < 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return -1;
}

Locale ParseLocaleCode(const char* code) {
    if (code == nullptr || code[0] == '\0') {
        return kDefaultLocale;
    }
    for (size_t i = 0; i < kLocaleCount; ++i) {
        if (std::strcmp(code, kLocaleInfos[i].code) == 0) {
            return kLocaleInfos[i].id;
        }
    }
    return kDefaultLocale;
}

const char* TableAt(Locale locale, size_t index) {
    if (index >= kStringCount) {
        return "";
    }
    const size_t li = static_cast<size_t>(locale);
    if (li >= kLocaleCount) {
        return kMsgIds[index];
    }
    const char* text = kLocaleTables[li][index];
    return text != nullptr ? text : kMsgIds[index];
}

}  // namespace

void Init() {
    Settings s(kSettingsNs, false);
    const std::string code = s.GetString(kKeyLocale, kLocaleInfos[static_cast<size_t>(kDefaultLocale)].code);
    s_locale = ParseLocaleCode(code.c_str());
    EnsureLookup();
    ESP_LOGI(TAG, "locale=%s (%u strings, %u locales)", GetLocaleCode(),
             static_cast<unsigned>(kStringCount),
             static_cast<unsigned>(kLocaleCount));
}

Locale GetLocale() { return s_locale; }

const char* GetLocaleCode() {
    const size_t li = static_cast<size_t>(s_locale);
    if (li >= kLocaleCount) {
        return kLocaleInfos[static_cast<size_t>(kDefaultLocale)].code;
    }
    return kLocaleInfos[li].code;
}

bool SetLocale(Locale locale) {
    const size_t li = static_cast<size_t>(locale);
    if (li >= kLocaleCount) {
        return false;
    }
    if (locale == s_locale) {
        return false;
    }
    s_locale = locale;
    Settings s(kSettingsNs, true);
    s.SetString(kKeyLocale, GetLocaleCode());
    ESP_LOGI(TAG, "locale -> %s", GetLocaleCode());
    return true;
}

bool SetLocaleByCode(const char* code) {
    return SetLocale(ParseLocaleCode(code));
}

const LocaleInfo* GetLocaleInfo(Locale locale) {
    const size_t li = static_cast<size_t>(locale);
    if (li >= kLocaleCount) {
        return nullptr;
    }
    return &kLocaleInfos[li];
}

size_t GetLocaleCount() { return kLocaleCount; }

const char* Tr(Str id) {
    return TableAt(s_locale, static_cast<size_t>(id));
}

const char* T(const char* msgid) {
    if (msgid == nullptr) {
        return "";
    }
    // Fast path: default locale returns msgid (catalog source language).
    if (s_locale == kDefaultLocale) {
        return msgid;
    }
    const int idx = FindMsgIdIndex(msgid);
    if (idx < 0) {
        return msgid;
    }
    return TableAt(s_locale, static_cast<size_t>(idx));
}

int Tf(char* buf, size_t buf_size, Str id, ...) {
    if (buf == nullptr || buf_size == 0) {
        return 0;
    }
    va_list ap;
    va_start(ap, id);
    const int n = std::vsnprintf(buf, buf_size, Tr(id), ap);
    va_end(ap);
    return n;
}

}  // namespace I18n
