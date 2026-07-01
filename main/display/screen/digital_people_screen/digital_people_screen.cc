#include "digital_people_screen.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "esp_log.h"
#include "lv_eaf.h"

#include "application.h"
#include "device_state.h"
#include "SdCardManager.hpp"
#include "home_screen/home_screen.h"
#include "screen_util.h"

LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_20_4);

namespace {

constexpr const char* TAG = "DigitalPeopleScreen";

constexpr int32_t  kPanelSize    = 720;
constexpr uint32_t kColorBg      = 0x000000;          // 纯黑背景

// 表情资源目录与扩展名（改 DIGITAL_PEOPLE_EMOTION_EXT 切换格式）：
//   完整路径 = kEmotionDir + 大类名 + kEmotionExt
//   例: "S:/sdcard/system/emotion/loving.sjpg"
// 大类名来自 LVAdapterDisplay::SetEmotion 里的 GetEmoteCategory()，
// 取值范围被收敛到 6 个：crying / happy / loving / neutral / surprised /
// thinking。所有资源都放在 SD 卡的 system/emotion/ 目录下。
constexpr const char* kEmotionDir = "S:/sdcard/system/emotion/";
constexpr const char* kEmotionPosixDir = "/sdcard/system/emotion/";
constexpr const char* kEmotionExt = DIGITAL_PEOPLE_EMOTION_EXT;
constexpr const char* kDefaultEmotion = "neutral";

// 端侧必须存在的 6 个大类资源；缺任意一个都视为资源包未就绪。
constexpr const char* kRequiredEmotions[] = {
    "crying", "happy", "loving", "neutral", "surprised", "thinking",
};
constexpr size_t kRequiredEmotionCount =
    sizeof(kRequiredEmotions) / sizeof(kRequiredEmotions[0]);

// 记录"当前应该播放哪一张"——LVAdapterDisplay 在屏幕没进前台时也可以
// 调用 SetEmotion 预置；下次 Create() 拿这个值拼路径。
// s_emotion_path_buf 是 lv_eaf_set_src / lv_image_set_src 传入的路径缓冲，
// 必须保证在调用之间一直有效，所以放在 namespace 静态。
constexpr size_t kEmotionPathBufSize = 64;
char s_current_emotion[24] = "neutral";
char s_emotion_path_buf[kEmotionPathBufSize];

const char* EmotionCategoryName(const char* category) {
    return (category != nullptr && category[0] != '\0') ? category
                                                        : kDefaultEmotion;
}

const char* BuildEmotionPath(const char* category) {
    std::snprintf(s_emotion_path_buf, sizeof(s_emotion_path_buf), "%s%s%s",
                  kEmotionDir, EmotionCategoryName(category), kEmotionExt);
    return s_emotion_path_buf;
}

bool EmotionUsesEaf() { return std::strcmp(kEmotionExt, ".eaf") == 0; }

void SetEmotionSrc(lv_obj_t* widget, const char* category) {
    const char* path = BuildEmotionPath(category);
    if (EmotionUsesEaf()) {
        lv_eaf_set_src(widget, path);
    } else {
        lv_image_set_src(widget, path);
    }
}

lv_obj_t* CreateEmotionWidget(lv_obj_t* parent) {
    if (EmotionUsesEaf()) {
        return lv_eaf_create(parent);
    }
    return lv_image_create(parent);
}

// ---------------------------------------------------------------------------
// 对话气泡视觉参数
//
//   ┌─────────────────────────────────────────┐ 0
//   │ [back]                                  │ ← back btn 占 (16,16) 60x60
//   │   ╭─ system bubble ─╮                   │ ← top-left，y 起点 kSysBubbleTop
//   │   ╰────────────────╯                    │
//   │                                         │
//   │            (gif 动画)                    │
//   │                                         │
//   │        ╭─ user bubble ─╮                │ ← bottom-center
//   │        ╰───────────────╯                │
//   └─────────────────────────────────────────┘ 720
//
//   - 气泡背景：深色 80% 透明（保证 gif 仍能透出一点），白色 2px 边框 +
//     圆角，符合用户要求 "类似聊天气泡白边框"。
//   - 文本：白色 puhui_30。
//   - 文本宽度：根据内容 + padding 计算，封顶 max_w 后换行（LV_LABEL_LONG_WRAP）。
//   - 屏幕未在前台时静态指针都被清空，所有显示接口直接 no-op。
// ---------------------------------------------------------------------------
constexpr int32_t  kBubbleRadius     = 18;
constexpr int32_t  kBubblePadX       = 18;
constexpr int32_t  kBubblePadY       = 14;
constexpr int32_t  kBubbleBorder     = 2;
constexpr int32_t  kSideMargin       = 16;
constexpr int32_t  kSysBubbleTop     = 24;            // 顶部安全间距
constexpr int32_t  kUserBubbleBottom = 24;
constexpr int32_t  kSysBubbleMaxW    = kPanelSize - kSideMargin * 2;       // 688
constexpr int32_t  kUserBubbleMaxW   = kPanelSize - kSideMargin * 2;       // 688

constexpr uint32_t kColorBubbleBg     = 0x000000;
constexpr uint32_t kColorBubbleBorder = 0xFFFFFF;
constexpr uint32_t kColorBubbleText   = 0xFFFFFF;
constexpr uint32_t kColorHintText     = 0xC8C9CC;
constexpr lv_opa_t kBubbleBgOpa       = LV_OPA_80;

struct UiState {
    lv_obj_t* screen        = nullptr;
    lv_obj_t* eaf           = nullptr;
    lv_obj_t* hint_label    = nullptr;
    lv_obj_t* system_bubble = nullptr;
    lv_obj_t* system_label  = nullptr;
    lv_obj_t* user_bubble   = nullptr;
    lv_obj_t* user_label    = nullptr;
};

UiState s_ui;

lv_timer_t* s_activation_guard_timer = nullptr;

// 未激活拦截：全屏模态弹窗，不可关闭，仅能通过返回键离开。
struct ActivationBlockedDialogUi {
    lv_obj_t* mask = nullptr;
};
ActivationBlockedDialogUi s_activation_dlg;
bool s_activation_blocked = false;

const lv_font_t* bubble_font() { return &font_puhui_30_4; }

bool EmotionFileExists(const char* name) {
    char path[96];
    std::snprintf(path, sizeof(path), "%s%s%s", kEmotionPosixDir, name,
                  kEmotionExt);
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

void LogMissingEmotionFiles() {
    for (size_t i = 0; i < kRequiredEmotionCount; ++i) {
        if (!EmotionFileExists(kRequiredEmotions[i])) {
            ESP_LOGW(TAG, "missing emotion file: %s%s%s", kEmotionPosixDir,
                     kRequiredEmotions[i], kEmotionExt);
        }
    }
}

bool CheckEmotionResourcesReady() {
    if (!SdCardManager::GetInstance().IsMounted()) {
        return false;
    }
    for (size_t i = 0; i < kRequiredEmotionCount; ++i) {
        if (!EmotionFileExists(kRequiredEmotions[i])) {
            return false;
        }
    }
    return true;
}

const char* MissingResourceHintText() {
    if (!SdCardManager::GetInstance().IsMounted()) {
        return "未检测到 SD 卡\n\n"
               "请将数字人资源包放入 SD 卡\n"
               "system/emotion/ 目录";
    }
    return "数字人资源缺失\n\n"
           "请将资源包复制到 SD 卡\n"
           "system/emotion/ 目录\n\n"
           "需包含 6 个表情资源文件";
}

lv_obj_t* BuildMissingResourceHint(lv_obj_t* parent) {
    lv_obj_t* hint = lv_label_create(parent);
    lv_label_set_text(hint, MissingResourceHintText());
    lv_obj_set_width(hint, kPanelSize - 80);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, bubble_font(), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
    screen_make_input_passive(hint);
    return hint;
}

void StyleBubble(lv_obj_t* bubble) {
    screen_strip_obj_chrome(bubble);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(kColorBubbleBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bubble, kBubbleBgOpa, LV_PART_MAIN);
    lv_obj_set_style_radius(bubble, kBubbleRadius, LV_PART_MAIN);
    lv_obj_set_style_border_color(bubble, lv_color_hex(kColorBubbleBorder),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, kBubbleBorder, LV_PART_MAIN);
    lv_obj_set_style_border_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bubble, kBubblePadX, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bubble, kBubblePadY, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
}

// 创建带白边的聊天气泡，初始隐藏。dir 决定气泡在父容器内的对齐方式。
struct BubbleHandles {
    lv_obj_t* bubble;
    lv_obj_t* label;
};

BubbleHandles BuildBubble(lv_obj_t* parent, lv_align_t align, int32_t x_ofs,
                          int32_t y_ofs) {
    lv_obj_t* bubble = lv_obj_create(parent);
    StyleBubble(bubble);
    lv_obj_set_width(bubble, 100);  // 占位，AddMessage 时会重算
    lv_obj_align(bubble, align, x_ofs, y_ofs);
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* label = lv_label_create(bubble);
    lv_label_set_text(label, "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, bubble_font(), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorBubbleText),
                                LV_PART_MAIN);

    // 气泡不消费输入，让右滑返回手势能穿透。
    screen_make_input_passive(bubble);
    return {bubble, label};
}

void UpdateBubble(lv_obj_t* bubble, lv_obj_t* label, const char* text,
                  int32_t max_w) {
    if (bubble == nullptr || label == nullptr) return;

    const lv_font_t* font = bubble_font();
    int32_t text_w = lv_txt_get_width(text, std::strlen(text), font, 0);
    if (text_w < 32) text_w = 32;
    int32_t bubble_w = text_w + kBubblePadX * 2 + kBubbleBorder * 2;
    if (bubble_w > max_w) bubble_w = max_w;

    lv_obj_set_width(bubble, bubble_w);
    lv_obj_set_width(label, bubble_w - kBubblePadX * 2 - kBubbleBorder * 2);
    lv_label_set_text(label, text);
    lv_obj_update_layout(label);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_HIDDEN);
    // 重新对齐：内容变化后宽高会重算，需要再 align 一次保证锚点正确。
    lv_obj_update_layout(bubble);
}

void OnSwipeBack();

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
    ESP_LOGW(TAG, "DigitalPeople blocked: device not activated");
    if (app.HasPendingActivation()) {
        ESP_LOGW(TAG, "pending activation code: %s",
                 app.GetPendingActivationCode().c_str());
    }
    if (app.GetDeviceState() == kDeviceStateActivating) {
        ESP_LOGW(TAG, "device state: activating");
    }
}

void open_activation_blocked_dialog() {
    if (s_ui.screen == nullptr || s_activation_dlg.mask != nullptr) {
        return;
    }

    auto& app = Application::GetInstance();
    const bool has_code = app.HasPendingActivation();

    constexpr int32_t kCardW = 520;
    const int32_t kCardH = has_code ? 420 : 340;
    constexpr int32_t kBackBtnW = 200;
    constexpr int32_t kBackBtnH = 72;

    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelSize, kPanelSize);
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
    lv_label_set_text(desc, "请先完成设备激活后再使用数字人。");
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
                        [](lv_event_t* /*e*/) { OnSwipeBack(); },
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

void OnSwipeBack() {
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home    = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    if (s_activation_guard_timer != nullptr) {
        lv_timer_delete(s_activation_guard_timer);
        s_activation_guard_timer = nullptr;
    }
    s_activation_dlg = ActivationBlockedDialogUi{};
    s_activation_blocked = false;
    s_ui.screen        = nullptr;
    s_ui.eaf           = nullptr;
    s_ui.hint_label    = nullptr;
    s_ui.system_bubble = nullptr;
    s_ui.system_label  = nullptr;
    s_ui.user_bubble   = nullptr;
    s_ui.user_label    = nullptr;
}

}  // namespace

lv_obj_t* DigitalPeopleScreen::Create() {
    s_activation_blocked = !is_device_activated();
    if (s_activation_blocked) {
        log_activation_blocked();
    }

    ESP_LOGI(TAG, "create digital people screen");

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);


    const bool resources_ready = CheckEmotionResourcesReady();
    if (!resources_ready) {
        ESP_LOGW(TAG, "emotion resources not ready (mounted=%d)",
                 SdCardManager::GetInstance().IsMounted() ? 1 : 0);
        LogMissingEmotionFiles();
        s_ui.hint_label = BuildMissingResourceHint(scr);
    } else {
        s_ui.eaf = CreateEmotionWidget(scr);
        SetEmotionSrc(s_ui.eaf, s_current_emotion);
        lv_image_set_inner_align(s_ui.eaf, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_center(s_ui.eaf);
        screen_make_input_passive(s_ui.eaf);
    }

    // 两个气泡：system 锚到左上（让出 back 按钮位置），user 锚到底部居中。
    {
        BubbleHandles sys = BuildBubble(scr, LV_ALIGN_TOP_LEFT, kSideMargin,
                                        kSysBubbleTop);
        s_ui.system_bubble = sys.bubble;
        s_ui.system_label  = sys.label;

        BubbleHandles usr = BuildBubble(scr, LV_ALIGN_BOTTOM_MID, 0,
                                        -kUserBubbleBottom);
        s_ui.user_bubble = usr.bubble;
        s_ui.user_label  = usr.label;
    }

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    if (s_activation_blocked) {
        open_activation_blocked_dialog();
        s_activation_guard_timer =
            lv_timer_create(on_activation_guard_timer, 1000, nullptr);
    }

    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED &&
            s_activation_blocked) {
            ESP_LOGW(TAG, "screen loaded while not activated, keep dialog");
            ensure_activation_blocked_dialog();
        }
    }, LV_EVENT_SCREEN_LOADED, nullptr);

    return scr;
}

bool DigitalPeopleScreen::IsActive() {
    return s_ui.screen != nullptr;
}

void DigitalPeopleScreen::ShowUserMessage(const char* text) {
    if (!IsActive() || text == nullptr || text[0] == '\0') return;
    if (s_activation_blocked) return;
    UpdateBubble(s_ui.user_bubble, s_ui.user_label, text, kUserBubbleMaxW);
    // 重新对齐到底部中点，让宽度变化后视觉居中。
    lv_obj_align(s_ui.user_bubble, LV_ALIGN_BOTTOM_MID, 0, -kUserBubbleBottom);
}

void DigitalPeopleScreen::ShowSystemMessage(const char* text) {
    if (!IsActive() || text == nullptr || text[0] == '\0') return;
    if (s_activation_blocked) return;
    UpdateBubble(s_ui.system_bubble, s_ui.system_label, text, kSysBubbleMaxW);
    lv_obj_align(s_ui.system_bubble, LV_ALIGN_TOP_LEFT, kSideMargin,
                 kSysBubbleTop);
}

void DigitalPeopleScreen::ClearMessages() {
    if (s_ui.system_bubble != nullptr) {
        lv_label_set_text(s_ui.system_label, "");
        lv_obj_add_flag(s_ui.system_bubble, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ui.user_bubble != nullptr) {
        lv_label_set_text(s_ui.user_label, "");
        lv_obj_add_flag(s_ui.user_bubble, LV_OBJ_FLAG_HIDDEN);
    }
}

void DigitalPeopleScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    auto& audio_service = Application::GetInstance().GetAudioService();
    if (event == SCREEN_LIFECYCLE_LOAD) {
        if (!is_device_activated()) {
            ESP_LOGW(TAG,
                     "load: digital_people_screen blocked (device not activated)");
            log_activation_blocked();
        } else {
            ESP_LOGI(TAG, "load: digital_people_screen");
        }
        screen_register_pwr_key_toggle_chat();
        audio_service.EnableWakeWordDetection(true);
    } else {
        ESP_LOGI(TAG, "unload: digital_people_screen");
        screen_unregister_pwr_key_toggle_chat();
        Application::GetInstance().ForceReturnToIdle();
        audio_service.EnableWakeWordDetection(false);
    }
}

void DigitalPeopleScreen::SetEmotion(const char* category) {
    if (category == nullptr || category[0] == '\0') {
        category = kDefaultEmotion;
    }
    // 同步更新静态缓存：屏幕不在前台时也能记住请求，下次 Create()
    // 走 BuildEmotionPath(s_current_emotion) 时就会用上。
    std::strncpy(s_current_emotion, category, sizeof(s_current_emotion) - 1);
    s_current_emotion[sizeof(s_current_emotion) - 1] = '\0';

    ESP_LOGI(TAG, "SetEmotion -> %s", s_current_emotion);

    // 在前台才真的替换表情源；调用方必须已经持有 LVGL 主锁
    // （和 ShowUserMessage / ShowSystemMessage 一致的约定）。
    if (s_ui.eaf != nullptr) {
        SetEmotionSrc(s_ui.eaf, s_current_emotion);
    }
}
