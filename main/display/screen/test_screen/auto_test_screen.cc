#include "auto_test_screen.h"
#include "i18n.h"

#include "esp_log.h"
#include "test_screen.h"
#include "audio_test.h"
#include "camera_test.h"
#include "vibrate_motor_test.h"
#include "qmc6309_test.h"
#include "sc7a20h_test.h"
#include "gps_test.h"
#include "sd_card_test.h"
#include "cell_4g_test.h"
#include "wifi_test.h"
#include "bq27220_test.h"
#include "nu1680_test.h"
#include "pin_gpio_test.h"
#include "pwr_key_handler.h"
#include "screen_util.h"
#include "test_ui_common.h"

namespace {

constexpr const char* TAG = "AutoTestScreen";

constexpr uint32_t kPollPeriodMs = 500;

lv_obj_t*   s_screen     = nullptr;
lv_timer_t* s_poll_timer = nullptr;

void OnPollTimer(lv_timer_t* /*t*/) {
    AudioTest::Poll();
    CameraTest::Poll();
    Sc7a20hTest::Poll();
    Qmc6309Test::Poll();
    SdCardTest::Poll();
    GpsTest::Poll();
    Bq27220Test::Poll();
    Nu1680Test::Poll();
}

void OnSwipeBackToMenu() {
    TestUiNavigateTo(TestScreen::Create);
}

void OnBackBtnClicked(lv_event_t* /*e*/) {
    OnSwipeBackToMenu();
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    if (s_poll_timer != nullptr) {
        lv_timer_delete(s_poll_timer);
        s_poll_timer = nullptr;
    }
    TestUiDismissConfirmDialog();
    TestUiSetScreen(nullptr);
    VibrateMotorTest::OnUnload();
    AudioTest::OnUnload();
    CameraTest::OnUnload();
    Sc7a20hTest::OnUnload();
    Qmc6309Test::OnUnload();
    SdCardTest::OnUnload();
    Cell4gTest::OnUnload();
    WifiTest::OnUnload();
    GpsTest::OnUnload();
    Bq27220Test::OnUnload();
    Nu1680Test::OnUnload();
    PinGpioTest::OnUnload();
    s_screen = nullptr;
}

void OnScreenLoadItems() {
    VibrateMotorTest::OnLoad();
    AudioTest::OnLoad();
    CameraTest::OnLoad();
    Sc7a20hTest::OnLoad();
    Qmc6309Test::OnLoad();
    SdCardTest::OnLoad();
    Cell4gTest::OnLoad();
    WifiTest::OnLoad();
    GpsTest::OnLoad();
    Bq27220Test::OnLoad();
    Nu1680Test::OnLoad();
    PinGpioTest::OnLoad();
}

void auto_test_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("auto_test", event);
}

}  // namespace

lv_obj_t* AutoTestScreen::Create() {
    ESP_LOGI(TAG, "create auto test screen");

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    TestUiSetScreen(scr);
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kTestPanelW, kTestPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kTestColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    TestUiCreateHeader(scr, I18n::T("自动测试"), OnBackBtnClicked);

    lv_obj_t* body = TestUiCreateScrollBody(scr);

    VibrateMotorTest::BuildRow(body);
    AudioTest::BuildRow(body);
    CameraTest::BuildRow(body);
    Cell4gTest::BuildRow(body);
    WifiTest::BuildRow(body);
    Sc7a20hTest::BuildRow(body);
    Qmc6309Test::BuildRow(body);
    SdCardTest::BuildRow(body);
    GpsTest::BuildRow(body);
    Bq27220Test::BuildRow(body);
    Nu1680Test::BuildRow(body);
    PinGpioTest::BuildRow(body);

    OnScreenLoadItems();

    s_poll_timer = lv_timer_create(OnPollTimer, kPollPeriodMs, nullptr);

    screen_attach_lifecycle(scr, auto_test_lifecycle_cb);
    screen_attach_swipe_back(scr, OnSwipeBackToMenu);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    return scr;
}
