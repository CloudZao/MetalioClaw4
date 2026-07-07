#include "test_screen.h"

#include "esp_log.h"
#include "home_screen/home_screen.h"
#include "audio_test.h"
#include "camera_test.h"
#include "vibrate_motor_test.h"
#include "qmc6309_test.h"
#include "sc7a20h_test.h"
#include "gps_test.h"
#include "sd_card_test.h"
#include "cell_4g_test.h"
#include "wifi_test.h"
#include "screen_util.h"
#include "test_ui_common.h"

LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "TestScreen";

constexpr uint32_t kPollPeriodMs = 500;

lv_obj_t*   s_screen      = nullptr;
lv_timer_t* s_poll_timer  = nullptr;

void OnPollTimer(lv_timer_t* /*t*/) {
    AudioTest::Poll();
    CameraTest::Poll();
    Sc7a20hTest::Poll();
    Qmc6309Test::Poll();
    SdCardTest::Poll();
    GpsTest::Poll();
}

void OnSwipeBack() {
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
    OnSwipeBack();
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
}

}  // namespace

lv_obj_t* TestScreen::Create() {
    ESP_LOGI(TAG, "create test screen");

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    TestUiSetScreen(scr);
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kTestPanelW, kTestPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kTestColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // -------- 顶栏：左上角返回 --------
    lv_obj_t* header = lv_obj_create(scr);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kTestPanelW, kTestHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    constexpr int kBackBtnSize = 72;
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
    lv_obj_add_event_cb(back, OnBackBtnClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "硬件测试");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);

    // -------- 测试项列表 --------
    lv_obj_t* body = lv_obj_create(scr);
    screen_strip_obj_chrome(body);
    lv_obj_set_size(body, kTestPanelW, kTestBodyH);
    lv_obj_set_pos(body, 0, kTestBodyY);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(body, kTestSideMargin, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(body, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(body, kTestRowGap, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    VibrateMotorTest::BuildRow(body);
    AudioTest::BuildRow(body);
    CameraTest::BuildRow(body);
    Cell4gTest::BuildRow(body);
    WifiTest::BuildRow(body);
    Sc7a20hTest::BuildRow(body);
    Qmc6309Test::BuildRow(body);
    SdCardTest::BuildRow(body);
    GpsTest::BuildRow(body);

    OnScreenLoadItems();

    s_poll_timer = lv_timer_create(OnPollTimer, kPollPeriodMs, nullptr);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    return scr;
}

void TestScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: test_screen");
    } else {
        ESP_LOGI(TAG, "unload: test_screen");
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
    }
}

void TestScreen::LaunchFromHome(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = TestScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}
