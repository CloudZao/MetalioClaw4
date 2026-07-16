#include "pwr_key_handler.h"

#include <cstring>

#include "esp_log.h"
#include "lvgl.h"

#include "IOExpander.hpp"
#include "application.h"
#include "home_screen/home_screen.h"
#include "standby_screen/standby_screen.h"

namespace {

constexpr const char* TAG = "PwrKey";
// 栈空 / 未知前台：短按不进待机（绝不能回落成 "home"，否则二级页
// 父页 UNLOAD 时会被误判为首页）。
constexpr const char* kNoneScreen = "none";
constexpr const char* kHomeScreen = "home";
constexpr uint32_t kLongPressMs = 1500;
constexpr int kMaxStack = 8;

const char* s_stack[kMaxStack] = {};
int s_depth = 0;
bool s_inited = false;

bool IsChatToggleScreen(const char* name) {
    return std::strcmp(name, "chat") == 0 ||
           std::strcmp(name, "digital_people") == 0;
}

void StackPush(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    if (s_depth < kMaxStack) {
        s_stack[s_depth++] = name;
    } else {
        s_stack[kMaxStack - 1] = name;
        ESP_LOGW(TAG, "screen stack full, replace top with %s", name);
    }
}

void StackRemoveTopmost(const char* name) {
    if (name == nullptr || s_depth <= 0) {
        return;
    }
    for (int i = s_depth - 1; i >= 0; --i) {
        if (std::strcmp(s_stack[i], name) == 0) {
            for (int j = i; j < s_depth - 1; ++j) {
                s_stack[j] = s_stack[j + 1];
            }
            --s_depth;
            s_stack[s_depth] = nullptr;
            return;
        }
    }
}

const char* StackTop() {
    return s_depth > 0 ? s_stack[s_depth - 1] : kNoneScreen;
}

void OnEnterStandbyAsync(void* /*arg*/) { StandbyScreen::Show(); }

void OnLeaveStandbyAsync(void* /*arg*/) { StandbyScreen::ReturnHome(); }

void OnShortPress() {
    const char* screen = PwrKey_ActiveScreen();
    ESP_LOGI(TAG, "short-press on screen=%s (depth=%d)", screen, s_depth);

    if (std::strcmp(screen, kHomeScreen) == 0) {
        ESP_LOGI(TAG, "dispatch: enter standby_screen");
        lv_async_call(OnEnterStandbyAsync, nullptr);
        return;
    }

    if (std::strcmp(screen, "standby") == 0) {
        ESP_LOGI(TAG, "dispatch: leave standby -> home");
        lv_async_call(OnLeaveStandbyAsync, nullptr);
        return;
    }

    if (IsChatToggleScreen(screen)) {
        ESP_LOGI(TAG, "dispatch: ToggleChatState()");
        Application::GetInstance().ToggleChatState();
        return;
    }

    ESP_LOGI(TAG, "dispatch: no-op (screen has no short-press action)");
}

void OnLongPressAsync(void* /*arg*/) {
    HomeScreen::ShowPowerOptionsDialog();
}

void OnLongPress() {
    const char* screen = PwrKey_ActiveScreen();
    ESP_LOGI(TAG, "long-press %ums on screen=%s -> power dialog",
             static_cast<unsigned>(kLongPressMs), screen);
    lv_async_call(OnLongPressAsync, nullptr);
}

}  // namespace

void PwrKey_Init() {
    if (s_inited) {
        return;
    }

    auto& io = IOExpander::getInstance();
    const esp_err_t click_err = io.onClick(IOExpander::Pin::PWR_KEY, OnShortPress);
    if (click_err != ESP_OK) {
        ESP_LOGE(TAG, "onClick register failed: 0x%x",
                 static_cast<unsigned>(click_err));
        return;
    }

    const esp_err_t long_err =
        io.onLongPress(IOExpander::Pin::PWR_KEY, kLongPressMs, OnLongPress);
    if (long_err != ESP_OK) {
        ESP_LOGE(TAG, "onLongPress register failed: 0x%x",
                 static_cast<unsigned>(long_err));
        return;
    }

    s_inited = true;
    s_depth = 0;  // 等 HomeScreen LOAD 再入栈，避免残留假 "home"
    ESP_LOGI(TAG,
             "armed: short-press + long-press %ums (active_screen=%s)",
             static_cast<unsigned>(kLongPressMs), PwrKey_ActiveScreen());
}

void PwrKey_OnScreenLifecycle(const char* name,
                              screen_lifecycle_event_t event) {
    if (name == nullptr || name[0] == '\0') {
        name = kNoneScreen;
    }

    if (event == SCREEN_LIFECYCLE_LOAD) {
        StackPush(name);
        ESP_LOGD(TAG, "active_screen -> %s (load %s, depth=%d)", StackTop(),
                 name, s_depth);
        return;
    }

    // UNLOAD：只移除栈里最靠上的同名页，绝不能回落成 "home"。
    // 否则 Test→AutoTest 这类「父页销毁、子页未登记」会误进待机。
    StackRemoveTopmost(name);
    ESP_LOGD(TAG, "active_screen -> %s (unload %s, depth=%d)", StackTop(), name,
             s_depth);
}

const char* PwrKey_ActiveScreen() {
    return StackTop();
}
