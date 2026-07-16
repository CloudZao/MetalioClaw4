#include "test_screen.h"
#include "i18n.h"

#include "esp_log.h"
#include "home_screen/home_screen.h"
#include "auto_test_screen.h"
#include "stress_test_screen.h"
#include "screen_util.h"
#include "test_ui_common.h"

namespace {

constexpr const char* TAG = "TestScreen";

lv_obj_t* s_screen = nullptr;
screen_lifecycle_cb_t s_lifecycle_cb = nullptr;

void OnSwipeBackHome() {
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

void OnBackBtnClicked(lv_event_t* /*e*/) {
    OnSwipeBackHome();
}

void OnAutoTestClicked(lv_event_t* /*e*/) {
    TestUiNavigateTo(AutoTestScreen::Create);
}

void OnStressTestClicked(lv_event_t* /*e*/) {
    TestUiNavigateTo(StressTestScreen::Create);
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    s_screen = nullptr;
}

}  // namespace

lv_obj_t* TestScreen::Create() {
    ESP_LOGI(TAG, "create test screen menu");

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kTestPanelW, kTestPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kTestColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    TestUiCreateHeader(scr, I18n::T("硬件测试"), OnBackBtnClicked);

    lv_obj_t* body = TestUiCreateScrollBody(scr);
    TestUiCreateMenuRow(body, I18n::T("自动测试"), OnAutoTestClicked);
    TestUiCreateMenuRow(body, I18n::T("压力测试"), OnStressTestClicked);

    if (s_lifecycle_cb != nullptr) {
        screen_attach_lifecycle(scr, s_lifecycle_cb);
    }

    screen_attach_swipe_back(scr, OnSwipeBackHome);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    return scr;
}

void TestScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: test_screen");
    } else {
        ESP_LOGI(TAG, "unload: test_screen");
    }
}

void TestScreen::LaunchFromHome(screen_lifecycle_cb_t lifecycle_cb) {
    s_lifecycle_cb = lifecycle_cb;
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = TestScreen::Create();
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}
