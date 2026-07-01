#include "calculator_screen.h"

#include "home_screen/home_screen.h"
#include "screen_util.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lvgl.h"

LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_20_4);

// ---------------------------------------------------------------------------
// 720x720 layout
//
//  +-----------------------------------------------+ y=0
//  |  计算器                              [返回]  |  header  (h=80)
//  +-----------------------------------------------+ y=80
//  |                                  12 +         |  history (h=40)
//  |                                                |
//  |                              123              |  display (h=120)
//  +-----------------------------------------------+ y=240
//  |  [AC] [+/-] [%]  [/]                          |
//  |  [7]  [8]   [9]  [*]                          |  4 cols x 5 rows
//  |  [4]  [5]   [6]  [-]                          |  buttons grid
//  |  [1]  [2]   [3]  [+]                          |
//  |  [   0   ]  [.]  [=]                          |  (0 spans 2 cols)
//  +-----------------------------------------------+ y=720
// ---------------------------------------------------------------------------

namespace {

// ----- screen layout constants ---------------------------------------------
constexpr int kPanelSize  = 720;
constexpr int kPad        = 16;
constexpr int kHeaderH    = 80;
constexpr int kHistoryH   = 40;
constexpr int kDisplayH   = 120;
constexpr int kGridY      = kHeaderH + kHistoryH + kDisplayH;     // 240
constexpr int kGridH      = kPanelSize - kGridY - kPad;           // 464
constexpr int kGridCols   = 4;
constexpr int kGridRows   = 5;

// ----- color palette (iOS-inspired dark) -----------------------------------
constexpr uint32_t kColorBg          = 0x000000;
constexpr uint32_t kColorTextPrimary = 0xFFFFFF;
constexpr uint32_t kColorTextHistory = 0x9A9A9A;

constexpr uint32_t kDigitBg          = 0x333333;
constexpr uint32_t kDigitBgPress     = 0x595959;
constexpr uint32_t kDigitText        = 0xFFFFFF;

constexpr uint32_t kFuncBg           = 0xA5A5A5;
constexpr uint32_t kFuncBgPress      = 0xD0D0D0;
constexpr uint32_t kFuncText         = 0x1A1A1A;

constexpr uint32_t kOpBg             = 0xFF9500;
constexpr uint32_t kOpBgPress        = 0xFFB04D;
constexpr uint32_t kOpText           = 0xFFFFFF;

constexpr uint32_t kColorHintText    = 0x6E6E70;

// ----- calculator state machine --------------------------------------------
enum class Action {
    kDigit, kDot, kClear, kBackspace, kSign, kPercent,
    kOpAdd, kOpSub, kOpMul, kOpDiv, kEquals,
};

struct Btn {
    const char* label;
    Action action;
    char digit;          // valid only when action == kDigit
    int row, col;
    int row_span, col_span;
    enum class Style { kDigit, kFunc, kOp } style;
};

// Layout table -- this is the single source of truth for the keypad.
const Btn kButtons[] = {
    // row 0: function row
    {"AC",  Action::kClear,     0, 0, 0, 1, 1, Btn::Style::kFunc},
    {"+/-", Action::kSign,      0, 0, 1, 1, 1, Btn::Style::kFunc},
    {"%",   Action::kPercent,   0, 0, 2, 1, 1, Btn::Style::kFunc},
    {"/",   Action::kOpDiv,     0, 0, 3, 1, 1, Btn::Style::kOp},
    // row 1
    {"7",   Action::kDigit,   '7', 1, 0, 1, 1, Btn::Style::kDigit},
    {"8",   Action::kDigit,   '8', 1, 1, 1, 1, Btn::Style::kDigit},
    {"9",   Action::kDigit,   '9', 1, 2, 1, 1, Btn::Style::kDigit},
    {"x",   Action::kOpMul,     0, 1, 3, 1, 1, Btn::Style::kOp},
    // row 2
    {"4",   Action::kDigit,   '4', 2, 0, 1, 1, Btn::Style::kDigit},
    {"5",   Action::kDigit,   '5', 2, 1, 1, 1, Btn::Style::kDigit},
    {"6",   Action::kDigit,   '6', 2, 2, 1, 1, Btn::Style::kDigit},
    {"-",   Action::kOpSub,     0, 2, 3, 1, 1, Btn::Style::kOp},
    // row 3
    {"1",   Action::kDigit,   '1', 3, 0, 1, 1, Btn::Style::kDigit},
    {"2",   Action::kDigit,   '2', 3, 1, 1, 1, Btn::Style::kDigit},
    {"3",   Action::kDigit,   '3', 3, 2, 1, 1, Btn::Style::kDigit},
    {"+",   Action::kOpAdd,     0, 3, 3, 1, 1, Btn::Style::kOp},
    // row 4 -- "0" spans 2 columns
    {"0",   Action::kDigit,   '0', 4, 0, 1, 2, Btn::Style::kDigit},
    {".",   Action::kDot,       0, 4, 2, 1, 1, Btn::Style::kDigit},
    {"=",   Action::kEquals,    0, 4, 3, 1, 1, Btn::Style::kOp},
};

double s_acc;             // accumulator
bool   s_has_acc;         // is accumulator valid
char   s_input[24];       // currently typed number
int    s_pending_op;      // 0, '+', '-', '*', '/'
bool   s_just_eq;         // last action was '='
bool   s_error;           // last operation produced NaN/inf

lv_obj_t* s_history_lbl;
lv_obj_t* s_display_lbl;

// Grid descriptors are kept alive with static storage so the LVGL grid
// layout engine can reference them after this function returns.
constexpr int kGridFr1 = LV_GRID_FR(1);
lv_coord_t s_col_dsc[kGridCols + 1];
lv_coord_t s_row_dsc[kGridRows + 1];

// ----- pure-state helpers --------------------------------------------------

void ResetState() {
    s_acc = 0;
    s_has_acc = false;
    s_input[0] = '\0';
    s_pending_op = 0;
    s_just_eq = false;
    s_error = false;
}

double ParseInput() {
    if (s_input[0] == '\0' || (s_input[0] == '-' && s_input[1] == '\0')) {
        return 0.0;
    }
    return strtod(s_input, nullptr);
}

void FormatNumber(double v, char* out, size_t sz) {
    if (std::isnan(v) || std::isinf(v)) {
        snprintf(out, sz, "%s", "错误");
        return;
    }
    // Compact double formatting: avoid trailing zeros, max 12 sig figs.
    snprintf(out, sz, "%.12g", v);
}

double ApplyOp(double a, double b, int op) {
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return (b == 0.0) ? std::nan("") : (a / b);
        default:  return b;
    }
}

void OnDigit(char d) {
    if (s_error) {
        ResetState();
    }
    if (s_just_eq) {
        // Start a fresh expression after '='.
        ResetState();
    }
    size_t len = std::strlen(s_input);
    if (len >= sizeof(s_input) - 2) {
        return;  // hard cap on input length
    }
    if (len == 0 || (len == 1 && s_input[0] == '0')) {
        s_input[0] = d;
        s_input[1] = '\0';
    } else {
        s_input[len]     = d;
        s_input[len + 1] = '\0';
    }
}

void OnDot() {
    if (s_error || s_just_eq) {
        ResetState();
    }
    size_t len = std::strlen(s_input);
    if (len >= sizeof(s_input) - 2) {
        return;
    }
    if (len == 0) {
        std::strcpy(s_input, "0.");
        return;
    }
    if (std::strchr(s_input, '.') != nullptr) {
        return;  // already has a dot
    }
    s_input[len]     = '.';
    s_input[len + 1] = '\0';
}

void OnClear() {
    ResetState();
}

void OnBackspace() {
    if (s_error || s_just_eq) {
        ResetState();
        return;
    }
    size_t len = std::strlen(s_input);
    if (len > 0) {
        s_input[len - 1] = '\0';
    }
    if (s_input[0] == '-' && s_input[1] == '\0') {
        s_input[0] = '\0';
    }
}

void OnSign() {
    if (s_error) return;
    if (s_input[0] == '\0') {
        // Apply sign to accumulator if user hasn't typed yet.
        if (s_has_acc) {
            FormatNumber(-s_acc, s_input, sizeof(s_input));
            s_has_acc = false;
            s_pending_op = 0;
            s_just_eq = false;
        }
        return;
    }
    if (s_input[0] == '-') {
        std::memmove(s_input, s_input + 1, std::strlen(s_input));
    } else {
        size_t len = std::strlen(s_input);
        if (len >= sizeof(s_input) - 2) return;
        std::memmove(s_input + 1, s_input, len + 1);
        s_input[0] = '-';
    }
}

void OnPercent() {
    if (s_error) return;
    double v = (s_input[0] != '\0') ? ParseInput()
                                    : (s_has_acc ? s_acc : 0.0);
    v /= 100.0;
    FormatNumber(v, s_input, sizeof(s_input));
    s_just_eq = false;
}

void OnOp(int op) {
    if (s_error) return;
    if (s_input[0] != '\0') {
        if (!s_has_acc) {
            s_acc = ParseInput();
            s_has_acc = true;
        } else if (!s_just_eq) {
            double rhs = ParseInput();
            double r = ApplyOp(s_acc, rhs, s_pending_op);
            if (std::isnan(r) || std::isinf(r)) { s_error = true; return; }
            s_acc = r;
        }
    }
    s_pending_op = op;
    s_input[0] = '\0';
    s_just_eq = false;
}

void OnEquals() {
    if (s_error) return;
    if (!s_has_acc || s_pending_op == 0 || s_input[0] == '\0') {
        return;
    }
    double rhs = ParseInput();
    double r = ApplyOp(s_acc, rhs, s_pending_op);
    if (std::isnan(r) || std::isinf(r)) { s_error = true; return; }
    FormatNumber(r, s_input, sizeof(s_input));
    s_acc = 0;
    s_has_acc = false;
    s_pending_op = 0;
    s_just_eq = true;
}

// ----- UI updates ----------------------------------------------------------

void RefreshDisplay() {
    if (s_error) {
        lv_label_set_text(s_history_lbl, "");
        lv_label_set_text(s_display_lbl, "错误");
        return;
    }

    // History line: "<acc> <op>" while waiting for the right-hand side.
    if (s_has_acc && s_pending_op != 0) {
        char buf[40];
        char acc_str[24];
        FormatNumber(s_acc, acc_str, sizeof(acc_str));
        const char* op_str = "?";
        switch (s_pending_op) {
            case '+': op_str = "+"; break;
            case '-': op_str = "-"; break;
            case '*': op_str = "x"; break;
            case '/': op_str = "/"; break;
        }
        snprintf(buf, sizeof(buf), "%s %s", acc_str, op_str);
        lv_label_set_text(s_history_lbl, buf);
    } else {
        lv_label_set_text(s_history_lbl, "");
    }

    // Main display: input takes precedence; otherwise show the accumulator.
    if (s_input[0] != '\0') {
        lv_label_set_text(s_display_lbl, s_input);
    } else if (s_has_acc) {
        char acc_str[24];
        FormatNumber(s_acc, acc_str, sizeof(acc_str));
        lv_label_set_text(s_display_lbl, acc_str);
    } else {
        lv_label_set_text(s_display_lbl, "0");
    }
}

// ----- LVGL event handlers -------------------------------------------------

void BtnEventCb(lv_event_t* e) {
    const Btn* btn = static_cast<const Btn*>(lv_event_get_user_data(e));
    if (!btn) return;

    switch (btn->action) {
        case Action::kDigit:    OnDigit(btn->digit); break;
        case Action::kDot:      OnDot();             break;
        case Action::kClear:    OnClear();           break;
        case Action::kBackspace:OnBackspace();       break;
        case Action::kSign:     OnSign();            break;
        case Action::kPercent:  OnPercent();         break;
        case Action::kOpAdd:    OnOp('+');           break;
        case Action::kOpSub:    OnOp('-');           break;
        case Action::kOpMul:    OnOp('*');           break;
        case Action::kOpDiv:    OnOp('/');           break;
        case Action::kEquals:   OnEquals();          break;
    }
    RefreshDisplay();
}

void OnSwipeBack() {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

// ----- builders ------------------------------------------------------------

void BuildHeader(lv_obj_t* parent) {
    // Title -- left-aligned.
    lv_obj_t* title = lv_label_create(parent);
    lv_label_set_text(title, "计算器");
    lv_obj_set_style_text_color(title, lv_color_hex(kColorTextPrimary),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, kPad + 8, kPad + 16);
    // The label is non-interactive on its own; keep it that way so the
    // screen-level swipe-back tracker can see PRESS / RELEASE events
    // anywhere in the header strip.
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    // Right-aligned hint -- replaces the old "返回" pill button now that
    // navigation is gesture-based.
    lv_obj_t* hint = lv_label_create(parent);
    lv_label_set_text(hint, "右滑返回");
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -kPad - 4, kPad + 20);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);
}

void BuildDisplay(lv_obj_t* parent) {
    // History line above the main display.
    s_history_lbl = lv_label_create(parent);
    lv_label_set_text(s_history_lbl, "");
    lv_obj_set_style_text_color(s_history_lbl,
                                lv_color_hex(kColorTextHistory), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_history_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_history_lbl, LV_TEXT_ALIGN_RIGHT,
                                LV_PART_MAIN);
    lv_obj_set_size(s_history_lbl, kPanelSize - 2 * kPad, kHistoryH);
    lv_obj_set_pos(s_history_lbl, kPad, kHeaderH);

    // Main display -- right-aligned big number.
    s_display_lbl = lv_label_create(parent);
    lv_label_set_text(s_display_lbl, "0");
    lv_obj_set_style_text_color(s_display_lbl,
                                lv_color_hex(kColorTextPrimary), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_display_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_display_lbl, LV_TEXT_ALIGN_RIGHT,
                                LV_PART_MAIN);
    lv_label_set_long_mode(s_display_lbl, LV_LABEL_LONG_SCROLL);
    lv_obj_set_size(s_display_lbl, kPanelSize - 2 * kPad - 8, kDisplayH);
    lv_obj_set_pos(s_display_lbl, kPad + 4, kHeaderH + kHistoryH);
}

void StyleButton(lv_obj_t* btn, Btn::Style style) {
    uint32_t bg, bg_press, fg;
    switch (style) {
        case Btn::Style::kDigit:
            bg = kDigitBg; bg_press = kDigitBgPress; fg = kDigitText; break;
        case Btn::Style::kFunc:
            bg = kFuncBg;  bg_press = kFuncBgPress;  fg = kFuncText;  break;
        case Btn::Style::kOp:
        default:
            bg = kOpBg;    bg_press = kOpBgPress;    fg = kOpText;    break;
    }

    lv_obj_set_style_bg_color(btn, lv_color_hex(bg),       LV_PART_MAIN);
    lv_obj_set_style_bg_opa  (btn, LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_radius  (btn, LV_RADIUS_CIRCLE,       LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_press),
                              LV_PART_MAIN | LV_STATE_PRESSED);

    // Tag the foreground color via user-data-free style: store on first child
    // (label) when we create it -- handled in BuildKeypad.
    (void)fg;
}

void BuildKeypad(lv_obj_t* parent) {
    // 4 equal columns, 5 equal rows.
    for (int i = 0; i < kGridCols; i++) s_col_dsc[i] = kGridFr1;
    s_col_dsc[kGridCols] = LV_GRID_TEMPLATE_LAST;
    for (int i = 0; i < kGridRows; i++) s_row_dsc[i] = kGridFr1;
    s_row_dsc[kGridRows] = LV_GRID_TEMPLATE_LAST;

    lv_obj_t* grid = lv_obj_create(parent);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, kPanelSize - 2 * kPad, kGridH);
    lv_obj_set_pos(grid, kPad, kGridY);
    lv_obj_set_style_pad_all(grid, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(grid, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_column(grid, 12, LV_PART_MAIN);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_dsc_array(grid, s_col_dsc, s_row_dsc);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);

    for (const auto& def : kButtons) {
        lv_obj_t* btn = lv_button_create(grid);
        lv_obj_set_grid_cell(btn,
                             LV_GRID_ALIGN_STRETCH, def.col, def.col_span,
                             LV_GRID_ALIGN_STRETCH, def.row, def.row_span);
        StyleButton(btn, def.style);
        lv_obj_add_event_cb(btn, BtnEventCb, LV_EVENT_CLICKED,
                            const_cast<Btn*>(&def));

        uint32_t fg_color;
        switch (def.style) {
            case Btn::Style::kDigit: fg_color = kDigitText; break;
            case Btn::Style::kFunc:  fg_color = kFuncText;  break;
            case Btn::Style::kOp:
            default:                 fg_color = kOpText;    break;
        }

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, def.label);
        lv_obj_set_style_text_color(lbl, lv_color_hex(fg_color), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_center(lbl);
    }
}

}  // namespace

lv_obj_t* Calculator::Create() {
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);

    ResetState();

    BuildHeader(scr);
    BuildDisplay(scr);
    BuildKeypad(scr);
    RefreshDisplay();

    // Right-swipe back to home. The keypad buttons retain their own
    // CLICKED handlers; only PRESS / RELEASE events landing on the screen
    // background (header strip, gaps between buttons) feed the gesture
    // tracker, so taps on a key never accidentally count as a swipe.
    screen_attach_swipe_back(scr, OnSwipeBack);

    return scr;
}
