#include "pin_gpio_test.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "driver/gpio.h"
#include "esp_log.h"
#include "screen_util.h"
#include "test_ui_common.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "PinGpioTest";

constexpr int kRowH      = 84;
constexpr int kRowGap    = 10;
constexpr int kCardPad   = 16;
constexpr int kCycleBtnH = 72;

constexpr uint32_t kColorAccent   = 0x3B82F6;
constexpr uint32_t kColorAccentHi = 0x60A5FA;
constexpr uint32_t kColorMuted    = 0x2A2F3A;
constexpr uint32_t kColorMutedHi  = 0x3A4050;
constexpr uint32_t kColorHigh     = 0x10B981;
constexpr uint32_t kColorLow      = 0x6B7280;

enum class Dir : uint8_t {
    kInput  = 0,
    kOutput = 1,
};

struct PinDef {
    int         index;
    const char* label;
};

constexpr PinDef kPins[] = {
    { 5, "GPIO5"},
    {14, "GPIO14"},
    {15, "GPIO15"},
    {16, "GPIO16"},
    {17, "GPIO17"},
    {18, "GPIO18"},
    {19, "GPIO19"},
    {20, "GPIO20"},
    {21, "GPIO21"},
    {23, "GPIO23"},
    {35, "GPIO35"},
    {46, "GPIO46"},
    {47, "GPIO47"},
    {48, "GPIO48"},
};
constexpr int kPinCount = static_cast<int>(sizeof(kPins) / sizeof(kPins[0]));

struct PinRowUi {
    lv_obj_t* row           = nullptr;
    lv_obj_t* dir_in_btn    = nullptr;
    lv_obj_t* dir_out_btn   = nullptr;
    lv_obj_t* out_switch    = nullptr;
    lv_obj_t* out_state_lbl = nullptr;
    lv_obj_t* in_level_dot  = nullptr;
    lv_obj_t* in_level_lbl  = nullptr;
};

struct PinState {
    Dir  dir         = Dir::kInput;
    bool current_lvl = false;
};

PinRowUi s_rows[kPinCount];
PinState s_state[kPinCount];

lv_obj_t*   s_card       = nullptr;
lv_obj_t*   s_cycle_btn  = nullptr;
lv_obj_t*   s_cycle_lbl  = nullptr;
lv_timer_t* s_input_timer = nullptr;
lv_timer_t* s_cycle_timer = nullptr;
bool        s_cycle_active = false;
bool        s_cycle_level  = false;

void ApplyPinDir(int i) {
    const PinDef& p = kPins[i];
    const PinState& s = s_state[i];
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << static_cast<uint64_t>(p.index));
    cfg.mode = (s.dir == Dir::kOutput) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gpio_config(GPIO%d, %s) failed: %s", p.index,
                 s.dir == Dir::kOutput ? "OUT" : "IN", esp_err_to_name(err));
        return;
    }
    if (s.dir == Dir::kOutput) {
        gpio_set_level(static_cast<gpio_num_t>(p.index), s.current_lvl ? 1 : 0);
    }
}

void WritePinLevel(int i, bool level) {
    PinState& s = s_state[i];
    if (s.dir != Dir::kOutput) {
        return;
    }
    s.current_lvl = level;
    gpio_set_level(static_cast<gpio_num_t>(kPins[i].index), level ? 1 : 0);
}

bool ReadPinLevel(int i) {
    return gpio_get_level(static_cast<gpio_num_t>(kPins[i].index)) != 0;
}

void RefreshDirButtons(int i) {
    const PinRowUi& r = s_rows[i];
    if (r.dir_in_btn == nullptr || r.dir_out_btn == nullptr) {
        return;
    }
    const bool in_active = (s_state[i].dir == Dir::kInput);

    lv_obj_set_style_bg_color(r.dir_in_btn,
        lv_color_hex(in_active ? kColorAccent : kColorMuted), LV_PART_MAIN);
    lv_obj_set_style_border_width(r.dir_in_btn, in_active ? 2 : 0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(r.dir_out_btn,
        lv_color_hex(!in_active ? kColorAccent : kColorMuted), LV_PART_MAIN);
    lv_obj_set_style_border_width(r.dir_out_btn, !in_active ? 2 : 0, LV_PART_MAIN);
}

void RefreshCtrl(int i) {
    const PinRowUi& r = s_rows[i];
    if (s_state[i].dir == Dir::kOutput) {
        if (r.out_switch != nullptr) {
            lv_obj_remove_flag(r.out_switch, LV_OBJ_FLAG_HIDDEN);
            if (s_state[i].current_lvl) {
                lv_obj_add_state(r.out_switch, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(r.out_switch, LV_STATE_CHECKED);
            }
        }
        if (r.out_state_lbl != nullptr) {
            lv_obj_remove_flag(r.out_state_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(r.out_state_lbl,
                              s_state[i].current_lvl ? "高" : "低");
            lv_obj_set_style_text_color(
                r.out_state_lbl,
                lv_color_hex(s_state[i].current_lvl ? kColorHigh : kColorLow),
                LV_PART_MAIN);
        }
        if (r.in_level_dot != nullptr) {
            lv_obj_add_flag(r.in_level_dot, LV_OBJ_FLAG_HIDDEN);
        }
        if (r.in_level_lbl != nullptr) {
            lv_obj_add_flag(r.in_level_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (r.out_switch != nullptr) {
            lv_obj_add_flag(r.out_switch, LV_OBJ_FLAG_HIDDEN);
        }
        if (r.out_state_lbl != nullptr) {
            lv_obj_add_flag(r.out_state_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        if (r.in_level_dot != nullptr) {
            lv_obj_remove_flag(r.in_level_dot, LV_OBJ_FLAG_HIDDEN);
        }
        if (r.in_level_lbl != nullptr) {
            lv_obj_remove_flag(r.in_level_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void RefreshInputDisplay(int i, bool lvl) {
    s_state[i].current_lvl = lvl;
    const PinRowUi& r = s_rows[i];
    if (r.in_level_dot != nullptr) {
        lv_obj_set_style_bg_color(r.in_level_dot,
            lv_color_hex(lvl ? kColorHigh : kColorLow), LV_PART_MAIN);
    }
    if (r.in_level_lbl != nullptr) {
        lv_label_set_text(r.in_level_lbl, lvl ? "高" : "低");
        lv_obj_set_style_text_color(r.in_level_lbl,
            lv_color_hex(lvl ? kColorHigh : kColorLow), LV_PART_MAIN);
    }
}

void RefreshAllInputs() {
    for (int i = 0; i < kPinCount; ++i) {
        if (s_state[i].dir != Dir::kInput) {
            continue;
        }
        RefreshInputDisplay(i, ReadPinLevel(i));
    }
}

void OnCycleTimer(lv_timer_t* /*t*/) {
    s_cycle_level = !s_cycle_level;
    for (int i = 0; i < kPinCount; ++i) {
        if (s_state[i].dir != Dir::kOutput) {
            continue;
        }
        WritePinLevel(i, s_cycle_level);
        const PinRowUi& r = s_rows[i];
        if (r.out_switch != nullptr) {
            if (s_cycle_level) {
                lv_obj_add_state(r.out_switch, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(r.out_switch, LV_STATE_CHECKED);
            }
        }
        if (r.out_state_lbl != nullptr) {
            lv_label_set_text(r.out_state_lbl, s_cycle_level ? "高" : "低");
            lv_obj_set_style_text_color(r.out_state_lbl,
                lv_color_hex(s_cycle_level ? kColorHigh : kColorLow),
                LV_PART_MAIN);
        }
    }
}

void StopCycle() {
    if (s_cycle_timer != nullptr) {
        lv_timer_delete(s_cycle_timer);
        s_cycle_timer = nullptr;
    }
    s_cycle_active = false;
    if (s_cycle_btn != nullptr) {
        lv_obj_set_style_bg_color(s_cycle_btn, lv_color_hex(kColorMuted),
                                  LV_PART_MAIN);
        lv_obj_set_style_border_width(s_cycle_btn, 0, LV_PART_MAIN);
    }
    if (s_cycle_lbl != nullptr) {
        lv_label_set_text(s_cycle_lbl, "全部输出 · 周期方波");
    }
}

void StartCycle() {
    for (int i = 0; i < kPinCount; ++i) {
        s_state[i].dir = Dir::kOutput;
        s_state[i].current_lvl = false;
        ApplyPinDir(i);
        RefreshDirButtons(i);
        RefreshCtrl(i);
    }
    s_cycle_level = false;
    s_cycle_active = true;
    if (s_cycle_btn != nullptr) {
        lv_obj_set_style_bg_color(s_cycle_btn, lv_color_hex(kColorAccent),
                                  LV_PART_MAIN);
        lv_obj_set_style_border_color(s_cycle_btn,
                                      lv_color_hex(kColorAccentHi),
                                      LV_PART_MAIN);
        lv_obj_set_style_border_width(s_cycle_btn, 2, LV_PART_MAIN);
    }
    if (s_cycle_lbl != nullptr) {
        lv_label_set_text(s_cycle_lbl, "周期方波运行中 · 点击停止");
    }
    if (s_cycle_timer == nullptr) {
        s_cycle_timer = lv_timer_create(OnCycleTimer, 1000, nullptr);
    }
}

void OnDirButtonClicked(lv_event_t* e) {
    const intptr_t encoded =
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    const int pin_idx = static_cast<int>(encoded >> 1);
    const Dir new_dir = (encoded & 1) ? Dir::kOutput : Dir::kInput;
    if (pin_idx < 0 || pin_idx >= kPinCount) {
        return;
    }
    if (s_state[pin_idx].dir == new_dir) {
        return;
    }
    if (s_cycle_active) {
        StopCycle();
    }

    s_state[pin_idx].dir = new_dir;
    s_state[pin_idx].current_lvl = false;
    ApplyPinDir(pin_idx);
    RefreshDirButtons(pin_idx);
    RefreshCtrl(pin_idx);
    if (new_dir == Dir::kInput) {
        RefreshInputDisplay(pin_idx, ReadPinLevel(pin_idx));
    }
}

void OnOutSwitchChanged(lv_event_t* e) {
    const int pin_idx =
        static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (pin_idx < 0 || pin_idx >= kPinCount) {
        return;
    }
    if (s_cycle_active) {
        StopCycle();
    }
    lv_obj_t* sw = lv_event_get_target_obj(e);
    const bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    WritePinLevel(pin_idx, checked);
    const PinRowUi& r = s_rows[pin_idx];
    if (r.out_state_lbl != nullptr) {
        lv_label_set_text(r.out_state_lbl, checked ? "高" : "低");
        lv_obj_set_style_text_color(r.out_state_lbl,
            lv_color_hex(checked ? kColorHigh : kColorLow), LV_PART_MAIN);
    }
}

void OnCycleBtnClicked(lv_event_t* /*e*/) {
    if (s_cycle_active) {
        StopCycle();
    } else {
        StartCycle();
    }
}

void OnInputPollTimer(lv_timer_t* /*t*/) {
    RefreshAllInputs();
}

void CreatePinRow(lv_obj_t* parent, int idx, int row_w) {
    constexpr int kLabelW  = 120;
    constexpr int kDirSegW = 180;
    constexpr int kDirBtnW = (kDirSegW - 6) / 2;
    constexpr int kCtrlW   = 280;

    lv_obj_t* row = lv_obj_create(parent);
    screen_strip_obj_chrome(row);
    lv_obj_set_size(row, row_w, kRowH);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x12151C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 12, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    screen_swipe_back_ignore(row, true);

    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, kPins[idx].label);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_width(label, kLabelW);

    lv_obj_t* seg = lv_obj_create(row);
    screen_strip_obj_chrome(seg);
    lv_obj_set_size(seg, kDirSegW, kRowH - 24);
    lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(seg, 6, LV_PART_MAIN);

    auto make_dir_btn = [&](const char* text, bool is_out) {
        lv_obj_t* b = lv_button_create(seg);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, kDirBtnW, kRowH - 28);
        lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_hex(kColorMuted), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(b, lv_color_hex(kColorAccentHi),
                                      LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
        screen_swipe_back_ignore(b, true);

        lv_obj_t* lbl = lv_label_create(b);
        lv_label_set_text(lbl, text);
        lv_obj_center(lbl);
        lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        const intptr_t enc =
            static_cast<intptr_t>((idx << 1) | (is_out ? 1 : 0));
        lv_obj_add_event_cb(b, OnDirButtonClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(enc));
        return b;
    };

    lv_obj_t* in_btn  = make_dir_btn("输入", false);
    lv_obj_t* out_btn = make_dir_btn("输出", true);

    lv_obj_t* ctrl = lv_obj_create(row);
    screen_strip_obj_chrome(ctrl);
    lv_obj_set_size(ctrl, kCtrlW, kRowH - 16);
    lv_obj_set_style_bg_opa(ctrl, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(ctrl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl, 12, LV_PART_MAIN);
    lv_obj_set_flex_grow(ctrl, 1);

    lv_obj_t* out_state = lv_label_create(ctrl);
    lv_label_set_text(out_state, "低");
    lv_obj_set_style_text_color(out_state, lv_color_hex(kColorLow),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(out_state, &font_puhui_20_4, LV_PART_MAIN);

    lv_obj_t* out_sw = lv_switch_create(ctrl);
    lv_obj_set_size(out_sw, 80, 40);
    lv_obj_set_style_bg_color(out_sw, lv_color_hex(kColorMuted), LV_PART_MAIN);
    lv_obj_set_style_bg_color(out_sw, lv_color_hex(kColorHigh),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(out_sw, OnOutSwitchChanged, LV_EVENT_VALUE_CHANGED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(idx)));
    screen_swipe_back_ignore(out_sw, true);

    lv_obj_t* in_dot = lv_obj_create(ctrl);
    lv_obj_remove_style_all(in_dot);
    lv_obj_set_size(in_dot, 22, 22);
    lv_obj_set_style_radius(in_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(in_dot, lv_color_hex(kColorLow), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(in_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(in_dot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* in_lvl_lbl = lv_label_create(ctrl);
    lv_label_set_text(in_lvl_lbl, "低");
    lv_obj_set_style_text_color(in_lvl_lbl, lv_color_hex(kColorLow),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(in_lvl_lbl, &font_puhui_20_4, LV_PART_MAIN);

    PinRowUi& ui = s_rows[idx];
    ui.row           = row;
    ui.dir_in_btn    = in_btn;
    ui.dir_out_btn   = out_btn;
    ui.out_switch    = out_sw;
    ui.out_state_lbl = out_state;
    ui.in_level_dot  = in_dot;
    ui.in_level_lbl  = in_lvl_lbl;
}

}  // namespace

namespace PinGpioTest {

void BuildRow(lv_obj_t* list) {
    constexpr int kCardW = kTestPanelW - 2 * kTestSideMargin;
    constexpr int kInnerW = kCardW - 2 * kCardPad;

    lv_obj_t* card = lv_obj_create(list);
    s_card = card;
    screen_strip_obj_chrome(card);
    lv_obj_set_width(card, kCardW);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(kTestColorCardBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, kCardPad, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, kRowGap, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t* title_row = lv_obj_create(card);
    screen_strip_obj_chrome(title_row);
    lv_obj_set_size(title_row, kInnerW, 48);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(title_row);
    lv_label_set_text(title, "引脚测试");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* hint = lv_label_create(title_row);
    lv_label_set_text(hint, "切换方向 · 控制电平");
    lv_obj_set_style_text_color(hint, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_RIGHT_MID, 0, 0);

    for (int i = 0; i < kPinCount; ++i) {
        s_state[i] = PinState{};
        s_rows[i] = PinRowUi{};
    }
    s_cycle_active = false;
    s_cycle_level = false;

    for (int i = 0; i < kPinCount; ++i) {
        CreatePinRow(card, i, kInnerW);
        ApplyPinDir(i);
        RefreshDirButtons(i);
        RefreshCtrl(i);
    }
    RefreshAllInputs();

    lv_obj_t* cycle_btn = lv_button_create(card);
    s_cycle_btn = cycle_btn;
    lv_obj_remove_style_all(cycle_btn);
    lv_obj_set_size(cycle_btn, kInnerW, kCycleBtnH);
    lv_obj_set_style_radius(cycle_btn, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cycle_btn, lv_color_hex(kColorMuted),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cycle_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(cycle_btn, lv_color_hex(kColorAccentHi),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(cycle_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cycle_btn, lv_color_hex(kColorMutedHi),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(cycle_btn, OnCycleBtnClicked, LV_EVENT_CLICKED,
                        nullptr);
    screen_swipe_back_ignore(cycle_btn, true);

    lv_obj_t* cycle_lbl = lv_label_create(cycle_btn);
    s_cycle_lbl = cycle_lbl;
    lv_label_set_text(cycle_lbl, "全部输出 · 周期方波");
    lv_obj_set_style_text_color(cycle_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(cycle_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(cycle_lbl);
    lv_obj_remove_flag(cycle_lbl, LV_OBJ_FLAG_CLICKABLE);
}

void OnLoad() {
    if (s_input_timer == nullptr) {
        s_input_timer = lv_timer_create(OnInputPollTimer, 200, nullptr);
    }
    RefreshAllInputs();
}

void OnUnload() {
    StopCycle();
    if (s_input_timer != nullptr) {
        lv_timer_delete(s_input_timer);
        s_input_timer = nullptr;
    }
    s_card = nullptr;
    s_cycle_btn = nullptr;
    s_cycle_lbl = nullptr;
    for (int i = 0; i < kPinCount; ++i) {
        s_rows[i] = PinRowUi{};
    }
}

void Poll() {
    // 输入轮询由 200ms 定时器负责；此处留空以匹配其它测试项接口。
}

}  // namespace PinGpioTest
