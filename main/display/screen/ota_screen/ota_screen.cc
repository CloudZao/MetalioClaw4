#include "ota_screen.h"

#include <cstdio>

#include <esp_log.h>
#include <esp_timer.h>

#include "assets/lang_config.h"
#include "esp_lv_adapter.h"
#include "home_screen/home_screen.h"
#include "screen_util.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "OtaScreen";
constexpr int kPanelSize = 720;
constexpr int kBarWidth = 560;
constexpr int kBarHeight = 16;

struct OtaUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* bar = nullptr;
    lv_obj_t* percent_lbl = nullptr;
    lv_obj_t* bytes_lbl = nullptr;
    lv_obj_t* speed_lbl = nullptr;
    lv_obj_t* time_lbl = nullptr;
    lv_obj_t* status_lbl = nullptr;
    lv_obj_t* version_lbl = nullptr;
    bool active = false;
};

OtaUi s_ui;
int64_t s_start_time_us = 0;

void FormatDuration(char* buf, size_t buf_size, int seconds) {
    if (seconds < 0) {
        seconds = 0;
    }
    const int hours = seconds / 3600;
    const int minutes = (seconds % 3600) / 60;
    const int secs = seconds % 60;
    if (hours > 0) {
        snprintf(buf, buf_size, "%d:%02d:%02d", hours, minutes, secs);
    } else {
        snprintf(buf, buf_size, "%02d:%02d", minutes, secs);
    }
}

void FormatDataSize(char* buf, size_t buf_size, size_t bytes) {
    if (bytes >= 1024 * 1024) {
        snprintf(buf, buf_size, "%.2f MB", bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(buf, buf_size, "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(buf, buf_size, "%u B", static_cast<unsigned>(bytes));
    }
}

void FormatSpeed(char* buf, size_t buf_size, size_t speed_bps) {
    if (speed_bps >= 1024 * 1024) {
        snprintf(buf, buf_size, "%.2f MB/s", speed_bps / (1024.0 * 1024.0));
    } else if (speed_bps >= 1024) {
        snprintf(buf, buf_size, "%.1f KB/s", speed_bps / 1024.0);
    } else {
        snprintf(buf, buf_size, "%u B/s", static_cast<unsigned>(speed_bps));
    }
}

void OnScreenDeleted(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target_obj(e) == s_ui.screen) {
        s_ui = {};
        s_start_time_us = 0;
    }
}

lv_obj_t* CreateTouchBlocker(lv_obj_t* parent) {
    lv_obj_t* blocker = lv_obj_create(parent);
    screen_strip_obj_chrome(blocker);
    lv_obj_set_size(blocker, kPanelSize, kPanelSize);
    lv_obj_align(blocker, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(blocker, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(blocker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(blocker, LV_OBJ_FLAG_SCROLLABLE);
    return blocker;
}

lv_obj_t* BuildScreen(const char* version_text) {
    lv_obj_t* screen = lv_obj_create(nullptr);
    screen_strip_obj_chrome(screen);
    lv_obj_set_size(screen, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0A0D12), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(screen);
    lv_label_set_text(title, Lang::Strings::OTA_UPGRADE);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t* version_lbl = lv_label_create(screen);
    s_ui.version_lbl = version_lbl;
    if (version_text != nullptr && version_text[0] != '\0') {
        lv_label_set_text(version_lbl, version_text);
    } else {
        lv_label_set_text(version_lbl, "");
    }
    lv_obj_set_style_text_color(version_lbl, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(version_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(version_lbl, LV_ALIGN_TOP_MID, 0, 170);

    lv_obj_t* status_lbl = lv_label_create(screen);
    s_ui.status_lbl = status_lbl;
    lv_label_set_text(status_lbl, Lang::Strings::UPGRADING);
    lv_obj_set_style_text_color(status_lbl, lv_color_hex(0xC7CDD9), LV_PART_MAIN);
    lv_obj_set_style_text_font(status_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(status_lbl, LV_ALIGN_TOP_MID, 0, 220);

    lv_obj_t* bar = lv_bar_create(screen);
    s_ui.bar = bar;
    lv_obj_set_size(bar, kBarWidth, kBarHeight);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, -10);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1B2230), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, kBarHeight / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x3B82F6), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, kBarHeight / 2, LV_PART_INDICATOR);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* percent_lbl = lv_label_create(screen);
    s_ui.percent_lbl = percent_lbl;
    lv_label_set_text(percent_lbl, "0%");
    lv_obj_set_style_text_color(percent_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(percent_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(percent_lbl, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t* bytes_lbl = lv_label_create(screen);
    s_ui.bytes_lbl = bytes_lbl;
    lv_label_set_text(bytes_lbl, "0 B / 0 B");
    lv_obj_set_style_text_color(bytes_lbl, lv_color_hex(0xC7CDD9), LV_PART_MAIN);
    lv_obj_set_style_text_font(bytes_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(bytes_lbl, LV_ALIGN_CENTER, 0, 90);

    lv_obj_t* speed_lbl = lv_label_create(screen);
    s_ui.speed_lbl = speed_lbl;
    lv_label_set_text(speed_lbl, "0 B/s");
    lv_obj_set_style_text_color(speed_lbl, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(speed_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(speed_lbl, LV_ALIGN_CENTER, 0, 130);

    lv_obj_t* time_lbl = lv_label_create(screen);
    s_ui.time_lbl = time_lbl;
    lv_label_set_text(time_lbl, "用时 00:00");
    lv_obj_set_style_text_color(time_lbl, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(time_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(time_lbl, LV_ALIGN_CENTER, 0, 170);

    lv_obj_t* hint = lv_label_create(screen);
    lv_label_set_text(hint, Lang::Strings::PLEASE_WAIT);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x6B7280), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -80);

    CreateTouchBlocker(screen);
    lv_obj_add_event_cb(screen, OnScreenDeleted, LV_EVENT_DELETE, nullptr);
    return screen;
}

void UpdateWidgets(int progress, size_t downloaded, size_t total, size_t speed_bps) {
    if (!s_ui.active || s_ui.bar == nullptr) {
        return;
    }

    if (progress < 0) {
        progress = 0;
    } else if (progress > 100) {
        progress = 100;
    }

    lv_bar_set_value(s_ui.bar, progress, LV_ANIM_OFF);

    char percent_buf[16];
    snprintf(percent_buf, sizeof(percent_buf), "%d%%", progress);
    lv_label_set_text(s_ui.percent_lbl, percent_buf);

    char downloaded_buf[24];
    char total_buf[24];
    FormatDataSize(downloaded_buf, sizeof(downloaded_buf), downloaded);
    FormatDataSize(total_buf, sizeof(total_buf), total);
    char bytes_buf[64];
    snprintf(bytes_buf, sizeof(bytes_buf), "%s / %s", downloaded_buf, total_buf);
    lv_label_set_text(s_ui.bytes_lbl, bytes_buf);

    char speed_buf[32];
    FormatSpeed(speed_buf, sizeof(speed_buf), speed_bps);
    lv_label_set_text(s_ui.speed_lbl, speed_buf);

    const int64_t now_us = esp_timer_get_time();
    int elapsed_sec = 0;
    if (s_start_time_us > 0 && now_us > s_start_time_us) {
        elapsed_sec = static_cast<int>((now_us - s_start_time_us) / 1000000);
    }

    char elapsed_buf[16];
    FormatDuration(elapsed_buf, sizeof(elapsed_buf), elapsed_sec);
    char time_buf[24];
    snprintf(time_buf, sizeof(time_buf), "用时 %s", elapsed_buf);
    lv_label_set_text(s_ui.time_lbl, time_buf);
}

struct OtaProgressMsg {
    int progress;
    size_t downloaded;
    size_t total;
    size_t speed_bps;
};

void AsyncUpdateProgress(void* user_data) {
    auto* msg = static_cast<OtaProgressMsg*>(user_data);
    if (msg == nullptr) {
        return;
    }
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        UpdateWidgets(msg->progress, msg->downloaded, msg->total, msg->speed_bps);
        esp_lv_adapter_refresh_now(nullptr);
        esp_lv_adapter_unlock();
    }
    delete msg;
}

}  // namespace

void OtaScreen::Show(const char* version_text) {
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }

    if (s_ui.active && s_ui.screen != nullptr) {
        lv_obj_del(s_ui.screen);
        s_ui = {};
    }

    s_ui.screen = BuildScreen(version_text);
    s_ui.active = true;
    s_start_time_us = esp_timer_get_time();
    lv_screen_load(s_ui.screen);
    UpdateWidgets(0, 0, 0, 0);
    esp_lv_adapter_refresh_now(nullptr);
    esp_lv_adapter_unlock();
    ESP_LOGI(TAG, "OTA screen shown");
}

void OtaScreen::Update(int progress, size_t downloaded, size_t total, size_t speed_bps) {
    if (!s_ui.active) {
        return;
    }
    auto* msg = new OtaProgressMsg{progress, downloaded, total, speed_bps};
    lv_async_call(AsyncUpdateProgress, msg);
}

void OtaScreen::SetStatusMessage(const char* message) {
    if (!s_ui.active || message == nullptr) {
        return;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (s_ui.status_lbl != nullptr) {
        lv_label_set_text(s_ui.status_lbl, message);
    }
    if (s_ui.bar != nullptr) {
        lv_bar_set_value(s_ui.bar, 100, LV_ANIM_OFF);
        lv_label_set_text(s_ui.percent_lbl, "100%");
    }
    if (s_ui.time_lbl != nullptr && s_start_time_us > 0) {
        const int elapsed_sec = static_cast<int>((esp_timer_get_time() - s_start_time_us) / 1000000);
        char elapsed_buf[16];
        FormatDuration(elapsed_buf, sizeof(elapsed_buf), elapsed_sec);
        char time_buf[32];
        snprintf(time_buf, sizeof(time_buf), "用时 %s", elapsed_buf);
        lv_label_set_text(s_ui.time_lbl, time_buf);
    }
    esp_lv_adapter_refresh_now(nullptr);
    esp_lv_adapter_unlock();
}

void OtaScreen::Dismiss() {
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }

    lv_obj_t* old_screen = s_ui.screen;
    s_ui = {};
    s_start_time_us = 0;

    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_screen != nullptr) {
        lv_obj_delete(old_screen);
    }

    esp_lv_adapter_unlock();
    ESP_LOGI(TAG, "OTA screen dismissed");
}

bool OtaScreen::IsActive() {
    return s_ui.active;
}
