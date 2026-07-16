#include "settings_screen.h"

#include <cstdint>
#include <cstdio>

#include <esp_log.h>

#include "audio_codec.h"
#include "backlight.h"
#include "bluetooth_screen/bluetooth_screen.h"
#include "board.h"
#include "home_screen/home_screen.h"
#include "i18n.h"
#include "screen_util.h"
#include "settings.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_number_50_4);

namespace {

constexpr const char* TAG = "SettingsScreen";

constexpr int kPanelSize = 720;
constexpr int kHeaderH = 90;
constexpr int kBackBtnSize = 72;
constexpr int kTabBarW = 120;
constexpr int kTabItemH = 64;
constexpr int kTabItemGap = 10;
constexpr int kBodyH = kPanelSize - kHeaderH;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorSubtle = 0x9AA3B2;
constexpr uint32_t kColorCard = 0x1B2030;
constexpr uint32_t kColorTabBar = 0x12151C;
constexpr uint32_t kColorAccent = 0x3B82F6;
constexpr uint32_t kColorValue = 0x60A5FA;
constexpr uint32_t kColorSliderTrack = 0x2A2F3A;

struct UiState {
    lv_obj_t* screen = nullptr;
    lv_obj_t* tabview = nullptr;
    lv_obj_t* brightness_pct_label = nullptr;
    lv_obj_t* brightness_slider = nullptr;
    lv_obj_t* volume_pct_label = nullptr;
    lv_obj_t* volume_slider = nullptr;
    lv_obj_t* enter_standby_min_label = nullptr;
    lv_obj_t* enter_standby_slider = nullptr;
    lv_obj_t* shutdown_min_label = nullptr;
    lv_obj_t* shutdown_slider = nullptr;
};
UiState s_ui;

void OnSwipeBack();
void OnBackClicked(lv_event_t* e);

int ReadInitialBrightness() {
    int value = kBacklightDefaultPercent;
    if (Backlight* backlight = Board::GetInstance().GetBacklight()) {
        value = backlight->brightness();
    } else {
        Settings settings("display");
        value = settings.GetInt("brightness", kBacklightDefaultPercent);
    }
    if (value < static_cast<int>(kBacklightMinPercent)) {
        value = kBacklightMinPercent;
    }
    if (value > 100) {
        value = 100;
    }
    return value;
}

int ReadInitialVolume() {
    int volume = 70;
    if (AudioCodec* codec = Board::GetInstance().GetAudioCodec()) {
        volume = codec->output_volume();
    }
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    return volume;
}

void UpdatePctLabel(lv_obj_t* label, int pct) {
    if (label == nullptr) {
        return;
    }
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(label, buf);
}

void ApplyBrightness(int value) {
    if (value < static_cast<int>(kBacklightMinPercent)) {
        value = kBacklightMinPercent;
    }
    Backlight* backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr) {
        backlight->SetBrightness(static_cast<uint8_t>(value), true);
    }
}

void ApplyVolume(int volume) {
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    UpdatePctLabel(s_ui.volume_pct_label, volume);

    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr || codec->output_volume() == volume) {
        return;
    }
    codec->SetOutputVolume(volume);
}

void OnBrightnessSliderChanged(lv_event_t* e) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int value = static_cast<int>(lv_slider_get_value(slider));
    if (value < static_cast<int>(kBacklightMinPercent)) {
        value = kBacklightMinPercent;
        lv_slider_set_value(slider, value, LV_ANIM_OFF);
    }
    UpdatePctLabel(s_ui.brightness_pct_label, value);
    ApplyBrightness(value);
}

void OnVolumeSliderChanged(lv_event_t* e) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int value = static_cast<int>(lv_slider_get_value(slider));
    ApplyVolume(value);
    if (value != static_cast<int>(lv_slider_get_value(slider))) {
        lv_slider_set_value(slider, value, LV_ANIM_OFF);
    }
}

void StyleSlider(lv_obj_t* slider) {
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_set_height(slider, 28);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorSliderTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_pad_hor(slider, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(slider, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 10, LV_PART_INDICATOR);
    screen_swipe_back_ignore(slider, true);
}

lv_obj_t* CreateSliderRow(lv_obj_t* parent, int min_value, int max_value,
                          int initial_value, lv_event_cb_t cb,
                          lv_obj_t** out_slider) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 52);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* slider = lv_slider_create(row);
    if (out_slider != nullptr) {
        *out_slider = slider;
    }
    StyleSlider(slider);
    lv_slider_set_range(slider, min_value, max_value);
    lv_slider_set_value(slider, initial_value, LV_ANIM_OFF);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return row;
}

void BuildSliderPanel(lv_obj_t* parent, const char* title, const char* hint,
                      const char* range_hint, int initial_value,
                      lv_obj_t** pct_label_out, lv_obj_t** slider_out,
                      int slider_min, int slider_max, lv_event_cb_t slider_cb,
                      int card_height = 180) {
    lv_obj_set_style_pad_all(parent, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_row(parent, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = lv_obj_create(parent);
    screen_strip_obj_chrome(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, card_height);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_make_input_passive(card);

    lv_obj_t* pct = lv_label_create(card);
    if (pct_label_out != nullptr) {
        *pct_label_out = pct;
    }
    lv_obj_set_width(pct, LV_PCT(100));
    lv_label_set_long_mode(pct, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(pct, lv_color_hex(kColorValue), LV_PART_MAIN);
    lv_obj_set_style_text_font(pct, &font_puhui_number_50_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    const int value_y = card_height <= 150 ? -10 : -16;
    const int hint_y = card_height <= 150 ? -12 : -20;
    lv_obj_align(pct, LV_ALIGN_CENTER, 0, value_y);
    UpdatePctLabel(pct, initial_value);

    lv_obj_t* card_hint = lv_label_create(card);
    lv_label_set_text(card_hint, hint);
    lv_obj_set_style_text_color(card_hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(card_hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(card_hint, LV_ALIGN_BOTTOM_MID, 0, hint_y);

    lv_obj_t* slider_hdr = lv_obj_create(parent);
    lv_obj_remove_style_all(slider_hdr);
    lv_obj_set_width(slider_hdr, LV_PCT(100));
    lv_obj_set_height(slider_hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(slider_hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(slider_hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(slider_hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* slider_title = lv_label_create(slider_hdr);
    lv_label_set_text(slider_title, title);
    lv_obj_set_style_text_color(slider_title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(slider_title, &font_puhui_20_4, LV_PART_MAIN);

    lv_obj_t* range_lbl = lv_label_create(slider_hdr);
    lv_label_set_text(range_lbl, range_hint);
    lv_obj_set_style_text_color(range_lbl, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(range_lbl, &font_puhui_20_4, LV_PART_MAIN);

    CreateSliderRow(parent, slider_min, slider_max, initial_value, slider_cb,
                    slider_out);
}

void BuildBrightnessTab(lv_obj_t* tab, int initial_brightness) {
    char range_buf[24];
    std::snprintf(range_buf, sizeof(range_buf), "%d%% ~ 100%%",
                  static_cast<int>(kBacklightMinPercent));
    BuildSliderPanel(tab, I18n::T("拖动调节"), I18n::T("当前亮度"), range_buf,
                     initial_brightness, &s_ui.brightness_pct_label,
                     &s_ui.brightness_slider,
                     static_cast<int>(kBacklightMinPercent), 100,
                     OnBrightnessSliderChanged);

    lv_obj_t* foot = lv_label_create(tab);
    lv_label_set_text(foot, I18n::T("亮度设置会自动保存"));
    lv_obj_set_style_text_color(foot, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(foot, &font_puhui_20_4, LV_PART_MAIN);
}

void UpdateMinutesLabel(lv_obj_t* label, int minutes, const char* never_text) {
    if (label == nullptr) {
        return;
    }
    char buf[24];
    if (minutes <= 0) {
        std::snprintf(buf, sizeof(buf), "%s", never_text);
    } else {
        std::snprintf(buf, sizeof(buf), I18n::T("%d 分钟"), minutes);
    }
    lv_label_set_text(label, buf);
}

void OnEnterStandbySliderChanged(lv_event_t* e) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int value = static_cast<int>(lv_slider_get_value(slider));
    UpdateMinutesLabel(s_ui.enter_standby_min_label, value, I18n::T("永不进入"));
    HomeScreen::SetIdleStandbyMinutes(value);
}

void OnShutdownSliderChanged(lv_event_t* e) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int value = static_cast<int>(lv_slider_get_value(slider));
    UpdateMinutesLabel(s_ui.shutdown_min_label, value, I18n::T("永不关机"));
    HomeScreen::SetIdleShutdownMinutes(value);
}

void BuildStandbyTab(lv_obj_t* tab) {
    const int initial_standby = HomeScreen::GetIdleStandbyMinutes();
    const int initial_shutdown = HomeScreen::GetIdleShutdownMinutes();
    constexpr int kStandbyCardH = 132;

    BuildSliderPanel(tab, I18n::T("进入待机"), I18n::T("首页无操作后进入待机页"),
                     I18n::T("0 ~ 60 分钟"), initial_standby,
                     &s_ui.enter_standby_min_label, &s_ui.enter_standby_slider,
                     0, 60, OnEnterStandbySliderChanged, kStandbyCardH);
    UpdateMinutesLabel(s_ui.enter_standby_min_label, initial_standby,
                       I18n::T("永不进入"));

    BuildSliderPanel(tab, I18n::T("自动关机"),
                     I18n::T("首页+待机累计无操作后关机"), I18n::T("0 ~ 60 分钟"),
                     initial_shutdown, &s_ui.shutdown_min_label,
                     &s_ui.shutdown_slider, 0, 60, OnShutdownSliderChanged,
                     kStandbyCardH);
    UpdateMinutesLabel(s_ui.shutdown_min_label, initial_shutdown,
                       I18n::T("永不关机"));

    // 待机 Tab 内容更密：略收紧行距，并加大底部留白。
    lv_obj_set_style_pad_row(tab, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(tab, 56, LV_PART_MAIN);

    lv_obj_t* foot = lv_label_create(tab);
    lv_label_set_text(foot, I18n::T("待机设置会自动保存"));
    lv_obj_set_style_text_color(foot, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(foot, &font_puhui_20_4, LV_PART_MAIN);
}

void BuildVolumeTab(lv_obj_t* tab, int initial_volume) {
    BuildSliderPanel(tab, I18n::T("拖动调节"), I18n::T("当前音量"), "0% ~ 100%",
                     initial_volume, &s_ui.volume_pct_label, &s_ui.volume_slider,
                     0, 100, OnVolumeSliderChanged);

    lv_obj_t* foot = lv_label_create(tab);
    lv_label_set_text(foot, I18n::T("音量设置会自动保存"));
    lv_obj_set_style_text_color(foot, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(foot, &font_puhui_20_4, LV_PART_MAIN);
}

void BuildHeader(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("设置"));
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);
}

void GoHomeAfterLocaleChange() {
    HomeScreen::ResetToFirstPage();
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void OnLanguageCardClicked(lv_event_t* e) {
    auto locale = static_cast<I18n::Locale>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    if (!I18n::SetLocale(locale)) {
        return;
    }
    ESP_LOGI(TAG, "language -> %s (rebuild home)", I18n::GetLocaleCode());
    GoHomeAfterLocaleChange();
}

void BuildLanguageTab(lv_obj_t* tab) {
    lv_obj_set_style_pad_all(tab, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tab, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* hint = lv_label_create(tab);
    lv_label_set_text(hint, I18n::T("选择界面显示语言"));
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);

    const I18n::Locale current = I18n::GetLocale();
    for (size_t i = 0; i < I18n::GetLocaleCount(); ++i) {
        const I18n::LocaleInfo* info =
            I18n::GetLocaleInfo(static_cast<I18n::Locale>(i));
        if (info == nullptr) {
            continue;
        }

        lv_obj_t* card = lv_obj_create(tab);
        screen_strip_obj_chrome(card);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, 88);
        lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
        const bool selected = (info->id == current);
        lv_obj_set_style_border_color(
            card, lv_color_hex(selected ? kColorAccent : kColorCard),
            LV_PART_MAIN);
        lv_obj_set_style_border_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, OnLanguageCardClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(
                                static_cast<unsigned>(info->id))));
        screen_swipe_back_ignore(card, true);

        lv_obj_t* name = lv_label_create(card);
        // Native name stays in its own script (简体中文 / English).
        lv_label_set_text(name, info->native_name);
        lv_obj_set_style_text_color(name, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(name, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 20, -10);

        lv_obj_t* code = lv_label_create(card);
        lv_label_set_text(code, info->english_name);
        lv_obj_set_style_text_color(code, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(code, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(code, LV_ALIGN_LEFT_MID, 20, 18);

        if (selected) {
            lv_obj_t* mark = lv_label_create(card);
            lv_label_set_text(mark, I18n::T("当前语言"));
            lv_obj_set_style_text_color(mark, lv_color_hex(kColorValue),
                                        LV_PART_MAIN);
            lv_obj_set_style_text_font(mark, &font_puhui_20_4, LV_PART_MAIN);
            lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -20, 0);
        }
    }

    lv_obj_t* foot = lv_label_create(tab);
    lv_label_set_text(foot, I18n::T("切换后立即生效并返回主页"));
    lv_obj_set_style_text_color(foot, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(foot, &font_puhui_20_4, LV_PART_MAIN);
}

void FixTabBarItemHeights(lv_obj_t* tabview) {
    lv_obj_t* bar = lv_tabview_get_tab_bar(tabview);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(bar, kTabItemGap, LV_PART_MAIN);
    lv_obj_set_style_pad_top(bar, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, 8, LV_PART_MAIN);

    const uint32_t count = lv_tabview_get_tab_count(tabview);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* btn = lv_obj_get_child_by_type(bar, i, &lv_button_class);
        if (btn == nullptr) {
            continue;
        }
        lv_obj_set_flex_grow(btn, 0);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, kTabItemH);
        lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    }
}

void BuildBluetoothTab(lv_obj_t* tab) {
    lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tab, 0, LV_PART_MAIN);
    lv_obj_add_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    BluetoothScreen::BuildInto(tab);
}

void BuildTabView(lv_obj_t* parent) {
    const int initial_brightness = ReadInitialBrightness();
    const int initial_volume = ReadInitialVolume();

    lv_obj_t* tv = lv_tabview_create(parent);
    s_ui.tabview = tv;
    lv_obj_set_size(tv, kPanelSize, kBodyH);
    lv_obj_set_pos(tv, 0, kHeaderH);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_LEFT);
    lv_tabview_set_tab_bar_size(tv, kTabBarW);

    lv_obj_set_style_bg_color(tv, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tv, 0, LV_PART_MAIN);

    lv_obj_t* bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorTabBar), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(bar, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(bar, 6, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorAccent),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER,
                            LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText),
                                LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_t* content = lv_tabview_get_content(tv);
    screen_swipe_back_ignore(content, true);

    lv_obj_t* tab_brightness = lv_tabview_add_tab(tv, I18n::T("亮度"));
    BuildBrightnessTab(tab_brightness, initial_brightness);

    lv_obj_t* tab_standby = lv_tabview_add_tab(tv, I18n::T("待机"));
    BuildStandbyTab(tab_standby);

    lv_obj_t* tab_volume = lv_tabview_add_tab(tv, I18n::T("音量"));
    BuildVolumeTab(tab_volume, initial_volume);

    lv_obj_t* tab_language = lv_tabview_add_tab(tv, I18n::T("语言"));
    BuildLanguageTab(tab_language);

    lv_obj_t* tab_bluetooth = lv_tabview_add_tab(tv, I18n::T("蓝牙"));
    BuildBluetoothTab(tab_bluetooth);

    FixTabBarItemHeights(tv);
}

void OnSwipeBack() {
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void OnBackClicked(lv_event_t* /*e*/) { OnSwipeBack(); }

void OnScreenUnloaded(lv_event_t* /*e*/) {
    BluetoothScreen::ResetUi();
    s_ui.screen = nullptr;
    s_ui.tabview = nullptr;
    s_ui.brightness_pct_label = nullptr;
    s_ui.brightness_slider = nullptr;
    s_ui.volume_pct_label = nullptr;
    s_ui.volume_slider = nullptr;
    s_ui.enter_standby_min_label = nullptr;
    s_ui.enter_standby_slider = nullptr;
    s_ui.shutdown_min_label = nullptr;
    s_ui.shutdown_slider = nullptr;
}

}  // namespace

lv_obj_t* SettingsScreen::Create() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);
    BuildTabView(scr);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    return scr;
}

void SettingsScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: settings_screen");
    } else {
        ESP_LOGI(TAG, "unload: settings_screen");
    }
    BluetoothScreen::LifecycleCallback(event);
}
