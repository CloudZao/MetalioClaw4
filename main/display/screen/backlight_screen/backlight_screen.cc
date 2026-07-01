#include "backlight_screen.h"

#include <cstdio>

#include <esp_log.h>

#include "backlight.h"
#include "board.h"
#include "home_screen/home_screen.h"
#include "screen_util.h"
#include "settings.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_number_50_4);

namespace {

constexpr const char* TAG = "BacklightScreen";

constexpr int kPanelSize = 720;
constexpr int kHeaderH = 90;
constexpr int kBackBtnSize = 72;

struct UiState {
    lv_obj_t* screen = nullptr;
    lv_obj_t* pct_label = nullptr;
    lv_obj_t* slider = nullptr;
};
UiState s_ui;

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

void UpdatePctLabel(int pct) {
    if (s_ui.pct_label == nullptr) {
        return;
    }
    if (pct < static_cast<int>(kBacklightMinPercent)) {
        pct = kBacklightMinPercent;
    } else if (pct > 100) {
        pct = 100;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(s_ui.pct_label, buf);
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

void OnSliderChanged(lv_event_t* e) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int value = static_cast<int>(lv_slider_get_value(slider));
    if (value < static_cast<int>(kBacklightMinPercent)) {
        value = kBacklightMinPercent;
        lv_slider_set_value(slider, value, LV_ANIM_OFF);
    }
    UpdatePctLabel(value);
    ApplyBrightness(value);
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
    s_ui.screen = nullptr;
    s_ui.pct_label = nullptr;
    s_ui.slider = nullptr;
}

lv_obj_t* CreateSlider(lv_obj_t* parent, int initial_brightness) {
    lv_obj_t* slider_wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(slider_wrap);
    lv_obj_set_width(slider_wrap, LV_PCT(100));
    lv_obj_set_height(slider_wrap, 52);
    lv_obj_set_style_bg_opa(slider_wrap, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(slider_wrap, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(slider_wrap, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* slider = lv_slider_create(slider_wrap);
    s_ui.slider = slider;
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_set_height(slider, 28);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
    lv_slider_set_range(slider, kBacklightMinPercent, 100);
    lv_slider_set_value(slider, initial_brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3B82F6), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_pad_hor(slider, 40, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(slider, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 10, LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider, OnSliderChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    screen_swipe_back_ignore(slider, true);
    return slider_wrap;
}

}  // namespace

lv_obj_t* BacklightScreen::Create() {
    const int initial_brightness = ReadInitialBrightness();

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E1116), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* header = lv_obj_create(scr);
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
    lv_label_set_text(title, "屏幕亮度");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);

    lv_obj_t* card = lv_obj_create(scr);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 660, 220);
    lv_obj_set_pos(card, 30, 110);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 28, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_make_input_passive(card);

    lv_obj_t* pct = lv_label_create(card);
    s_ui.pct_label = pct;
    lv_obj_set_width(pct, LV_PCT(100));
    lv_label_set_long_mode(pct, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(pct, lv_color_hex(0x60A5FA), LV_PART_MAIN);
    lv_obj_set_style_text_font(pct, &font_puhui_number_50_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(pct, LV_ALIGN_CENTER, 0, -20);
    UpdatePctLabel(initial_brightness);

    lv_obj_t* hint = lv_label_create(card);
    lv_label_set_text(hint, "当前亮度");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);

    lv_obj_t* slider_row = lv_obj_create(scr);
    lv_obj_remove_style_all(slider_row);
    lv_obj_set_size(slider_row, 660, LV_SIZE_CONTENT);
    lv_obj_set_pos(slider_row, 30, 360);
    lv_obj_set_flex_flow(slider_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(slider_row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(slider_row, 18, LV_PART_MAIN);
    lv_obj_add_flag(slider_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(slider_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* slider_hdr = lv_obj_create(slider_row);
    lv_obj_remove_style_all(slider_hdr);
    lv_obj_set_width(slider_hdr, LV_PCT(100));
    lv_obj_set_height(slider_hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(slider_hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(slider_hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(slider_hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* slider_lbl = lv_label_create(slider_hdr);
    lv_label_set_text(slider_lbl, "拖动调节");
    lv_obj_set_style_text_color(slider_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(slider_lbl, &font_puhui_20_4, LV_PART_MAIN);

    lv_obj_t* range_lbl = lv_label_create(slider_hdr);
    char range_buf[24];
    std::snprintf(range_buf, sizeof(range_buf), "%d%% ~ 100%%",
                  static_cast<int>(kBacklightMinPercent));
    lv_label_set_text(range_lbl, range_buf);
    lv_obj_set_style_text_color(range_lbl, lv_color_hex(0x9CA3AF), LV_PART_MAIN);
    lv_obj_set_style_text_font(range_lbl, &font_puhui_20_4, LV_PART_MAIN);

    CreateSlider(slider_row, initial_brightness);

    lv_obj_t* foot = lv_label_create(scr);
    lv_label_set_text(foot, "亮度设置会自动保存");
    lv_obj_set_style_text_color(foot, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(foot, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -40);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    return scr;
}

void BacklightScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: backlight_screen");
    } else {
        ESP_LOGI(TAG, "unload: backlight_screen");
    }
}
