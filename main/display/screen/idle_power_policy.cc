#include "idle_power_policy.h"
#include "i18n.h"

#include <cstdio>

#include <esp_log.h>
#include <lvgl.h>

#include "home_screen/home_screen.h"
#include "settings.h"
#include "standby_screen/standby_screen.h"

namespace {

constexpr const char* TAG = "IdlePower";

constexpr int kDefaultShutdownMin = 5;
constexpr int kDefaultStandbyMin = 1;
constexpr int kMaxMinutes = 60;
constexpr const char* kShutdownNvsKey = "idle_off_min";
constexpr const char* kStandbyNvsKey = "idle_stby_min";
constexpr uint32_t kTickPeriodMs = 1000;
constexpr uint32_t kStatusLogIntervalMs = 60 * 1000;

struct State {
    lv_timer_t* timer = nullptr;
    IdlePowerSession session = IdlePowerSession::None;
    uint32_t last_activity_tick = 0;
    uint32_t last_status_log_tick = 0;
    bool preserve_activity_on_detach = false;
    bool standby_triggered = false;
    bool shutdown_triggered = false;
};

State s;

int ClampMinutes(int minutes) {
    if (minutes < 0) {
        return 0;
    }
    if (minutes > kMaxMinutes) {
        return kMaxMinutes;
    }
    return minutes;
}

uint32_t MinutesToMs(int minutes) {
    if (minutes <= 0) {
        return 0;
    }
    return static_cast<uint32_t>(minutes) * 60U * 1000U;
}

void EnsureTimer();
void StopTimer();

void OnIdleTick(lv_timer_t* /*timer*/) {
    if (s.session == IdlePowerSession::None) {
        return;
    }

    const uint32_t idle_ms = lv_tick_elaps(s.last_activity_tick);
    const uint32_t standby_ms = MinutesToMs(IdlePower_GetStandbyMinutes());
    const uint32_t shutdown_ms = MinutesToMs(IdlePower_GetShutdownMinutes());

    // 关机优先：阈值更短或与待机相同时先关机。
    if (shutdown_ms > 0 && idle_ms >= shutdown_ms) {
        if (!s.shutdown_triggered) {
            s.shutdown_triggered = true;
            ESP_LOGW(TAG,
                     "idle shutdown: idle=%u s, limit=%u s, session=%d",
                     idle_ms / 1000, shutdown_ms / 1000,
                     static_cast<int>(s.session));
            char reason[72];
            std::snprintf(reason, sizeof(reason),
                          I18n::T("无操作 %u 分钟自动关机"),
                          static_cast<unsigned>(shutdown_ms / (60U * 1000U)));
            HomeScreen::RequestSystemShutdown(reason);
        }
        return;
    }

    if (s.session == IdlePowerSession::Home && standby_ms > 0 &&
        idle_ms >= standby_ms) {
        if (!s.standby_triggered) {
            s.standby_triggered = true;
            ESP_LOGI(TAG, "idle enter standby: idle=%u s, limit=%u s",
                     idle_ms / 1000, standby_ms / 1000);
            IdlePower_PrepareAutoStandby();
            StandbyScreen::Show();
        }
        return;
    }

    if (lv_tick_elaps(s.last_status_log_tick) < kStatusLogIntervalMs) {
        return;
    }
    s.last_status_log_tick = lv_tick_get();

    uint32_t next_ms = 0;
    const char* next_action = "none";
    if (s.session == IdlePowerSession::Home && standby_ms > 0 &&
        (shutdown_ms == 0 || standby_ms < shutdown_ms)) {
        next_ms = standby_ms;
        next_action = "standby";
    } else if (shutdown_ms > 0) {
        next_ms = shutdown_ms;
        next_action = "shutdown";
    }
    if (next_ms > idle_ms) {
        ESP_LOGI(TAG, "idle=%u s, remaining=%u s until %s (session=%d)",
                 idle_ms / 1000, (next_ms - idle_ms) / 1000, next_action,
                 static_cast<int>(s.session));
    }
}

void StopTimer() {
    if (s.timer != nullptr) {
        lv_timer_delete(s.timer);
        s.timer = nullptr;
    }
}

void EnsureTimer() {
    if (s.timer != nullptr) {
        return;
    }
    s.timer = lv_timer_create(OnIdleTick, kTickPeriodMs, nullptr);
}

}  // namespace

void IdlePower_NotifyActivity() {
    s.last_activity_tick = lv_tick_get();
    s.last_status_log_tick = lv_tick_get();
    s.standby_triggered = false;
    s.shutdown_triggered = false;
}

void IdlePower_PrepareAutoStandby() {
    s.preserve_activity_on_detach = true;
}

void IdlePower_Attach(IdlePowerSession session, bool reset_activity) {
    if (session == IdlePowerSession::None) {
        return;
    }

    const bool keep_activity =
        s.preserve_activity_on_detach && session == IdlePowerSession::Standby;
    s.preserve_activity_on_detach = false;
    s.session = session;

    if (reset_activity && !keep_activity) {
        IdlePower_NotifyActivity();
    } else if (s.last_activity_tick == 0) {
        IdlePower_NotifyActivity();
    } else {
        // 延续累计空闲；允许再次触发待机标记在回到首页时清掉。
        if (session == IdlePowerSession::Standby) {
            s.standby_triggered = true;
        } else {
            s.standby_triggered = false;
        }
        s.shutdown_triggered = false;
        s.last_status_log_tick = lv_tick_get();
    }

    EnsureTimer();
    ESP_LOGI(TAG, "attach session=%d reset=%d keep=%d",
             static_cast<int>(session), reset_activity ? 1 : 0,
             keep_activity ? 1 : 0);
}

void IdlePower_Detach(IdlePowerSession session) {
    if (s.session != session) {
        return;
    }
    s.session = IdlePowerSession::None;

    if (s.preserve_activity_on_detach) {
        // 首页即将被自动待机替换：保留 tick 与 timer。
        ESP_LOGI(TAG, "detach home → preserving idle for standby");
        return;
    }

    StopTimer();
    ESP_LOGI(TAG, "detach session=%d", static_cast<int>(session));
}

void IdlePower_Stop() {
    s.preserve_activity_on_detach = false;
    s.session = IdlePowerSession::None;
    s.standby_triggered = false;
    s.shutdown_triggered = false;
    StopTimer();
}

int IdlePower_GetStandbyMinutes() {
    Settings settings("display", false);
    return ClampMinutes(
        settings.GetInt(kStandbyNvsKey, kDefaultStandbyMin));
}

void IdlePower_SetStandbyMinutes(int minutes) {
    minutes = ClampMinutes(minutes);
    Settings settings("display", true);
    settings.SetInt(kStandbyNvsKey, minutes);
    ESP_LOGI(TAG, "standby timeout updated to %d min", minutes);
}

int IdlePower_GetShutdownMinutes() {
    Settings settings("display", false);
    return ClampMinutes(
        settings.GetInt(kShutdownNvsKey, kDefaultShutdownMin));
}

void IdlePower_SetShutdownMinutes(int minutes) {
    minutes = ClampMinutes(minutes);
    Settings settings("display", true);
    settings.SetInt(kShutdownNvsKey, minutes);
    ESP_LOGI(TAG, "shutdown timeout updated to %d min", minutes);
}
