#include "chat_screen.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "device_state.h"
#include "home_screen/home_screen.h"
#include "screen_util.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "ChatScreen";

// ---------------------------------------------------------------------------
// 720x720 视觉参数
//
//   ┌───────────────────────────────────────────┐ 0
//   │ Header   "聊天 待唤醒" [音量- 70% 音量+][清空] │ 88
//   ├───────────────────────────────────────────┤
//   │                                           │
//   │  ┌────────┐                                │
//   │  │ 左气泡 │                                │
//   │  └────────┘                                │
//   │                                ┌─────────┐ │
//   │                                │ 右气泡  │ │
//   │                                └─────────┘ │
//   │                                           │
//   ├───────────────────────────────────────────┤ 720
//
//   - 整页暗黑主题（对齐 openclaw_screen / network_screen）
//   - header 高 88px，标题旁显示聊天状态（待唤醒 / 聆听中 / 讲话中）
//   - msg list 占满剩余 632px，列表里的 row = 100% 宽 + 内容高，气泡用绝
//     对对齐放到行内（不依赖 flex 的水平对齐，避免长文本 wrap 时气泡被
//     拉到全宽）
// ---------------------------------------------------------------------------
constexpr int32_t kPanelW          = 720;
constexpr int32_t kPanelH          = 720;
constexpr int32_t kHeaderH         = 88;
constexpr int32_t kBackBtnSize       = 72;
constexpr int32_t kListPadH        = 18;
constexpr int32_t kListPadTop      = 14;
constexpr int32_t kListPadBottom   = 32;
constexpr int32_t kRowGap          = 12;
constexpr int32_t kBubblePadX      = 18;
constexpr int32_t kBubblePadY      = 14;
constexpr int32_t kBubbleRadius    = 18;
constexpr int32_t kSideMargin      = 8;
constexpr int32_t kMaxMessages     = 12;
constexpr int32_t kToggleBtnSize   = 80;
constexpr int32_t kToggleIconSize  = 64;
constexpr int32_t kToggleBtnMargin = 40;
constexpr int32_t kClearBtnW       = 130;
constexpr int32_t kClearBtnH       = 56;
constexpr int32_t kHeaderRightPad  = 20;
constexpr int32_t kHeaderCtrlGap   = 12;
constexpr int32_t kVolumeWrapW     = 220;
constexpr int32_t kVolumeWrapH     = 56;
constexpr int32_t kVolBtnW         = 72;
constexpr int32_t kVolBtnH         = 44;
constexpr int32_t kVolumeValueW    = 52;
constexpr int32_t kVolumeStep      = 5;

constexpr uint32_t kColorBg              = 0x0E1116;
constexpr uint32_t kColorHeaderBg        = 0x12151C;
constexpr uint32_t kColorDivider         = 0x2A2F3A;
constexpr uint32_t kColorHeaderText      = 0xFFFFFF;
constexpr uint32_t kColorHeaderBtn       = 0x2A2F3A;
constexpr uint32_t kColorHeaderBtnBorder = 0x3B4556;
constexpr uint32_t kColorHeaderBtnText   = 0xE5E7EB;
constexpr uint32_t kColorLeftBubble      = 0x202736;
constexpr uint32_t kColorRightBubble     = 0x1E3A2F;
constexpr uint32_t kColorRightBubbleText = 0xE8F5E9;
constexpr uint32_t kColorLeftBubbleText  = 0xE5E7EB;
constexpr uint32_t kColorHintText        = 0x9AA3B2;
constexpr uint32_t kColorStateIdle       = 0x9AA3B2;
constexpr uint32_t kColorStateListening  = 0x34D399;
constexpr uint32_t kColorStateSpeaking   = 0x60A5FA;
constexpr uint32_t kColorStateConnecting = 0xFBBF24;
constexpr uint32_t kColorToggleBtnBg     = 0xFFFFFF;
constexpr uint32_t kColorToggleBtnPress  = 0xF0F0F0;

constexpr const char kEmptyHint[] = "快来和我聊天吧\n用 \"Hi 钛灵\" 唤醒我";

lv_obj_t* s_screen          = nullptr;
lv_obj_t* s_msg_list        = nullptr;
lv_obj_t* s_empty_hint      = nullptr;
lv_obj_t* s_status_state_lbl = nullptr;
lv_obj_t* s_volume_value_lbl = nullptr;
lv_timer_t* s_state_timer   = nullptr;
lv_timer_t* s_activation_guard_timer = nullptr;
DeviceState s_last_device_state = kDeviceStateUnknown;

// 未激活拦截：全屏模态弹窗，不可关闭，仅能通过返回键离开。
struct ActivationBlockedDialogUi {
    lv_obj_t* mask = nullptr;
};
ActivationBlockedDialogUi s_activation_dlg;
bool s_activation_blocked = false;

const lv_font_t* chat_font() { return &font_puhui_30_4; }

// ---------------------------------------------------------------------------
// Header 标题旁：设备聊天状态
// ---------------------------------------------------------------------------
bool chat_status_for_state(DeviceState state, const char** text, uint32_t* color) {
    switch (state) {
        case kDeviceStateIdle:
            *text = "待唤醒";
            *color = kColorStateIdle;
            return true;
        case kDeviceStateListening:
            *text = "聆听中";
            *color = kColorStateListening;
            return true;
        case kDeviceStateSpeaking:
            *text = "讲话中";
            *color = kColorStateSpeaking;
            return true;
        case kDeviceStateConnecting:
            *text = "连接中";
            *color = kColorStateConnecting;
            return true;
        default:
            return false;
    }
}

void chat_update_device_state_label() {
    if (s_status_state_lbl == nullptr) {
        return;
    }

    const DeviceState state = Application::GetInstance().GetDeviceState();
    if (state == s_last_device_state) {
        return;
    }
    s_last_device_state = state;

    const char* text = nullptr;
    uint32_t color = kColorStateIdle;
    if (!chat_status_for_state(state, &text, &color)) {
        lv_obj_add_flag(s_status_state_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(s_status_state_lbl, text);
    lv_obj_set_style_text_color(s_status_state_lbl, lv_color_hex(color),
                                LV_PART_MAIN);
    lv_obj_remove_flag(s_status_state_lbl, LV_OBJ_FLAG_HIDDEN);
}

void on_chat_status_timer(lv_timer_t* /*timer*/) {
    chat_update_device_state_label();
}

void on_refresh_device_state_async(void* /*user_data*/) {
    chat_update_device_state_label();
}

// ---------------------------------------------------------------------------
// 列表辅助
// ---------------------------------------------------------------------------
void chat_update_empty_hint() {
    if (s_empty_hint == nullptr || s_msg_list == nullptr) return;
    if (lv_obj_get_child_count(s_msg_list) == 0) {
        lv_obj_remove_flag(s_empty_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

void chat_scroll_to_latest(bool anim) {
    if (s_msg_list == nullptr) return;
    const uint32_t count = lv_obj_get_child_count(s_msg_list);
    if (count == 0) return;
    lv_obj_t* latest = lv_obj_get_child(s_msg_list, count - 1);
    if (latest != nullptr) {
        lv_obj_scroll_to_view_recursive(latest, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    }
}

void chat_trim_old_msgs() {
    if (s_msg_list == nullptr) return;
    while (static_cast<int32_t>(lv_obj_get_child_count(s_msg_list)) > kMaxMessages) {
        lv_obj_t* oldest = lv_obj_get_child(s_msg_list, 0);
        if (oldest == nullptr) break;
        lv_obj_delete(oldest);
    }
}

// ---------------------------------------------------------------------------
// 气泡构造
//
// 一条消息 = 一行容器 + 一个气泡。气泡宽度按 “文本宽 + pad，封顶 72% 屏宽”
// 计算；超过封顶时给 label 设固定宽度并启用 LV_LABEL_LONG_WRAP 让 LVGL
// 帮我们换行。
// ---------------------------------------------------------------------------
lv_obj_t* chat_create_bubble_row(const char* text, ChatMsgDir dir) {
    const bool is_right = (dir == ChatMsgDir::Right);
    const lv_font_t* font = chat_font();

    lv_obj_t* row = lv_obj_create(s_msg_list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, kRowGap, LV_PART_MAIN);

    lv_obj_t* bubble = lv_obj_create(row);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, kBubbleRadius, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bubble, kBubblePadX, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bubble, kBubblePadY, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bubble, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    // 单行文本宽度：超过 72% 屏宽就吃满并由 LVGL 自动换行。
    const int32_t max_bubble_w = kPanelW * 72 / 100;
    int32_t text_w = lv_txt_get_width(text, std::strlen(text), font, 0);
    if (text_w < 24) text_w = 24;
    int32_t bubble_w = text_w + kBubblePadX * 2;
    if (bubble_w > max_bubble_w) bubble_w = max_bubble_w;
    lv_obj_set_width(bubble, bubble_w);

    if (is_right) {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(kColorRightBubble), LV_PART_MAIN);
        lv_obj_align(bubble, LV_ALIGN_TOP_RIGHT, -kSideMargin, 0);
    } else {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(kColorLeftBubble), LV_PART_MAIN);
        lv_obj_align(bubble, LV_ALIGN_TOP_LEFT, kSideMargin, 0);
    }
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* label = lv_label_create(bubble);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, bubble_w - kBubblePadX * 2);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(is_right ? kColorRightBubbleText
                                                           : kColorLeftBubbleText),
                                LV_PART_MAIN);
    lv_obj_update_layout(label);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);

    // 整个 row 都不消费输入：让右滑返回手势能从气泡上滑出去。气泡本身
    // 不需要点击，screen 级 swipe-back 会处理。
    screen_make_input_passive(row);
    return row;
}

// ---------------------------------------------------------------------------
// 设备激活检查
// ---------------------------------------------------------------------------
bool is_device_activated() {
    auto& app = Application::GetInstance();
    if (app.HasPendingActivation()) {
        return false;
    }
    if (app.GetDeviceState() == kDeviceStateActivating) {
        return false;
    }
    return true;
}

void log_activation_blocked() {
    auto& app = Application::GetInstance();
    ESP_LOGW(TAG, "Chat blocked: device not activated");
    if (app.HasPendingActivation()) {
        ESP_LOGW(TAG, "pending activation code: %s",
                 app.GetPendingActivationCode().c_str());
    }
    if (app.GetDeviceState() == kDeviceStateActivating) {
        ESP_LOGW(TAG, "device state: activating");
    }
}

void on_swipe_back();

void open_activation_blocked_dialog() {
    if (s_screen == nullptr || s_activation_dlg.mask != nullptr) {
        return;
    }

    auto& app = Application::GetInstance();
    const bool has_code = app.HasPendingActivation();

    constexpr int32_t kCardW = 520;
    const int32_t kCardH = has_code ? 420 : 340;
    constexpr int32_t kBackBtnW = 200;
    constexpr int32_t kBackBtnH = 72;

    lv_obj_t* mask = lv_obj_create(s_screen);
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(mask, true);
    s_activation_dlg.mask = mask;

    lv_obj_t* card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 28, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "设备未激活");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* desc = lv_label_create(card);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, kCardW - 56);
    lv_label_set_text(desc, "请先完成设备激活后再使用聊天。");
    lv_obj_set_style_text_color(desc, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(desc, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, has_code ? -30 : -10);
    lv_obj_remove_flag(desc, LV_OBJ_FLAG_CLICKABLE);

    if (has_code) {
        char code_buf[64];
        std::snprintf(code_buf, sizeof(code_buf), "验证码: %s",
                      app.GetPendingActivationCode().c_str());
        lv_obj_t* code_lbl = lv_label_create(card);
        lv_label_set_text(code_lbl, code_buf);
        lv_obj_set_style_text_color(code_lbl, lv_color_hex(0xFBBF24),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(code_lbl, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_align(code_lbl, LV_ALIGN_BOTTOM_MID, 0, -(kBackBtnH + 24));
        lv_obj_remove_flag(code_lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t* back = lv_button_create(card);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnW, kBackBtnH);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 16, LV_PART_MAIN);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(back,
                        [](lv_event_t* /*e*/) { on_swipe_back(); },
                        LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, "返回");
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_text_font(back_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(back_lbl);
    lv_obj_remove_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);
}

void ensure_activation_blocked_dialog() {
    if (!s_activation_blocked) {
        return;
    }
    if (s_activation_dlg.mask == nullptr) {
        open_activation_blocked_dialog();
    }
}

void on_activation_guard_timer(lv_timer_t* /*timer*/) {
    ensure_activation_blocked_dialog();
}

// ---------------------------------------------------------------------------
// Header 音量按钮
// ---------------------------------------------------------------------------
int chat_read_volume() {
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

void chat_update_volume_value_label(int volume) {
    if (s_volume_value_lbl == nullptr) {
        return;
    }
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d%%", volume);
    lv_label_set_text(s_volume_value_lbl, buf);
}

void chat_apply_volume(int volume) {
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    chat_update_volume_value_label(volume);

    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr || codec->output_volume() == volume) {
        return;
    }
    codec->SetOutputVolume(volume);
}

void on_volume_down_clicked(lv_event_t* /*e*/) {
    chat_apply_volume(chat_read_volume() - kVolumeStep);
}

void on_volume_up_clicked(lv_event_t* /*e*/) {
    chat_apply_volume(chat_read_volume() + kVolumeStep);
}

// ---------------------------------------------------------------------------
// "清空" 按钮 / 右下角对话切换
// ---------------------------------------------------------------------------
void on_clear_clicked(lv_event_t* /*e*/) {
    if (s_activation_blocked) {
        ensure_activation_blocked_dialog();
        return;
    }
    ChatScreen::ClearMessages();
}

void on_toggle_clicked(lv_event_t* /*e*/) {
    if (s_activation_blocked) {
        ESP_LOGW(TAG, "toggle ignored: device not activated");
        ensure_activation_blocked_dialog();
        return;
    }
    Application::GetInstance().ToggleChatState();
    ChatScreen::RefreshDeviceState();
}

void build_toggle_button(lv_obj_t* parent) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(btn, kToggleBtnSize, kToggleBtnSize);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, -kToggleBtnMargin, -kToggleBtnMargin);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorToggleBtnBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorToggleBtnPress),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 16, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_x(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, (kToggleBtnSize - kToggleIconSize) / 2,
                             LV_PART_MAIN);

    lv_obj_t* icon = lv_image_create(btn);
    lv_image_set_src(icon, "A:ic_app_chat_toggle.spng");
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_size(icon, kToggleIconSize, kToggleIconSize);
    lv_obj_center(icon);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(btn, on_toggle_clicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(btn, true);
}

// ---------------------------------------------------------------------------
// 屏幕导航
// ---------------------------------------------------------------------------
void on_swipe_back() {
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) lv_indev_wait_release(indev);
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home    = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void on_screen_unloaded(lv_event_t* /*e*/) {
    // 屏幕对象将由 lv_obj_delete_async 释放，清掉所有静态引用。后台不再
    // 持有任何消息。
    if (s_state_timer != nullptr) {
        lv_timer_delete(s_state_timer);
        s_state_timer = nullptr;
    }
    if (s_activation_guard_timer != nullptr) {
        lv_timer_delete(s_activation_guard_timer);
        s_activation_guard_timer = nullptr;
    }
    s_activation_dlg = ActivationBlockedDialogUi{};
    s_activation_blocked = false;
    s_screen           = nullptr;
    s_msg_list         = nullptr;
    s_empty_hint       = nullptr;
    s_status_state_lbl = nullptr;
    s_volume_value_lbl = nullptr;
    s_last_device_state = kDeviceStateUnknown;
}

// ---------------------------------------------------------------------------
// UI 组装
// ---------------------------------------------------------------------------
void style_header_btn(lv_obj_t* btn) {
    lv_obj_set_style_radius(btn, 28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorHeaderBtn), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(kColorHeaderBtnBorder),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3B4556),
                              LV_PART_MAIN | LV_STATE_PRESSED);
}

void style_header_icon_btn(lv_obj_t* btn) {
    style_header_btn(btn);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
}

lv_obj_t* create_volume_btn(lv_obj_t* parent, const char* text,
                            lv_event_cb_t cb) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, kVolBtnW, kVolBtnH);
    style_header_icon_btn(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(btn, true);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorHeaderBtnText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

void build_header(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(kColorHeaderBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // 1px 下分割线，靠 bg 占位即可，避免再开 line 控件。
    lv_obj_t* divider = lv_obj_create(header);
    screen_strip_obj_chrome(divider);
    lv_obj_set_size(divider, kPanelW, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorDivider), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    screen_make_input_passive(divider);

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
    lv_obj_add_event_cb(back,
                        [](lv_event_t* /*e*/) { on_swipe_back(); },
                        LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "聊天");
    lv_obj_set_style_text_color(title, lv_color_hex(kColorHeaderText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 12, 0);

    s_status_state_lbl = lv_label_create(header);
    lv_label_set_text(s_status_state_lbl, "待唤醒");
    lv_obj_set_style_text_font(s_status_state_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status_state_lbl, lv_color_hex(kColorStateIdle),
                                LV_PART_MAIN);
    lv_obj_align_to(s_status_state_lbl, title, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    screen_make_input_passive(s_status_state_lbl);

    s_last_device_state = kDeviceStateUnknown;
    chat_update_device_state_label();
    s_state_timer = lv_timer_create(on_chat_status_timer, 500, nullptr);

    lv_obj_t* clear = lv_button_create(header);
    lv_obj_set_size(clear, kClearBtnW, kClearBtnH);
    lv_obj_align(clear, LV_ALIGN_RIGHT_MID, -kHeaderRightPad, 0);
    style_header_btn(clear);
    lv_obj_add_event_cb(clear, on_clear_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* clear_lbl = lv_label_create(clear);
    lv_label_set_text(clear_lbl, "清空");
    lv_obj_set_style_text_color(clear_lbl, lv_color_hex(kColorHeaderBtnText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(clear_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(clear_lbl);

    const int initial_volume = chat_read_volume();

    lv_obj_t* vol_wrap = lv_obj_create(header);
    lv_obj_remove_style_all(vol_wrap);
    lv_obj_set_size(vol_wrap, kVolumeWrapW, kVolumeWrapH);
    lv_obj_align_to(vol_wrap, clear, LV_ALIGN_OUT_LEFT_MID, -kHeaderCtrlGap, 0);
    lv_obj_set_style_bg_opa(vol_wrap, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(vol_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(vol_wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol_wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(vol_wrap, 8, LV_PART_MAIN);
    screen_swipe_back_ignore(vol_wrap, true);

    create_volume_btn(vol_wrap, "音量-", on_volume_down_clicked);

    s_volume_value_lbl = lv_label_create(vol_wrap);
    lv_obj_set_width(s_volume_value_lbl, kVolumeValueW);
    lv_label_set_long_mode(s_volume_value_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_volume_value_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_volume_value_lbl, lv_color_hex(kColorHeaderBtnText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_volume_value_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    chat_update_volume_value_label(initial_volume);
    screen_make_input_passive(s_volume_value_lbl);

    create_volume_btn(vol_wrap, "音量+", on_volume_up_clicked);
}

void build_message_list(lv_obj_t* parent) {
    s_msg_list = lv_obj_create(parent);
    lv_obj_set_size(s_msg_list, kPanelW, kPanelH - kHeaderH);
    lv_obj_set_pos(s_msg_list, 0, kHeaderH);
    screen_strip_obj_chrome(s_msg_list);
    lv_obj_set_style_bg_opa(s_msg_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_msg_list, kListPadH, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_msg_list, kListPadH, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_msg_list, kListPadTop, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_msg_list, kListPadBottom, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_msg_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_msg_list, LV_DIR_VER);

    lv_obj_set_flex_flow(s_msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_msg_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    // 列表是垂直滚动控件，水平拖动 -> 右滑返回；不需要豁免。但是
    // screen_make_input_passive 不能用在列表自身上（否则滚不动）。

    s_empty_hint = lv_label_create(parent);
    lv_label_set_text(s_empty_hint, kEmptyHint);
    lv_obj_set_width(s_empty_hint, kPanelW * 80 / 100);
    lv_label_set_long_mode(s_empty_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_empty_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_empty_hint, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_empty_hint, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    // 居中放到 “屏幕中心稍微偏下一些”（header 占了顶部），按 (panel_h+header)/2
    // 算出中心更自然一点。
    lv_obj_align(s_empty_hint, LV_ALIGN_TOP_MID, 0,
                 kHeaderH + (kPanelH - kHeaderH) / 2 - 50);
    screen_make_input_passive(s_empty_hint);
    chat_update_empty_hint();
}

}  // namespace

// ===========================================================================
// 公共接口
// ===========================================================================
lv_obj_t* ChatScreen::Create() {
    s_activation_blocked = !is_device_activated();
    if (s_activation_blocked) {
        log_activation_blocked();
    }

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_header(scr);
    build_message_list(scr);
    build_toggle_button(scr);

    if (s_activation_blocked) {
        open_activation_blocked_dialog();
        s_activation_guard_timer =
            lv_timer_create(on_activation_guard_timer, 1000, nullptr);
    }

    screen_attach_swipe_back(scr, on_swipe_back);
    lv_obj_add_event_cb(scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED &&
            s_activation_blocked) {
            ESP_LOGW(TAG, "screen loaded while not activated, keep dialog");
            ensure_activation_blocked_dialog();
        }
    }, LV_EVENT_SCREEN_LOADED, nullptr);
    return scr;
}

void ChatScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    auto& audio_service = Application::GetInstance().GetAudioService();
    if (event == SCREEN_LIFECYCLE_LOAD) {
        if (!is_device_activated()) {
            ESP_LOGW(TAG, "load: chat_screen blocked (device not activated)");
            log_activation_blocked();
        } else {
            ESP_LOGI(TAG, "load: chat_screen");
        }
        screen_register_pwr_key_toggle_chat();
        audio_service.EnableWakeWordDetection(true);
        RefreshDeviceState();
    } else {
        ESP_LOGI(TAG, "unload: chat_screen");
        screen_unregister_pwr_key_toggle_chat();
        Application::GetInstance().ForceReturnToIdle();
        audio_service.EnableWakeWordDetection(false);
    }
}

bool ChatScreen::IsActive() { return s_screen != nullptr && s_msg_list != nullptr; }

void ChatScreen::RefreshDeviceState() {
    if (!IsActive()) {
        return;
    }
    // SetDeviceState 跑在 main_event_loop，不能直接碰 LVGL。
    lv_async_call(on_refresh_device_state_async, nullptr);
}

void ChatScreen::AddMessage(const char* text, ChatMsgDir dir) {
    if (text == nullptr || text[0] == '\0') return;
    // 屏幕未在前台时直接丢弃，避免在后台无界堆积消息。
    if (!IsActive()) return;
    if (s_activation_blocked) return;

    chat_create_bubble_row(text, dir);
    chat_trim_old_msgs();
    chat_update_empty_hint();
    lv_obj_update_layout(s_msg_list);
    chat_scroll_to_latest(true);
}

void ChatScreen::ClearMessages() {
    if (s_msg_list == nullptr) return;
    const uint32_t count = lv_obj_get_child_count(s_msg_list);
    for (int32_t i = static_cast<int32_t>(count) - 1; i >= 0; --i) {
        lv_obj_delete(lv_obj_get_child(s_msg_list, i));
    }
    chat_update_empty_hint();
}
