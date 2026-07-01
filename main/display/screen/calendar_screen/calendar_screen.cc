#include "calendar_screen.h"

#include "home_screen/home_screen.h"
#include "screen_util.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

// ---------------------------------------------------------------------------
// 720x720 layout (square panel; ported from a 640x480 horizontal source).
//
//   panel ........ 720
//   card margin .. 28 (left/right), 28 (top), 24 (bottom)
//                  -> content area = 664 wide x 668 tall
//
//   header_top ......... 22  (relative to content top)
//   header_row_height .. 84
//   divider_offset_y ... 16
//   weekday_margin_top . 18
//   weekday_row_height . 36
//   grid_margin_top .... 12
//   cell_height ........ 72
//   row_gap ............ 4
//   grid total = 6*72 + 5*4 = 452
//   grid bottom = 22 + 84 + 16 + 18 + 36 + 12 + 452 = 640 (within 668)
// ---------------------------------------------------------------------------
constexpr int32_t kScreenWidth      = 720;

constexpr int32_t kCardMarginX      = 28;
constexpr int32_t kCardMarginTop    = 28;
constexpr int32_t kCardMarginBottom = 24;

constexpr int32_t kHeaderTop        = 22;
constexpr int32_t kHeaderRowHeight  = 84;
constexpr int32_t kHeaderInnerPadX  = 22;

constexpr int32_t kColumns          = 7;
constexpr int32_t kRows             = 6;
constexpr int32_t kGridLeftPad      = 36;
constexpr int32_t kGridRightPad     = 36;
constexpr int32_t kCellHeight       = 72;
constexpr int32_t kRowGap           = 4;
constexpr int32_t kWeekdayRowHeight = 36;
constexpr int32_t kWeekdayMarginTop = 18;
constexpr int32_t kGridMarginTop    = 12;
constexpr int32_t kDividerOffsetY   = 16;

// ---- color palette (深色 + 黄色高亮) -------------------------------------
constexpr uint32_t kColorBg            = 0x0E1116;
constexpr uint32_t kColorBgGrad        = 0x161A22;
constexpr uint32_t kColorDivider       = 0x2A2F3A;
constexpr uint32_t kColorHeaderText    = 0xFFFFFF;
constexpr uint32_t kColorSubtleText    = 0x9A9DA3;
constexpr uint32_t kColorWeekdayText   = 0xB7BAC2;
constexpr uint32_t kColorDateText      = 0xEDEEF0;
constexpr uint32_t kColorWeekendText   = 0xFF6B6B;
constexpr uint32_t kColorOtherMonth    = 0x4A4D55;
constexpr uint32_t kColorAccent        = 0xE0FB3C;
constexpr uint32_t kColorTodayText     = 0x10131A;
constexpr uint32_t kColorAccentDot     = 0xFF4A4A;
constexpr uint32_t kColorBadgeBg       = 0x202632;

const char* const kWeekdayNames[7] = {"日", "一", "二", "三", "四", "五", "六"};

// ---- date math ------------------------------------------------------------

int days_in_month(int year, int month_1based) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month_1based < 1 || month_1based > 12) {
        return 0;
    }
    int d = kDays[month_1based - 1];
    if (month_1based == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) ||
                          (year % 400 == 0);
        if (leap) d = 29;
    }
    return d;
}

int first_weekday_of_month(int year, int month_1based) {
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = month_1based - 1;
    t.tm_mday = 1;
    t.tm_isdst = -1;
    mktime(&t);
    return t.tm_wday;  // 0 = Sunday
}

// ---- helpers --------------------------------------------------------------

void OnSwipeBack() {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

// ---- builders -------------------------------------------------------------

void BuildAccentBar(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 6, 40);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, kHeaderInnerPadX, 0);
    screen_strip_obj_chrome(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
}

void BuildTodayBadge(lv_obj_t* parent, int month, int mday, int wday) {
    constexpr int32_t kBadgeW = 240;
    constexpr int32_t kBadgeH = 48;

    lv_obj_t* badge = lv_obj_create(parent);
    lv_obj_set_size(badge, kBadgeW, kBadgeH);
    lv_obj_align(badge, LV_ALIGN_RIGHT_MID, -kHeaderInnerPadX, 0);
    screen_strip_obj_chrome(badge);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(badge, lv_color_hex(kColorBadgeBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(badge, kBadgeH / 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(badge, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(badge, lv_color_hex(kColorDivider),
                                  LV_PART_MAIN);

    // Yellow dot.
    lv_obj_t* dot = lv_obj_create(badge);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 16, 0);
    screen_strip_obj_chrome(dot);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(dot, lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    char buf[48];
    snprintf(buf, sizeof(buf), "今天 %d/%d 周%s", month, mday,
             kWeekdayNames[wday]);
    lv_obj_t* lbl = lv_label_create(badge);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorAccent),
                                LV_PART_MAIN);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl, kBadgeW - 50);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 36, 0);
}

void BuildHeaderRow(lv_obj_t* content, int year, int month, int mday,
                    int wday) {
    lv_obj_t* header = lv_obj_create(content);
    lv_obj_set_size(header, LV_PCT(100), kHeaderRowHeight);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, kHeaderTop);
    screen_strip_obj_chrome(header);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);

    BuildAccentBar(header);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d 年 %d 月", year, month);
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, buf);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorHeaderText),
                                LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, kHeaderInnerPadX + 22, 0);

    BuildTodayBadge(header, month, mday, wday);

    // Divider line below the header.
    lv_obj_t* divider = lv_obj_create(content);
    lv_obj_set_size(divider, LV_PCT(94), 1);
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0,
                 kHeaderTop + kHeaderRowHeight + kDividerOffsetY);
    screen_strip_obj_chrome(divider);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorDivider),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
}

void BuildWeekdayRow(lv_obj_t* content, int32_t inner_w) {
    const int32_t row_y = kHeaderTop + kHeaderRowHeight + kDividerOffsetY +
                          kWeekdayMarginTop;
    const int32_t grid_w = inner_w - kGridLeftPad - kGridRightPad;
    const int32_t cell_w = grid_w / kColumns;

    for (int i = 0; i < kColumns; ++i) {
        const bool is_weekend = (i == 0 || i == 6);
        lv_obj_t* lbl = lv_label_create(content);
        lv_label_set_text(lbl, kWeekdayNames[i]);
        lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(
            lbl,
            lv_color_hex(is_weekend ? kColorWeekendText : kColorWeekdayText),
            LV_PART_MAIN);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_width(lbl, cell_w);
        lv_obj_set_pos(lbl, kGridLeftPad + i * cell_w, row_y);
    }
}

void BuildDayCell(lv_obj_t* content, int row, int col, int day_num,
                  bool is_today, bool is_other_month, bool is_weekend,
                  int32_t cell_w, int32_t grid_top) {
    const int32_t x = kGridLeftPad + col * cell_w;
    const int32_t y = grid_top + row * (kCellHeight + kRowGap);

    lv_obj_t* cell = lv_obj_create(content);
    lv_obj_set_size(cell, cell_w, kCellHeight);
    lv_obj_set_pos(cell, x, y);
    screen_strip_obj_chrome(cell);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);

    if (is_today) {
        lv_obj_t* hl = lv_obj_create(cell);
        const int32_t d = std::min<int32_t>(cell_w - 18, kCellHeight - 8);
        lv_obj_set_size(hl, d, d);
        lv_obj_align(hl, LV_ALIGN_CENTER, 0, 0);
        screen_strip_obj_chrome(hl);
        lv_obj_remove_flag(hl, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(hl, lv_color_hex(kColorAccent),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(hl, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(hl, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(hl, 18, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(hl, lv_color_hex(kColorAccent),
                                      LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(hl, LV_OPA_30, LV_PART_MAIN);
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", day_num);
    lv_obj_t* lbl = lv_label_create(cell);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);

    uint32_t color = kColorDateText;
    if (is_today)            color = kColorTodayText;
    else if (is_other_month) color = kColorOtherMonth;
    else if (is_weekend)     color = kColorWeekendText;
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(lbl, cell_w);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

    if (is_today) {
        lv_obj_t* mk = lv_obj_create(cell);
        lv_obj_set_size(mk, 5, 5);
        lv_obj_align(mk, LV_ALIGN_BOTTOM_MID, 0, -6);
        screen_strip_obj_chrome(mk);
        lv_obj_remove_flag(mk, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(mk, lv_color_hex(kColorAccentDot),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(mk, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(mk, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    }
}

void BuildCalendarGrid(lv_obj_t* content, int year, int month, int today_mday,
                       int32_t inner_w) {
    const int32_t grid_w = inner_w - kGridLeftPad - kGridRightPad;
    const int32_t cell_w = grid_w / kColumns;
    const int32_t grid_top = kHeaderTop + kHeaderRowHeight + kDividerOffsetY +
                             kWeekdayMarginTop + kWeekdayRowHeight +
                             kGridMarginTop;

    const int first_wday = first_weekday_of_month(year, month);
    const int days = days_in_month(year, month);

    int prev_year = year;
    int prev_month = month - 1;
    if (prev_month < 1) {
        prev_month = 12;
        --prev_year;
    }
    const int prev_days = days_in_month(prev_year, prev_month);

    int next_day = 1;
    for (int idx = 0; idx < kRows * kColumns; ++idx) {
        const int row = idx / kColumns;
        const int col = idx % kColumns;
        const bool is_weekend = (col == 0 || col == 6);

        int day_num = 0;
        bool is_other_month = false;
        bool is_today = false;

        if (idx < first_wday) {
            day_num = prev_days - (first_wday - idx - 1);
            is_other_month = true;
        } else if (idx < first_wday + days) {
            day_num = idx - first_wday + 1;
            is_today = (day_num == today_mday);
        } else {
            day_num = next_day++;
            is_other_month = true;
        }

        BuildDayCell(content, row, col, day_num, is_today, is_other_month,
                     is_weekend, cell_w, grid_top);
    }
}

}  // namespace

lv_obj_t* CalendarScreen::Create() {
    lv_obj_t* scr = lv_obj_create(NULL);
    screen_strip_obj_chrome(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Vertical gradient background.
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(kColorBgGrad),
                                   LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    // Transparent content container that adds outer padding around the
    // header / grid. All other widgets are positioned relative to it.
    lv_obj_t* content = lv_obj_create(scr);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(content, 0, LV_PART_MAIN);
    lv_obj_set_style_margin_all(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(content, kCardMarginX, LV_PART_MAIN);
    lv_obj_set_style_pad_right(content, kCardMarginX, LV_PART_MAIN);
    lv_obj_set_style_pad_top(content, kCardMarginTop, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(content, kCardMarginBottom, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

    time_t now = time(nullptr);
    struct tm tm_now = {};
    localtime_r(&now, &tm_now);
    const int year  = tm_now.tm_year + 1900;
    const int month = tm_now.tm_mon + 1;
    const int mday  = tm_now.tm_mday;
    const int wday  = tm_now.tm_wday;

    const int32_t inner_w = kScreenWidth - kCardMarginX * 2;
    BuildHeaderRow(content, year, month, mday, wday);
    BuildWeekdayRow(content, inner_w);
    BuildCalendarGrid(content, year, month, mday, inner_w);

    // "右滑返回" hint along the bottom.
    lv_obj_t* hint = lv_label_create(content);
    lv_label_set_text(hint, "右滑返回");
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtleText),
                                LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    // Make the entire content tree input-passive so PRESSED/RELEASED events
    // bubble up to the screen-level swipe-back handler.
    screen_make_input_passive(content);
    screen_attach_swipe_back(scr, OnSwipeBack);

    return scr;
}
