#include "standby_screen.h"
#include "i18n.h"

#include <cstdio>
#include <ctime>

#include <esp_log.h>

#include "board.h"
#include "home_screen/home_screen.h"
#include "idle_power_policy.h"
#include "pwr_key_handler.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_number_120_4);

namespace {

constexpr const char* TAG = "StandbyScreen";
constexpr int kPanelSize = 720;

constexpr int kChargeFxW = 560;
constexpr int kChargeFxH = 360;
constexpr int kParticleCount = 52;
constexpr uint32_t kChargeBlueSoft = 0x59B2FF;
constexpr uint32_t kChargeBlueBright = 0x9AD0FF;
constexpr uint32_t kChargeEffectMs = 10000;
constexpr uint32_t kParticleTickMs = 33;

// msgid 源文案（zh-CN）；显示时再 I18n::T，禁止在静态初始化里调用 T()。
constexpr const char* kWeekdayMsgIds[7] = {"日", "一", "二", "三", "四", "五", "六"};

struct Particle {
    lv_obj_t* obj = nullptr;
    float x = 0;
    float y = 0;
    float vx = 0;
    float vy = 0;
    float size = 6;
    float life = 0;      // 0..1，1 为新生
    float life_decay = 0;
};

struct UiState {
    lv_obj_t* screen = nullptr;
    lv_obj_t* time_lbl = nullptr;
    lv_obj_t* date_lbl = nullptr;
    lv_timer_t* update_timer = nullptr;

    lv_obj_t* charge_root = nullptr;
    lv_obj_t* charge_tip = nullptr;
    Particle particles[kParticleCount]{};
    lv_timer_t* charge_tick_timer = nullptr;
    lv_timer_t* charge_stop_timer = nullptr;
    uint32_t charge_rng = 1;
    bool charge_playing = false;
    bool last_charging = false;
    bool charge_primed = false;
};

UiState s_ui;

void StopChargeEffect();
void StartChargeEffect(int battery_level);

uint32_t ChargeRand() {
    // xorshift32
    uint32_t x = s_ui.charge_rng ? s_ui.charge_rng : 1u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_ui.charge_rng = x;
    return x;
}

float ChargeRand01() {
    return static_cast<float>(ChargeRand() & 0xFFFFu) / 65535.0f;
}

void RespawnParticle(Particle* p, bool birth_at_bottom) {
    if (p == nullptr || p->obj == nullptr) {
        return;
    }
    // 更宽的出生带 + 略快衰减，同屏活跃点更多、翻滚更密。
    const float spread = 260.0f;
    p->x = kChargeFxW * 0.5f + (ChargeRand01() - 0.5f) * spread;
    p->y = birth_at_bottom ? (kChargeFxH - 8.0f - ChargeRand01() * 36.0f)
                           : (kChargeFxH * (0.30f + ChargeRand01() * 0.60f));
    p->vx = (ChargeRand01() - 0.5f) * 2.2f;
    p->vy = -(1.6f + ChargeRand01() * 3.8f);  // 向上
    p->size = 3.0f + ChargeRand01() * 9.0f;
    p->life = 0.70f + ChargeRand01() * 0.30f;
    p->life_decay = 0.014f + ChargeRand01() * 0.022f;

    const int sz = static_cast<int>(p->size);
    lv_obj_set_size(p->obj, sz, sz);
    lv_obj_set_pos(p->obj, static_cast<int>(p->x - p->size * 0.5f),
                   static_cast<int>(p->y - p->size * 0.5f));

    const uint32_t color = (ChargeRand() & 1u) ? kChargeBlueBright : kChargeBlueSoft;
    lv_obj_set_style_bg_color(p->obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(p->obj, LV_OPA_COVER, LV_PART_MAIN);
}

void StopChargeEffect() {
    if (s_ui.charge_stop_timer != nullptr) {
        lv_timer_delete(s_ui.charge_stop_timer);
        s_ui.charge_stop_timer = nullptr;
    }
    if (s_ui.charge_tick_timer != nullptr) {
        lv_timer_delete(s_ui.charge_tick_timer);
        s_ui.charge_tick_timer = nullptr;
    }
    if (s_ui.charge_root != nullptr) {
        lv_obj_delete(s_ui.charge_root);
    }
    s_ui.charge_root = nullptr;
    s_ui.charge_tip = nullptr;
    for (Particle& p : s_ui.particles) {
        p = Particle{};
    }
    s_ui.charge_playing = false;
}

void OnChargeEffectTimeout(lv_timer_t* /*timer*/) {
    s_ui.charge_stop_timer = nullptr;
    StopChargeEffect();
}

void OnParticleTick(lv_timer_t* /*timer*/) {
    if (!s_ui.charge_playing || s_ui.charge_root == nullptr) {
        return;
    }

    for (Particle& p : s_ui.particles) {
        if (p.obj == nullptr) {
            continue;
        }

        p.x += p.vx;
        p.y += p.vy;
        // 轻微横向扩散，向上加速感。
        p.vx *= 0.992f;
        p.vy -= 0.035f;
        p.life -= p.life_decay;

        if (p.life <= 0.0f || p.y < -20.0f) {
            RespawnParticle(&p, true);
            continue;
        }

        const int sz_raw = static_cast<int>(p.size * (0.55f + 0.45f * p.life));
        const int sz = sz_raw < 2 ? 2 : sz_raw;
        int opa_i = static_cast<int>(p.life * 220.0f);
        if (opa_i < 0) {
            opa_i = 0;
        }
        if (opa_i > 220) {
            opa_i = 220;
        }
        lv_obj_set_size(p.obj, sz, sz);
        lv_obj_set_pos(p.obj, static_cast<int>(p.x - sz * 0.5f),
                       static_cast<int>(p.y - sz * 0.5f));
        lv_obj_set_style_bg_opa(p.obj, static_cast<lv_opa_t>(opa_i),
                                LV_PART_MAIN);
    }
}

void StartChargeEffect(int battery_level) {
    if (s_ui.screen == nullptr || s_ui.charge_playing) {
        return;
    }
    s_ui.charge_playing = true;
    s_ui.charge_rng = static_cast<uint32_t>(esp_log_timestamp()) | 1u;

    lv_obj_t* root = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, kChargeFxW, kChargeFxH);
    lv_obj_align(root, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(root, true, LV_PART_MAIN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
    s_ui.charge_root = root;

    for (int i = 0; i < kParticleCount; ++i) {
        Particle& p = s_ui.particles[i];
        p.obj = lv_obj_create(root);
        lv_obj_remove_style_all(p.obj);
        lv_obj_set_style_radius(p.obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(p.obj, 0, LV_PART_MAIN);
        lv_obj_remove_flag(p.obj, LV_OBJ_FLAG_CLICKABLE);
        RespawnParticle(&p, true);
        // 错开初始高度，看起来像在向上蔓延。
        p.y = kChargeFxH - 20.0f - ChargeRand01() * (kChargeFxH * 0.75f);
        p.life = ChargeRand01();
        lv_obj_set_pos(p.obj, static_cast<int>(p.x - p.size * 0.5f),
                       static_cast<int>(p.y - p.size * 0.5f));
    }

    lv_obj_t* tip = lv_label_create(root);
    char tip_buf[32];
    if (battery_level >= 0 && battery_level <= 100) {
        std::snprintf(tip_buf, sizeof(tip_buf), I18n::T("充电中  %d%%"), battery_level);
    } else {
        std::snprintf(tip_buf, sizeof(tip_buf), I18n::T("充电中"));
    }
    lv_label_set_text(tip, tip_buf);
    lv_obj_set_style_text_color(tip, lv_color_hex(kChargeBlueSoft),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(tip, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, -52);
    lv_obj_remove_flag(tip, LV_OBJ_FLAG_CLICKABLE);
    s_ui.charge_tip = tip;

    s_ui.charge_tick_timer =
        lv_timer_create(OnParticleTick, kParticleTickMs, nullptr);
    s_ui.charge_stop_timer =
        lv_timer_create(OnChargeEffectTimeout, kChargeEffectMs, nullptr);
    lv_timer_set_repeat_count(s_ui.charge_stop_timer, 1);

    ESP_LOGI(TAG, "charge particle effect start (%d%%, %u ms)", battery_level,
             static_cast<unsigned>(kChargeEffectMs));
}

void MaybeTriggerChargeEffect(bool charging, int battery_level) {
    const bool rising = charging && (!s_ui.charge_primed || !s_ui.last_charging);
    s_ui.last_charging = charging;
    s_ui.charge_primed = true;
    if (rising) {
        StartChargeEffect(battery_level);
    }
}

void UpdateClockLabels() {
    if (s_ui.time_lbl == nullptr || s_ui.date_lbl == nullptr) {
        return;
    }

    time_t now = time(nullptr);
    struct tm tm_info = {};
    const bool have_time =
        localtime_r(&now, &tm_info) != nullptr && tm_info.tm_year >= 2025 - 1900;

    if (!have_time) {
        // font_puhui_number_120_4 仅含 0-9 与 ':'，无有效时间时用占位。
        lv_label_set_text(s_ui.time_lbl, "00:00:00");
        lv_label_set_text(s_ui.date_lbl, I18n::T("----年--月--日 星期-"));
    } else {
        char time_str[16];
        std::strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm_info);
        lv_label_set_text(s_ui.time_lbl, time_str);

        const int wday = tm_info.tm_wday;
        const char* weekday =
            (wday >= 0 && wday < 7) ? I18n::T(kWeekdayMsgIds[wday]) : "-";

        char date_str[64];
        std::snprintf(date_str, sizeof(date_str), I18n::T("%04d年%02d月%02d日 星期%s"),
                      tm_info.tm_year + 1900, tm_info.tm_mon + 1,
                      tm_info.tm_mday, weekday);
        lv_label_set_text(s_ui.date_lbl, date_str);
    }

    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (Board::GetInstance().GetBatteryLevel(battery_level, charging,
                                             discharging)) {
        if (battery_level < 0) {
            battery_level = 0;
        }
        if (battery_level > 100) {
            battery_level = 100;
        }
        MaybeTriggerChargeEffect(charging, battery_level);
    }
}

void OnClockTimer(lv_timer_t* /*timer*/) { UpdateClockLabels(); }

void OnScreenUnloaded(lv_event_t* /*e*/) {
    IdlePower_Detach(IdlePowerSession::Standby);
    StopChargeEffect();
    if (s_ui.update_timer != nullptr) {
        lv_timer_delete(s_ui.update_timer);
        s_ui.update_timer = nullptr;
    }
    s_ui = UiState{};
}

void OnStandbyClicked(lv_event_t* e) {
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) {
        return;
    }
    StandbyScreen::ReturnHome();
}

void standby_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("standby", event);
    StandbyScreen::LifecycleCallback(event);
}

}  // namespace

lv_obj_t* StandbyScreen::Create() {
    lv_obj_t* screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, OnStandbyClicked, LV_EVENT_CLICKED, nullptr);
    s_ui.screen = screen;

    lv_obj_t* box = lv_obj_create(screen);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_row(box, 28, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    s_ui.time_lbl = lv_label_create(box);
    lv_label_set_text(s_ui.time_lbl, "00:00:00");
    lv_obj_set_style_text_color(s_ui.time_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.time_lbl, &font_puhui_number_120_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.time_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_remove_flag(s_ui.time_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_ui.date_lbl = lv_label_create(box);
    lv_label_set_text(s_ui.date_lbl, I18n::T("----年--月--日 星期-"));
    lv_obj_set_style_text_color(s_ui.date_lbl, lv_color_hex(0xE5E7EB),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.date_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.date_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_remove_flag(s_ui.date_lbl, LV_OBJ_FLAG_CLICKABLE);

    UpdateClockLabels();
    s_ui.update_timer = lv_timer_create(OnClockTimer, 1000, nullptr);

    lv_obj_add_event_cb(screen, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    return screen;
}

void StandbyScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: standby_screen");
        // reset_activity=false：自动待机保留累计；手动进入时 Prepare 未置位也会
        // 在 Attach 里因 keep=false 且 reset=false 保留旧 tick——手动路径应重置。
        IdlePower_Attach(IdlePowerSession::Standby, /*reset_activity=*/true);
    } else {
        ESP_LOGI(TAG, "unload: standby_screen");
    }
}

void StandbyScreen::Show() {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = StandbyScreen::Create();
    screen_attach_lifecycle(app, standby_lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void StandbyScreen::ReturnHome() {
    IdlePower_NotifyActivity();
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}
