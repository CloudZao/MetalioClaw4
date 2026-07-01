#include "theme_screen.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "esp_log.h"

#include "application.h"
#include "home_screen/home_screen.h"
#include "screen_util.h"
#include "theme_manager.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "ThemeScreen";

constexpr int kPanelSize   = 720;
constexpr int kHeaderH     = 90;
constexpr int kPad         = 16;
constexpr int kBackBtnSize = 72;

// 预览卡片尺寸：与 home_screen 上的 app cell 同尺寸 + 同圆角，让用户一眼
// 看出"这就是主页磁贴的样子"。
//
// 主题数量 1~3 时勉强能 3 张横排，4 张就超 720 宽了，这里直接按 2 列布
// 局；列数变更影响行排列与内容垂直起点，下面 BuildContent 里会按 2 列推
// 出每张卡的位置。
constexpr int kCardSize     = 180;
constexpr int kCardColGap   = 60;
constexpr int kCardRowGap   = 40;
constexpr int kCardRadius   = 40;
constexpr int kCardBorder   = 3;   // 选中描边宽度；未选中用透明边框占位保持对齐
constexpr int kCardLabelH   = 36;
constexpr int kCardLabelPad = 12;
constexpr int kGridCols     = 2;

constexpr uint32_t kColorBg            = 0x000000;
constexpr uint32_t kColorText          = 0xFFFFFF;
constexpr uint32_t kColorSubtle        = 0x9CA3AF;
constexpr uint32_t kColorActive        = 0x3B82F6;  // 当前主题高亮蓝
constexpr uint32_t kColorDialogBg      = 0x1B2030;
constexpr uint32_t kColorBtnPrimaryBg  = 0x3B82F6;
constexpr uint32_t kColorBtnCancelBg   = 0x2A2F3A;

struct UiState {
    lv_obj_t* screen = nullptr;
    int current_theme = ThemeManager::kDefaultThemeId;
};
UiState s_ui;

struct DialogState {
    lv_obj_t* mask = nullptr;
    int target_theme = 0;
};
DialogState s_dlg;

inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

// ---- 前向声明 ----
void OnSwipeBack();
void OpenConfirmDialog(int theme_id);
void CloseDialog();

// ---- 卡片点击 ----
void OnCardClicked(lv_event_t* e) {
    const int target = static_cast<int>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (target < ThemeManager::kMinThemeId ||
        target > ThemeManager::kMaxThemeId) {
        return;
    }
    if (target == s_ui.current_theme) {
        ESP_LOGI(TAG, "theme%d already active, skip", target);
        return;
    }
    OpenConfirmDialog(target);
}

lv_obj_t* BuildThemeCard(lv_obj_t* parent, int theme_id, int x, int y_top) {
    const bool active = (theme_id == s_ui.current_theme);

    // 「卡片 + 文字」一对儿放在同一个 slot 里，方便 align。
    lv_obj_t* slot = lv_obj_create(parent);
    lv_obj_remove_style_all(slot);
    lv_obj_set_size(slot, kCardSize, kCardSize + kCardLabelH + kCardLabelPad);
    lv_obj_set_pos(slot, x, y_top);
    lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(slot, LV_OBJ_FLAG_SCROLLABLE);

    // ---- 卡片本体 ----
    lv_obj_t* card = lv_obj_create(slot);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardSize, kCardSize);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // 图标直接展示；仅当前主题显示蓝色描边。所有卡片统一预留描边区域，
    // 图标缩进后居中，避免选中态边框把图标挤偏。
    const int icon_size = kCardSize - kCardBorder * 2;
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(card, kCardRadius, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, kCardBorder, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(kColorActive), LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, active ? LV_OPA_COVER : LV_OPA_TRANSP,
                                LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(card, true, LV_PART_MAIN);

    // 用该主题的 "theme app" 图标本身作为预览（用户指明"展示三个主题图标"）
    char src[64];
    std::snprintf(src, sizeof(src),
                  "A:ic_app_home_theme%d_theme.spng", theme_id);
    lv_obj_t* preview = lv_image_create(card);
    lv_image_set_src(preview, src);
    lv_obj_set_size(preview, icon_size, icon_size);
    lv_obj_center(preview);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    // 把 theme_id 编码进 user_data 指针，避免单独 new 一份 int。
    lv_obj_add_event_cb(
        card, OnCardClicked, LV_EVENT_CLICKED,
        reinterpret_cast<void*>(static_cast<intptr_t>(theme_id)));

    // ---- 卡片下方标签：主题 N (当前) ----
    lv_obj_t* lbl = lv_label_create(slot);
    char buf[32];
    if (active) {
        std::snprintf(buf, sizeof(buf), "主题 %d  当前", theme_id);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorActive),
                                    LV_PART_MAIN);
    } else {
        std::snprintf(buf, sizeof(buf), "主题 %d", theme_id);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
    }
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    return slot;
}

// ---- header（与 sd_card_screen 同款返回按钮 + 居中标题） ----
void BuildHeader(lv_obj_t* parent) {
    lv_obj_t* back_btn = lv_button_create(parent);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, kBackBtnSize, kBackBtnSize);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_20,
                            Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, kPad + 8, kPad + 8);
    screen_swipe_back_ignore(back_btn, true);

    lv_obj_t* back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);
    lv_obj_add_event_cb(
        back_btn, [](lv_event_t*) { OnSwipeBack(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* title = lv_label_create(parent);
    lv_label_set_text(title, "主题");
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, kPad + 20);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);
}

// ---- 主体内容：2x2 网格预览卡片 + 底部提示 ----
//
// 卡片单元高度 = 卡片本体 + 文字标签 + 标签内边距；行间用 kCardRowGap 隔。
// 整个网格按 (主题数, 列数) 自动算行数，再水平/垂直居中放进 header 下方
// 至屏幕底部之间的可视区域；卡片数量在 1~kGridCols 之间也能自然适配
// （只占一行，水平居中）。
void BuildContent(lv_obj_t* parent) {
    constexpr int kCellH = kCardSize + kCardLabelH + kCardLabelPad;
    constexpr int kHintReservedH = 60;  // 底部提示占用的高度（含余量）

    const int cols = std::min(kGridCols, ThemeManager::kThemeCount);
    const int rows = (ThemeManager::kThemeCount + cols - 1) / cols;

    const int grid_w = cols * kCardSize + (cols - 1) * kCardColGap;
    const int grid_h = rows * kCellH + (rows - 1) * kCardRowGap;
    static_assert(2 * kCardSize + kCardColGap <= kPanelSize,
                  "theme card row exceeds panel width");

    const int avail_top = kHeaderH;
    const int avail_bottom = kPanelSize - kHintReservedH;
    const int avail_h = avail_bottom - avail_top;

    const int origin_x = (kPanelSize - grid_w) / 2;
    int origin_y = avail_top + (avail_h - grid_h) / 2;
    if (origin_y < avail_top + 16) {
        origin_y = avail_top + 16;
    }

    for (int i = 0; i < ThemeManager::kThemeCount; ++i) {
        const int theme_id = ThemeManager::kMinThemeId + i;
        const int col = i % cols;
        const int row = i / cols;
        const int x = origin_x + col * (kCardSize + kCardColGap);
        const int y = origin_y + row * (kCellH + kCardRowGap);
        BuildThemeCard(parent, theme_id, x, y);
    }

    lv_obj_t* hint = lv_label_create(parent);
    lv_label_set_text(hint, "切换主题后将自动重启设备以应用新主题");
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);
}

// ---- 二次确认对话框 ----
void OnDialogMaskClicked(lv_event_t* e) {
    // 仅"点中 mask 自身"才关闭；card / 按钮上的点击因 EVENT_BUBBLE 默认
    // 关闭根本不会冒泡到这里，做一道 target 校验属于双保险。
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) {
        return;
    }
    CloseDialog();
}

void CloseDialog() {
    if (s_dlg.mask != nullptr) {
        lv_obj_delete(s_dlg.mask);
    }
    s_dlg = DialogState{};
}

void OnCancelClicked(lv_event_t* /*e*/) { CloseDialog(); }

void OnConfirmClicked(lv_event_t* /*e*/) {
    const int target = s_dlg.target_theme;
    // 把状态先清掉再 Reboot —— 即便 Reboot 在某些场景延迟生效，
    // dialog 也不会留个无主的指针在 s_dlg 里。
    s_dlg = DialogState{};
    ESP_LOGW(TAG, "switch theme -> theme%d, rebooting", target);
    ThemeManager::SetCurrentThemeId(target);
    Application::GetInstance().Reboot();
}

void OpenConfirmDialog(int theme_id) {
    if (s_dlg.mask != nullptr || s_ui.screen == nullptr) {
        return;
    }
    s_dlg.target_theme = theme_id;

    constexpr int kCardW = 480;
    constexpr int kCardH = 280;
    constexpr int kBtnW  = 200;
    constexpr int kBtnH  = 80;

    // 全屏遮罩 —— FLOATING 让它脱离父屏布局（即便父屏后续接上 flex）
    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelSize, kPanelSize);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    lv_obj_add_event_cb(mask, OnDialogMaskClicked, LV_EVENT_CLICKED, nullptr);
    s_dlg.mask = mask;

    // 中心卡片
    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorDialogBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // card 必须 clickable —— 否则点中 card 的事件会冒泡到 mask 触发关闭
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    char title_buf[48];
    std::snprintf(title_buf, sizeof(title_buf), "切换到主题 %d ?", theme_id);
    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, title_buf);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* desc = lv_label_create(card);
    lv_label_set_text(desc, "确定后设备将自动重启");
    lv_obj_set_style_text_color(desc, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(desc, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, -10);
    lv_obj_remove_flag(desc, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* cancel = lv_button_create(card);
    lv_obj_remove_style_all(cancel);
    lv_obj_set_size(cancel, kBtnW, kBtnH);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(kColorBtnCancelBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(cancel, 16, LV_PART_MAIN);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(cancel, OnCancelClicked, LV_EVENT_CLICKED, nullptr);
    {
        lv_obj_t* lbl = lv_label_create(cancel);
        lv_label_set_text(lbl, "取消");
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_center(lbl);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t* ok = lv_button_create(card);
    lv_obj_remove_style_all(ok);
    lv_obj_set_size(ok, kBtnW, kBtnH);
    lv_obj_set_style_bg_color(ok, lv_color_hex(kColorBtnPrimaryBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ok, 16, LV_PART_MAIN);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(ok, OnConfirmClicked, LV_EVENT_CLICKED, nullptr);
    {
        lv_obj_t* lbl = lv_label_create(ok);
        lv_label_set_text(lbl, "切换并重启");
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_center(lbl);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    }
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    s_ui = UiState{};
    s_dlg = DialogState{};
}

void OnSwipeBack() {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

}  // namespace

lv_obj_t* ThemeScreen::Create() {
    ESP_LOGI(TAG, "create theme screen");

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    s_ui.current_theme = ThemeManager::GetCurrentThemeId();

    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);
    BuildContent(scr);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    return scr;
}

void ThemeScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load");
    } else {
        ESP_LOGI(TAG, "unload");
    }
}
