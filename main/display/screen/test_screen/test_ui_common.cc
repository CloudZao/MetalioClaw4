#include "test_ui_common.h"

#include "screen_util.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

lv_obj_t* s_test_screen = nullptr;
lv_obj_t* s_confirm_mask = nullptr;
TestUiConfirmResultCb s_confirm_cb = nullptr;
void* s_confirm_user_data = nullptr;

struct ConfirmBtnCtx {
    bool pass;
};

void CloseConfirmDialog() {
    if (s_confirm_mask != nullptr) {
        lv_obj_delete(s_confirm_mask);
        s_confirm_mask = nullptr;
    }
    s_confirm_cb = nullptr;
    s_confirm_user_data = nullptr;
}

void OnConfirmBtnClicked(lv_event_t* e) {
    auto* ctx = static_cast<ConfirmBtnCtx*>(lv_event_get_user_data(e));
    if (ctx == nullptr) {
        return;
    }
    TestUiConfirmResultCb cb = s_confirm_cb;
    void* user_data = s_confirm_user_data;
    const bool pass = ctx->pass;
    CloseConfirmDialog();
    if (cb != nullptr) {
        cb(pass, user_data);
    }
}

lv_obj_t* CreateConfirmButton(lv_obj_t* parent, const char* text,
                              uint32_t bg_color, bool pass) {
    static ConfirmBtnCtx yes_ctx{true};
    static ConfirmBtnCtx no_ctx{false};

    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 180, 64);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color | 0x00101010),
                              LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(lbl);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(btn, OnConfirmBtnClicked, LV_EVENT_CLICKED,
                        pass ? static_cast<void*>(&yes_ctx)
                             : static_cast<void*>(&no_ctx));
    screen_swipe_back_ignore(btn, true);
    return btn;
}

}  // namespace

void TestUiSetScreen(lv_obj_t* scr) {
    s_test_screen = scr;
}

lv_obj_t* TestUiGetScreen() {
    return s_test_screen;
}

void TestUiShowConfirmDialog(const char* message, TestUiConfirmResultCb cb,
                             void* user_data) {
    if (s_test_screen == nullptr || message == nullptr || cb == nullptr) {
        return;
    }
    TestUiDismissConfirmDialog();

    s_confirm_cb = cb;
    s_confirm_user_data = user_data;

    constexpr int32_t kCardW = 520;
    constexpr int32_t kCardH = 280;

    lv_obj_t* mask = lv_obj_create(s_test_screen);
    s_confirm_mask = mask;
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kTestPanelW, kTestPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(mask, true);

    lv_obj_t* card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(kTestColorCardBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 28, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(card);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, kCardW - 56);
    lv_label_set_text(lbl, message);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* btn_row = lv_obj_create(card);
    screen_strip_obj_chrome(btn_row);
    lv_obj_set_size(btn_row, kCardW - 56, 72);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    CreateConfirmButton(btn_row, "否", kTestColorError, false);
    CreateConfirmButton(btn_row, "是", kTestColorHigh, true);
}

void TestUiDismissConfirmDialog() {
    CloseConfirmDialog();
}

lv_obj_t* TestUiCreateRowShell(lv_obj_t* parent, const char* title,
                               lv_obj_t** out_status_icon,
                               lv_obj_t** out_right_ctrl) {
    constexpr int kRowInnerPad = 16;
    constexpr int kLabelW      = 180;
    constexpr int kStatusIconSz = 36;

    lv_obj_t* row = lv_obj_create(parent);
    screen_strip_obj_chrome(row);
    lv_obj_set_size(row, kTestPanelW - 2 * kTestSideMargin, kTestRowH);
    lv_obj_set_style_bg_color(row, lv_color_hex(kTestColorCardBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, kRowInnerPad, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);

    const int status_w = (out_status_icon != nullptr) ? kStatusIconSz : 0;
    if (out_status_icon != nullptr) {
        lv_obj_t* status = lv_image_create(row);
        lv_obj_set_size(status, kStatusIconSz, kStatusIconSz);
        lv_obj_remove_flag(status, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(status, LV_OBJ_FLAG_HIDDEN);
        *out_status_icon = status;
    }

    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_width(label, kLabelW);

    lv_obj_t* ctrl = lv_obj_create(row);
    screen_strip_obj_chrome(ctrl);
    lv_obj_set_size(ctrl, kTestPanelW - 2 * kTestSideMargin - kLabelW - status_w -
                        2 * kRowInnerPad - (status_w > 0 ? 20 : 10),
                    kTestRowH - 16);
    lv_obj_set_style_bg_opa(ctrl, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(ctrl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_flex_grow(ctrl, 1);

    if (out_right_ctrl != nullptr) {
        *out_right_ctrl = ctrl;
    }
    return row;
}

void TestUiUpdateStatus(lv_obj_t* status_icon, bool pass) {
    if (status_icon == nullptr) {
        return;
    }
    lv_image_set_src(status_icon, pass ? kTestIconPass : kTestIconFail);
    lv_obj_remove_flag(status_icon, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* TestUiCreateValueLabel(lv_obj_t* right_ctrl) {
    lv_obj_t* lbl = lv_label_create(right_ctrl);
    lv_label_set_text(lbl, "--");
    lv_obj_set_style_text_color(lbl, lv_color_hex(kTestColorTextDim), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    return lbl;
}

lv_obj_t* TestUiCreateSwitch(lv_obj_t* right_ctrl, lv_event_cb_t cb,
                             void* user_data) {
    lv_obj_t* sw = lv_switch_create(right_ctrl);
    lv_obj_set_size(sw, 88, 44);
    lv_obj_set_style_bg_color(sw, lv_color_hex(kTestColorMuted), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(kTestColorHigh),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (cb != nullptr) {
        lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user_data);
    }
    screen_swipe_back_ignore(sw, true);
    return sw;
}
