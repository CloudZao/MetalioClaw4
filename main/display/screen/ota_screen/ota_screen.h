#pragma once

#include <cstddef>

class OtaScreen {
public:
    static void Show(const char* version_text);
    static void Update(int progress, size_t downloaded, size_t total, size_t speed_bps);
    static void SetStatusMessage(const char* message);
    static void Dismiss();
    static bool IsActive();
};
