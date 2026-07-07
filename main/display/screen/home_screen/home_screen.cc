#include "home_screen.h"

#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <ctime>
#include <string>
#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <font_awesome.h>

#include "application.h"
#include "board.h"
#include "dual_network_board.h"
#include "nt26_board.h"
#include "IOExpander.hpp"
#include "bq27220_gauge.h"
#include "settings.h"
#include "backlight_screen/backlight_screen.h"
#include "bluetooth_screen/bluetooth_screen.h"
#include "calculator_screen/calculator_screen.h"
#include "calendar_screen/calendar_screen.h"
#include "call_screen/call_screen.h"
#include "camera_screen/camera_screen.h"
#include "chat_screen/chat_screen.h"
#include "digital_people_screen/digital_people_screen.h"
#include "game_2048_screen/game_2048_screen.h"
#include "gps_screen/gps_screen.h"
#include "level_screen/level_screen.h"
#include "magnet_screen/magnet_screen.h"
#include "music_screen/music_screen.h"
#include "openclaw_screen/openclaw_screen.h"
#include "screen_util.h"
#include "vibrate_screen/vibrate_screen.h"
#include "weather_screen/weather_screen.h"
#include "network_screen/network_screen.h"
#include "pin_test_screen/pin_test_screen.h"
#include "test_screen/test_screen.h"
#include "sd_card_screen/sd_card_screen.h"
#include "theme_screen/theme_screen.h"
#include "info_screen/info_screen.h"
#include "theme_manager.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_awesome_20_4);

// 电池显示模式：1 = 图标（Font Awesome 字形，放最右边），0 = 文字（"电量 NN% 充电中 4.02V"）
#define HOME_STATUS_SHOW_BATTERY_ICON 1

namespace {

constexpr const char* TAG_HOME = "HomeScreen";

// ---------------------------------------------------------------------------
// Per-app lifecycle callbacks
//
// Each launcher hands its callback to screen_attach_lifecycle() so we get a
// LOAD notification right after the new screen becomes active, and an
// UNLOAD notification when LVGL switches away from it.  For now we only log
// the transitions -- but this is the right place to hang start / stop
// behaviour that should track a specific app's lifetime (e.g. pausing the
// audio player when the player screen is dismissed).
//
// All callbacks share the same shape so they can all sit in the AppEntry
// table below.  A nullptr callback simply skips logging for that app.
// ---------------------------------------------------------------------------

void game_2048_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: game_2048");
    } else {
        ESP_LOGI(TAG_HOME, "unload: game_2048");
    }
}

void calculator_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: calculator");
    } else {
        ESP_LOGI(TAG_HOME, "unload: calculator");
    }
}

void call_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: call_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: call_screen");
    }
    // 转发给 CallScreen 自己处理 PA_SWITCH 切换以及通话兜底挂断。
    CallScreen::LifecycleCallback(event);
}

void calendar_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: calendar_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: calendar_screen");
    }
}

// 音乐界面的生命周期回调：把 BT 切到模式3、注册 / 摘 UART 回调全部
// 由 MusicScreen::LifecycleCallback 内部统一处理。
void music_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: music_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: music_screen");
    }
    MusicScreen::LifecycleCallback(event);
}

void weather_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: weather_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: weather_screen");
    }
}

// GPS 屏幕的 GPS_POWER 开关已经搬到 GpsScreen::LifecycleCallback；这里
// 只保留日志 + 转发，与 camera / vibrate / bluetooth 等屏幕的写法对齐。
void gps_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: gps_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: gps_screen");
    }
    GpsScreen::LifecycleCallback(event);
}

// 相机生命周期：直接转发到 CameraScreen::LifecycleCallback。
// 那里实现了 CAM_PWDN（TCA9555 IO2，低电平通电）的拉低 / 拉高，以及
// esp_video / V4L2 流水线的启停。这样摄像头只在该 App 处于前台时通电。
void camera_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: camera_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: camera_screen");
    }
    CameraScreen::LifecycleCallback(event);
}

// 震动生命周期：转发给 VibrateScreen::LifecycleCallback。
// LOAD 时确保 LEDC 初始化、duty=0；UNLOAD 时停掉 pattern timer 并把 duty=0，
// 保证用户离开屏幕马达不会还在响。
void vibrate_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: vibrate_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: vibrate_screen");
    }
    VibrateScreen::LifecycleCallback(event);
}

void bluetooth_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: bluetooth_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: bluetooth_screen");
    }
    BluetoothScreen::LifecycleCallback(event);
}

// 网络配置：进入页面停掉 WifiStation 并自己接管 STA 栈用于扫描 / 连接，
// 离开时还原。详细逻辑在 NetworkScreen::LifecycleCallback 内。
void wifi_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: network_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: network_screen");
    }
    NetworkScreen::LifecycleCallback(event);
}

void chat_lifecycle_cb(screen_lifecycle_event_t event) { ChatScreen::LifecycleCallback(event); }

void digital_people_lifecycle_cb(screen_lifecycle_event_t event) {
    DigitalPeopleScreen::LifecycleCallback(event);
}

// SD 卡生命周期：转发给 SdCardScreen::LifecycleCallback。
// LOAD 时挂载 SD 卡并刷新文件列表，UNLOAD 时安全卸载 SD 卡、
// 断电省电。
void sd_card_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: sd_card_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: sd_card_screen");
    }
    SdCardScreen::LifecycleCallback(event);
}

// 引脚测试生命周期：屏幕自身会在 UNLOAD 时清掉输入轮询 / 周期方波 timer，
// 这里只多兜底一次以及打 log。
void pin_test_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: pin_test_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: pin_test_screen");
    }
    PinTestScreen::LifecycleCallback(event);
}

void test_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: test_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: test_screen");
    }
    TestScreen::LifecycleCallback(event);
}

// 水平仪生命周期：转发给 LevelScreen::LifecycleCallback。LOAD 时确保
// SC7A20H 已经 probe + configure 过，UNLOAD 时让 LevelScreen 自己关掉
// sample timer（OnScreenUnloaded 里实现）。
void level_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: level_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: level_screen");
    }
    LevelScreen::LifecycleCallback(event);
}

// 磁场生命周期：转发给 MagnetScreen::LifecycleCallback。LOAD 时把 QMC6309
// 磁力计 probe + configure 一遍；UNLOAD 由 MagnetScreen 自身在
// OnScreenUnloaded 里关掉采样 timer。
void magnet_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: magnet_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: magnet_screen");
    }
    MagnetScreen::LifecycleCallback(event);
}

// OpenClaw 生命周期：转发给 OpenClawScreen::LifecycleCallback，让屏幕
// 自己负责录音 / 上传任务的兜底关闭与 wake word 恢复。
void openclaw_lifecycle_cb(screen_lifecycle_event_t event) {
    OpenClawScreen::LifecycleCallback(event);
}


constexpr int kPanelSize = 720;
constexpr int kStatusBarHeight = 48;      
constexpr int kIndicatorAreaHeight = 40;  
constexpr int kPagerHeight =
    kPanelSize - kStatusBarHeight - kIndicatorAreaHeight;  // 632
constexpr int kAppsPerPage = 9;           // 3x3
constexpr int kPageCols = 3;
constexpr int kPageRows = 3;
constexpr int kCellWidth = 160;
constexpr int kCellHeight = 160;
constexpr int kGridColGap = 60;
constexpr int kGridRowGap = 45;
constexpr int kPagePadHor =
    (kPanelSize - kPageCols * kCellWidth - (kPageCols - 1) * kGridColGap) / 2;
constexpr int kPagePadVer =
    (kPagerHeight - kPageRows * kCellHeight - (kPageRows - 1) * kGridRowGap) / 2;

constexpr uint32_t kStatusBarBg = 0x000000;
constexpr int kMaxPages = 6;  // hard cap; bump if app list grows

// Grid descriptors -- static so the array pointers passed to LVGL outlive
// the call.  Initialized at namespace scope; LVGL reads them lazily during
// each page's relayout, so we never have to refresh them.
int32_t s_col_dsc[kPageCols + 1] = {
    kCellWidth,
    kCellWidth,
    kCellWidth,
    LV_GRID_TEMPLATE_LAST,
};
int32_t s_row_dsc[kPageRows + 1] = {
    kCellHeight,
    kCellHeight,
    kCellHeight,
    LV_GRID_TEMPLATE_LAST,
};

// Indicator dot geometry
constexpr int kDotSize = 8;
constexpr int kDotGap = 12;
constexpr int kIndicatorPadHor = 14;
constexpr int kIndicatorPadVer = 8;
constexpr int kIndicatorYOffset = 12;  // distance from panel bottom
constexpr uint32_t kIndicatorBg = 0x000000;
constexpr uint32_t kDotColor = 0xFFFFFF;

// Launchers take the lifecycle callback as an argument so we can attach it
// to the screen object before lv_screen_load() fires LV_EVENT_SCREEN_LOADED
// -- otherwise the first LOAD event would be missed.  Passing nullptr is
// supported (screen_attach_lifecycle no-ops in that case).
typedef void (*LaunchFn)(screen_lifecycle_cb_t lifecycle_cb);

struct AppEntry {
    // 图标资源后缀，例如 "chat"、"2048"。实际 LVGL 路径由 ThemeManager
    // 的当前主题 id 拼出：A:ic_app_home_theme{N}_{suffix}.spng。
    // 这样切换主题只需要重启后重新拼一次路径，不用改 AppEntry。
    const char* icon_suffix;
    const char* name;                    // display name shown under the icon
    LaunchFn launch;                     // tapped -> launch this app (nullptr = no action)
    screen_lifecycle_cb_t lifecycle_cb;  // load / unload observer
};

void LaunchGame2048(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* game = Game2048::Create();
    screen_attach_lifecycle(game, lifecycle_cb);
    lv_screen_load(game);
    if (old_scr != nullptr && old_scr != game) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchCalculator(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = Calculator::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchCall(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = CallScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchCalendar(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = CalendarScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchMusic(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = MusicScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchWeather(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = WeatherScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchGps(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = GpsScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchCamera(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = CameraScreen::Create();
    if (app == nullptr) {
        ESP_LOGE(TAG_HOME, "CameraScreen::Create() failed");
        return;
    }
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchVibrate(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = VibrateScreen::Create();
    if (app == nullptr) {
        ESP_LOGE(TAG_HOME, "VibrateScreen::Create() failed");
        return;
    }
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchBluetooth(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = BluetoothScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchWifi(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = NetworkScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchChat(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = ChatScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchDigitalPeople(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = DigitalPeopleScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchLevel(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = LevelScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchMagnet(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = MagnetScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchSdCard(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = SdCardScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchPinTest(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = PinTestScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchTest(screen_lifecycle_cb_t lifecycle_cb) {
    TestScreen::LaunchFromHome(lifecycle_cb);
}

void LaunchOpenClaw(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = OpenClawScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchTheme(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = ThemeScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchBacklight(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = BacklightScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchInfo(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = InfoScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void theme_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: theme_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: theme_screen");
    }
    ThemeScreen::LifecycleCallback(event);
}

void backlight_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: backlight_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: backlight_screen");
    }
    BacklightScreen::LifecycleCallback(event);
}

void info_lifecycle_cb(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: info_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: info_screen");
    }
    InfoScreen::LifecycleCallback(event);
}

// 主屏 app 表。icon_suffix 对应 ic_app_home_theme{N}_{suffix}.spng 的
// 中间 suffix 部分；name 是图标下方文字（注：当前主屏只画图标，文字
// label 已经移除，name 仅在日志 / 未来可访问性场景里用）。
// 注意 "定位" 的 suffix 是 "gps"（资源命名），不是 "map"；"磁场" 暂无
// 主题图标，资源 ic_app_home_themeN_magnet.spng 缺失时该格会显示为空。
constexpr AppEntry kApps[] = {
    {"chat",           "聊天",     LaunchChat,          chat_lifecycle_cb},
    {"wifi",           "网络配置", LaunchWifi,          wifi_lifecycle_cb},
    {"digital_people", "数字人",   LaunchDigitalPeople, digital_people_lifecycle_cb},
    {"call",           "电话",     LaunchCall,          call_lifecycle_cb},
    {"music",          "音乐",     LaunchMusic,         music_lifecycle_cb},
    {"calendar",       "日历",     LaunchCalendar,      calendar_lifecycle_cb},
    {"openclaw",       "OpenClaw", LaunchOpenClaw,      openclaw_lifecycle_cb},
    {"gps",            "定位",     LaunchGps,           gps_lifecycle_cb},
    {"camera",         "相机",     LaunchCamera,        camera_lifecycle_cb},
    {"spirit_level",   "水平仪",   LaunchLevel,         level_lifecycle_cb},
    {"magnet",         "磁场",     LaunchMagnet,        magnet_lifecycle_cb},
    {"vibrate",        "震动",     LaunchVibrate,       vibrate_lifecycle_cb},
    {"bluetooth",      "蓝牙配置", LaunchBluetooth,     bluetooth_lifecycle_cb},
    {"calculator",     "计算器",   LaunchCalculator,    calculator_lifecycle_cb},
    {"weather",        "天气",     LaunchWeather,       weather_lifecycle_cb},
    {"sd",             "SD卡",     LaunchSdCard,        sd_card_lifecycle_cb},
    {"pin",            "引脚测试", LaunchPinTest,       pin_test_lifecycle_cb},
    {"2048",           "2048",     LaunchGame2048,      game_2048_lifecycle_cb},
    {"backlight",      "屏幕亮度", LaunchBacklight,     backlight_lifecycle_cb},
    {"info",           "系统信息", LaunchInfo,          info_lifecycle_cb},
    {"theme",          "主题",     LaunchTheme,         theme_lifecycle_cb},
    {"test",          "测试",     LaunchTest,      test_lifecycle_cb},
};

constexpr int kTotalApps = static_cast<int>(sizeof(kApps) / sizeof(kApps[0]));

// 当前主题下，每个 app 解析出来的完整 LVGL 路径。在 HomeScreen::Create()
// 入口处由 EnsureIconPathsBuilt() 填写；主题 id 变化时会自动重新拼接。
//
// 路径字符串必须在 lv_image 引用期间一直有效（lv_image_set_src 不复制内
// 存），所以放在 namespace 静态而不是栈或函数局部。
constexpr int kIconPathBufSize = 56;
char s_icon_paths[kTotalApps][kIconPathBufSize];

void EnsureIconPathsBuilt() {
    static bool built = false;
    static int s_built_theme_id = 0;
    const int tid = ThemeManager::GetCurrentThemeId();
    // 第一次构建 / 主题 id 变更（理论上只在调试热改 NVS 时出现），都重新拼。
    if (built && s_built_theme_id == tid) {
        return;
    }
    for (int i = 0; i < kTotalApps; ++i) {
        std::snprintf(s_icon_paths[i], kIconPathBufSize,
                      "A:ic_app_home_theme%d_%s.spng",
                      tid, kApps[i].icon_suffix);
    }
    s_built_theme_id = tid;
    built = true;
    ESP_LOGI(TAG_HOME, "icon paths built for theme%d", tid);
}

// ---------------------------------------------------------------------------
// 主屏触摸：一次「无→有→无」为一段会话，screen 级统一分类与分发。
// 不再使用 cell 的 LV_EVENT_CLICKED。
// ---------------------------------------------------------------------------

constexpr int kHomeMoveThreshold = 5;       // 滑动激活 / 点击位移上限（|dx|、|dy| 均 < 此值才算 tap）
constexpr uint32_t kHomeLongPressMs = 750;  // 按下→松开 ≥ 此值且位移够小 → 长按

constexpr lv_obj_flag_t kAppCellFlag = LV_OBJ_FLAG_USER_2;

enum class HomeTouchKind {
    None,
    SwipeLeft,
    SwipeRight,
    SwipeUp,
    SwipeDown,
    Click,
    LongPress,
};

struct HomeTouchSession {
    bool active = false;
    bool consumed = false;  // 已由滑动手势消费
    int16_t start_x = 0;
    int16_t start_y = 0;
    uint32_t press_tick = 0;
    lv_obj_t* press_cell = nullptr;
    const AppEntry* app = nullptr;
};
HomeTouchSession s_home_touch;

// ---------------------------------------------------------------------------
// 主屏无操作计时：停留在 HomeScreen 时，5 分钟无任何触摸/滑动则自动关机；
// 任意用户操作重置计时。仅在主屏 LOADED→DELETE 生命周期内运行。
// ---------------------------------------------------------------------------
struct HomeIdleTimerState {
    lv_timer_t* timer = nullptr;
    uint32_t last_activity_tick = 0;
    uint32_t last_status_log_tick = 0;
    bool timeout_logged = false;
};

HomeIdleTimerState* s_home_idle = nullptr;

constexpr uint32_t kHomeIdleTimeoutMs = 5 * 60 * 1000;
constexpr uint32_t kHomeIdleStatusIntervalMs = 60 * 1000;
constexpr uint32_t kHomeIdleTimerPeriodMs = 1000;

void BeginSystemShutdown(const char* reason);

void ResetHomeIdleTimer() {
    if (s_home_idle == nullptr) {
        return;
    }
    s_home_idle->last_activity_tick = lv_tick_get();
    s_home_idle->last_status_log_tick = lv_tick_get();
    s_home_idle->timeout_logged = false;
}

void StopHomeIdleTimer() {
    if (s_home_idle == nullptr) {
        return;
    }
    if (s_home_idle->timer != nullptr) {
        lv_timer_delete(s_home_idle->timer);
        s_home_idle->timer = nullptr;
    }
    delete s_home_idle;
    s_home_idle = nullptr;
}

void OnHomeIdleTimerTick(lv_timer_t* timer) {
    auto* st = static_cast<HomeIdleTimerState*>(lv_timer_get_user_data(timer));
    if (st == nullptr) {
        return;
    }

    const uint32_t idle_ms = lv_tick_elaps(st->last_activity_tick);
    if (idle_ms >= kHomeIdleTimeoutMs) {
        if (!st->timeout_logged) {
            st->timeout_logged = true;
            ESP_LOGW(TAG_HOME,
                     "idle timer timeout: no user activity for %u s on HomeScreen, shutting down",
                     idle_ms / 1000);
            BeginSystemShutdown("主屏 5 分钟无操作自动关机");
        }
        return;
    }

    if (lv_tick_elaps(st->last_status_log_tick) < kHomeIdleStatusIntervalMs) {
        return;
    }

    st->last_status_log_tick = lv_tick_get();
    const uint32_t remaining_s = (kHomeIdleTimeoutMs - idle_ms) / 1000;
    ESP_LOGI(TAG_HOME,
             "idle timer: idle=%u s, remaining=%u s until auto shutdown",
             idle_ms / 1000, remaining_s);
}

void StartHomeIdleTimer() {
    StopHomeIdleTimer();

    s_home_idle = new HomeIdleTimerState{};
    s_home_idle->last_activity_tick = lv_tick_get();
    s_home_idle->last_status_log_tick = lv_tick_get();
    s_home_idle->timer =
        lv_timer_create(OnHomeIdleTimerTick, kHomeIdleTimerPeriodMs, s_home_idle);
    ESP_LOGI(TAG_HOME, "idle timer started (auto shutdown in %u s, status log every %u s)",
             kHomeIdleTimeoutMs / 1000, kHomeIdleStatusIntervalMs / 1000);
}

// App 卡片按压过渡：确认点击/长按后触发缩放。
constexpr int kCellRadius = 28;
constexpr uint32_t kCellPressScaleMs = 200;  // 与 GetPressTransition 时长一致

const lv_style_prop_t kPressTransProps[] = {
    LV_STYLE_TRANSFORM_SCALE_X,
    LV_STYLE_TRANSFORM_SCALE_Y,
    LV_STYLE_PROP_INV,
};

lv_style_transition_dsc_t& GetPressTransition() {
    static lv_style_transition_dsc_t dsc;
    static bool inited = false;
    if (!inited) {
        lv_style_transition_dsc_init(&dsc, kPressTransProps, lv_anim_path_ease_out,
                                     kCellPressScaleMs, 0, nullptr);
        inited = true;
    }
    return dsc;
}

void LaunchHomeApp(const AppEntry* app) {
    if (app != nullptr && app->launch != nullptr) {
        app->launch(app->lifecycle_cb);
    }
}

struct CellScaleCtx {
    lv_obj_t* cell = nullptr;
    const AppEntry* launch_app = nullptr;  // 非空：缩放结束后进入 app
};

lv_timer_t* s_cell_scale_timer = nullptr;
CellScaleCtx s_cell_scale_ctx;

void CancelCellScaleTimer() {
    if (s_cell_scale_timer == nullptr) {
        return;
    }
    lv_timer_delete(s_cell_scale_timer);
    s_cell_scale_timer = nullptr;
    s_cell_scale_ctx = CellScaleCtx{};
}

void OnCellScaleTimer(lv_timer_t* timer) {
    const CellScaleCtx ctx = s_cell_scale_ctx;
    s_cell_scale_timer = nullptr;
    s_cell_scale_ctx = CellScaleCtx{};
    lv_timer_delete(timer);

    if (ctx.launch_app != nullptr) {
        LaunchHomeApp(ctx.launch_app);
        return;
    }
    if (ctx.cell != nullptr) {
        lv_obj_remove_state(ctx.cell, LV_STATE_PRESSED);
    }
}

// launch_app：单击传入 app，缩放 kCellPressScaleMs 后再 launch；长按传 nullptr，仅缩放回弹。
void PlayAppCellPressScale(lv_obj_t* cell, const AppEntry* launch_app) {
    if (cell == nullptr) {
        return;
    }
    CancelCellScaleTimer();
    lv_obj_add_state(cell, LV_STATE_PRESSED);
    s_cell_scale_ctx.cell = cell;
    s_cell_scale_ctx.launch_app = launch_app;
    s_cell_scale_timer =
        lv_timer_create(OnCellScaleTimer, kCellPressScaleMs, nullptr);
    lv_timer_set_repeat_count(s_cell_scale_timer, 1);
}

lv_obj_t* FindAppCellFromTarget(lv_obj_t* target, lv_obj_t* screen) {
    for (lv_obj_t* obj = target; obj != nullptr && obj != screen;
         obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, kAppCellFlag)) {
            return obj;
        }
    }
    return nullptr;
}

lv_obj_t* CreateAppCell(lv_obj_t* parent, const AppEntry& entry, int idx) {
    lv_obj_t* cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, kCellWidth, kCellHeight);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);

    // 图标占满 160x160 区域；clip_corner 在按下放大时裁切溢出部分。
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(cell, kCellRadius, LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(cell, true, LV_PART_MAIN);
    lv_obj_set_style_transition(cell, &GetPressTransition(), LV_PART_MAIN);

    // 缩放仅在 screen 确认 Click / LongPress 后通过 add_state(PRESSED) 触发。
    lv_obj_set_style_transform_pivot_x(cell, LV_PCT(50), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_y(cell, LV_PCT(50), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(cell, 262, LV_PART_MAIN | LV_STATE_PRESSED);

    // 图标满铺整张区域（160x160）。
    // 路径取自 s_icon_paths[idx]：已按当前主题前缀解析好（ic_app_home_themeN_xxx）。
    lv_obj_t* icon = lv_image_create(cell);
    lv_image_set_src(icon, s_icon_paths[idx]);
    lv_obj_set_size(icon, kCellWidth, kCellHeight);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    if (entry.launch != nullptr) {
        // 可命中以便 PRESSED 时锁定 app；不用 LV_EVENT_CLICKED，由 screen 分发。
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(cell, kAppCellFlag);
        lv_obj_set_user_data(cell, const_cast<AppEntry*>(&entry));
    }
    return cell;
}

// ---------------------------------------------------------------------------
// Pager + page-indicator
//
// Layout
//   pager (720 x kPagerHeight) -- horizontal scroll, snap to page center.
//     each child is a Page object (also 720 x kPagerHeight) which holds a
//     3x3 grid of cells.  We mark every page LV_OBJ_FLAG_SNAPPABLE so a
//     swipe ends with one page perfectly centered.
//
// State
//   PagerState lives on the heap and is owned by the screen via
//   LV_EVENT_DELETE.  The scroll callback uses it to map scroll position
//   -> current page and re-tint the dots.  No globals; if HomeScreen is
//   torn down and rebuilt, the new instance allocates a fresh state.
// ---------------------------------------------------------------------------

struct PagerState {
    lv_obj_t* pager;
    lv_obj_t* dots[kMaxPages];
    int page_count;
    int current_page;
};

// 记住用户上次停留在主屏的哪一页，便于从子页面返回时回到同一页。
// 子页面调用 HomeScreen::Create() 重建时，会读取这个值滚动到对应页。
// 用户每次翻页（GoToPage）会写入；HighlightDot 不写入，否则
// CreateIndicator 初始化时调用 HighlightDot(state, 0) 会把它清零。
int s_last_home_page = 0;

struct HomeStatusState {
    lv_obj_t* bar = nullptr;
    lv_obj_t* network_icon_lbl = nullptr;
    lv_obj_t* network_type_lbl = nullptr;
    lv_obj_t* sim_slot_lbl = nullptr;       // 仅 4G 模式下显示：外置卡 / 内置卡
    lv_obj_t* battery_icon_lbl = nullptr;   // 电池图标（Font Awesome 字形）
    lv_obj_t* battery_pct_lbl  = nullptr;   // 电池电量文字，例如 "电量 85%"
    lv_obj_t* time_lbl = nullptr;
    lv_obj_t* activation_code_lbl = nullptr;
    lv_timer_t* update_timer = nullptr;
    const char* last_icon = nullptr;
    const char* last_battery_icon = nullptr; // 缓存上次图标，避免重复 set_text
    int  last_battery_pct = -1;
    bool last_battery_low = false;  // 缓存上一次的低电量染色状态
    int  last_net_type = -1;        // 缓存上次显示的网络类型，避免每秒重绘 SIM 标签
    int  last_sim_slot = -1;        // 缓存上次显示的 SIM 槽位
    std::string last_activation_text;  // 缓存验证码文案，避免每秒 invalidate
    bool last_activation_visible = false;
};

void UpdateHomeStatusBar(HomeStatusState* st);

int GetSavedNetworkType() {
    // 与 DualNetworkBoard / network_screen 共用 NVS 读取逻辑
    constexpr int kDefaultNetType = 1;  // 默认 4G，与 metalio-claw-4 板级一致
    const NetworkType type =
        DualNetworkBoard::LoadNetworkTypeFromSettings(kDefaultNetType);
    return type == NetworkType::ML307 ? 1 : 0;
}

// 与 network_screen 共用 "network/sim_slot" 这一 NVS key。
// 返回 0 = 外置卡（默认）/ 1 = 内置卡。
int GetSavedSimSlot() {
    Settings settings("network", true);
    int v = settings.GetInt("sim_slot", 0);
    return (v == 1) ? 1 : 0;
}

void SaveSimSlot(int slot) {
    Settings settings("network", true);
    settings.SetInt("sim_slot", slot);
}

Nt26Board* GetNt26Board() {
    auto& board = Board::GetInstance();
    auto* dual = dynamic_cast<DualNetworkBoard*>(&board);
    if (dual != nullptr) {
        return dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard());
    }
    return dynamic_cast<Nt26Board*>(&board);
}

// 开机向模组查询一次当前 SIM 槽位，写回 NVS 并刷新状态栏。
HomeStatusState* s_home_status = nullptr;
bool s_boot_sim_slot_query_done = false;

int ParseSimSlotFromEcsimcfg(const std::string& resp) {
    constexpr const char* kKey = "\"SimSlot\"";
    size_t pos = 0;
    while ((pos = resp.find(kKey, pos)) != std::string::npos) {
        size_t comma = resp.find(',', pos);
        if (comma == std::string::npos) {
            return -1;
        }
        size_t i = comma + 1;
        while (i < resp.size() && (resp[i] == ' ' || resp[i] == '\t')) {
            ++i;
        }
        if (i >= resp.size() ||
            !std::isdigit(static_cast<unsigned char>(resp[i]))) {
            pos = comma + 1;
            continue;
        }
        int slot = 0;
        while (i < resp.size() &&
               std::isdigit(static_cast<unsigned char>(resp[i]))) {
            slot = slot * 10 + (resp[i] - '0');
            ++i;
        }
        return slot;
    }
    return -1;
}

struct BootSimSlotQueryMsg {
    int slot = -1;
};

void AsyncBootSimSlotSynced(void* user_data) {
    auto* msg = static_cast<BootSimSlotQueryMsg*>(user_data);
    if (msg->slot >= 0 && GetSavedSimSlot() != msg->slot) {
        SaveSimSlot(msg->slot);
        ESP_LOGI(TAG_HOME, "sim_slot synced from modem at boot: %d", msg->slot);
    }
    if (s_home_status != nullptr) {
        s_home_status->last_sim_slot = -1;
        UpdateHomeStatusBar(s_home_status);
    }
    delete msg;
}

void BootSimSlotQueryTask(void* /*arg*/) {
    auto* msg = new BootSimSlotQueryMsg{};
    Nt26Board* nt26 = GetNt26Board();
    if (nt26 != nullptr) {
        std::string resp;
        esp_err_t err = nt26->SendAtCommand("AT+ECSIMCFG?", resp, 5000,
                                            /*bypass_init_check=*/true);
        ESP_LOGI(TAG_HOME, "boot AT+ECSIMCFG? -> err=%d resp_len=%u",
                 (int)err, (unsigned)resp.size());
        if (err == ESP_OK && resp.find("OK") != std::string::npos) {
            const int slot = ParseSimSlotFromEcsimcfg(resp);
            if (slot == 0 || slot == 1) {
                msg->slot = slot;
            } else {
                ESP_LOGW(TAG_HOME, "boot ECSIMCFG: SimSlot not parsed, resp='%s'",
                         resp.c_str());
            }
        }
    }
    lv_async_call(AsyncBootSimSlotSynced, msg);
    vTaskDelete(nullptr);
}

void ScheduleBootSimSlotQuery() {
    if (s_boot_sim_slot_query_done) {
        return;
    }
    if (GetSavedNetworkType() != 1) {
        return;
    }
    if (GetNt26Board() == nullptr) {
        return;
    }
    s_boot_sim_slot_query_done = true;
    if (xTaskCreate(BootSimSlotQueryTask, "home_sim_q", 4096, nullptr, 5,
                    nullptr) != pdPASS) {
        s_boot_sim_slot_query_done = false;
        ESP_LOGE(TAG_HOME, "xTaskCreate(home_sim_q) failed");
    }
}

void UpdateHomeStatusBar(HomeStatusState* st) {
    if (st == nullptr || st->bar == nullptr) {
        return;
    }

    const int net_type = GetSavedNetworkType();
    if (st->network_type_lbl != nullptr) {
        lv_label_set_text(st->network_type_lbl, net_type == 1 ? "4G" : "WiFi");
    }

    // SIM 卡名称：4G 模式下显示「外置卡 / 内置卡」，WiFi 模式下整个标签隐藏。
    // 只在内容真正变化时更新，省一次 invalidate。
    if (st->sim_slot_lbl != nullptr) {
        if (net_type == 1) {
            const int slot = GetSavedSimSlot();
            if (st->last_net_type != net_type || st->last_sim_slot != slot) {
                lv_label_set_text(st->sim_slot_lbl,
                                  slot == 1 ? "内置卡" : "外置卡");
                st->last_sim_slot = slot;
            }
            lv_obj_remove_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (st->last_net_type != net_type) {
                lv_obj_add_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    st->last_net_type = net_type;

    const char* icon = Board::GetInstance().GetNetworkStateIcon();
    if (icon != nullptr && st->network_icon_lbl != nullptr &&
        icon != st->last_icon) {
        st->last_icon = icon;
        lv_label_set_text(st->network_icon_lbl, icon);
    }

    // ---- 电池 ----
#if HOME_STATUS_SHOW_BATTERY_ICON
    // 图标模式：仅 Font Awesome 电池图标，靠最右边
    if (st->battery_icon_lbl != nullptr) {
        int battery_level = 0;
        bool charging = false, discharging = false;
        if (Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging)) {
            if (battery_level < 0)   battery_level = 0;
            if (battery_level > 100) battery_level = 100;

            const char* bat_icon = nullptr;
            if (charging) {
                bat_icon = FONT_AWESOME_BATTERY_BOLT;
            } else if (battery_level >= 80) {
                bat_icon = FONT_AWESOME_BATTERY_FULL;
            } else if (battery_level >= 60) {
                bat_icon = FONT_AWESOME_BATTERY_THREE_QUARTERS;
            } else if (battery_level >= 40) {
                bat_icon = FONT_AWESOME_BATTERY_HALF;
            } else if (battery_level >= 20) {
                bat_icon = FONT_AWESOME_BATTERY_QUARTER;
            } else {
                bat_icon = FONT_AWESOME_BATTERY_EMPTY;
            }
            if (bat_icon != st->last_battery_icon) {
                st->last_battery_icon = bat_icon;
                lv_label_set_text(st->battery_icon_lbl, bat_icon);
            }

            const bool low = !charging && battery_level < 20;
            if (low != st->last_battery_low) {
                st->last_battery_low = low;
                uint32_t color = low ? 0xF87171 : 0xFFFFFF;
                lv_obj_set_style_text_color(st->battery_icon_lbl,
                                            lv_color_hex(color), LV_PART_MAIN);
            }
        } else {
            // 无电池：斜杠图标，恢复白色
            st->last_battery_icon = FONT_AWESOME_BATTERY_SLASH;
            lv_label_set_text(st->battery_icon_lbl, FONT_AWESOME_BATTERY_SLASH);
            if (st->last_battery_low) {
                st->last_battery_low = false;
                lv_obj_set_style_text_color(st->battery_icon_lbl,
                                            lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            }
        }
    }
#else
    // 文字模式：电量百分比 + 充电中 + 电压（原有行为）
    if (st->battery_pct_lbl != nullptr) {
        int battery_level = 0;
        bool charging = false, discharging = false;
        bool has_battery = Board::GetInstance().GetBatteryLevel(
            battery_level, charging, discharging);

        if (has_battery) {
            if (battery_level < 0)   battery_level = 0;
            if (battery_level > 100) battery_level = 100;

            const bool low = !charging && battery_level < 20;

            char buf[48];
            uint16_t dbg_mv = 0;
            char volt_str[16];
            if (Bq27220Gauge::GetInstance().GetVoltageMv(dbg_mv)) {
                std::snprintf(volt_str, sizeof(volt_str), "%.2fV",
                              dbg_mv / 1000.0f);
            } else {
                std::snprintf(volt_str, sizeof(volt_str), "--V");
            }
            if (charging) {
                std::snprintf(buf, sizeof(buf), "电量 %d%% 充电中 %s",
                              battery_level, volt_str);
            } else {
                std::snprintf(buf, sizeof(buf), "电量 %d%% %s",
                              battery_level, volt_str);
            }
            lv_label_set_text(st->battery_pct_lbl, buf);
            st->last_battery_pct = battery_level;

            if (low != st->last_battery_low) {
                st->last_battery_low = low;
                uint32_t color = low ? 0xF87171 : 0xFFFFFF;
                lv_obj_set_style_text_color(st->battery_pct_lbl,
                                            lv_color_hex(color), LV_PART_MAIN);
            }
        } else {
            if (st->last_battery_pct != -1) {
                st->last_battery_pct = -1;
                lv_label_set_text(st->battery_pct_lbl, "电量 --%");
            }
            if (st->last_battery_low) {
                st->last_battery_low = false;
                lv_obj_set_style_text_color(st->battery_pct_lbl,
                                            lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            }
        }
    }
#endif

    if (st->time_lbl != nullptr) {
        time_t now = time(nullptr);
        struct tm tm_info = {};
        if (localtime_r(&now, &tm_info) != nullptr &&
            tm_info.tm_year >= 2025 - 1900) {
            char time_str[16];
            strftime(time_str, sizeof(time_str), "%H:%M", &tm_info);
            lv_label_set_text(st->time_lbl, time_str);
        } else {
            lv_label_set_text(st->time_lbl, "--:--");
        }
    }

    if (st->activation_code_lbl != nullptr) {
        auto& app = Application::GetInstance();
        if (app.HasPendingActivation()) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "验证码: %s",
                          app.GetPendingActivationCode().c_str());
            if (st->last_activation_text != buf) {
                st->last_activation_text = buf;
                lv_label_set_text(st->activation_code_lbl, buf);
            }
            if (!st->last_activation_visible) {
                st->last_activation_visible = true;
                lv_obj_remove_flag(st->activation_code_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (st->last_activation_visible) {
            st->last_activation_visible = false;
            st->last_activation_text.clear();
            lv_obj_add_flag(st->activation_code_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void OnHomeStatusTimer(lv_timer_t* timer) {
    UpdateHomeStatusBar(static_cast<HomeStatusState*>(lv_timer_get_user_data(timer)));
}

void OnHomeStatusDeleted(lv_event_t* e) {
    auto* st = static_cast<HomeStatusState*>(lv_event_get_user_data(e));
    if (st == nullptr) {
        return;
    }
    if (s_home_status == st) {
        s_home_status = nullptr;
    }
    if (st->update_timer != nullptr) {
        lv_timer_delete(st->update_timer);
        st->update_timer = nullptr;
    }
    delete st;
}

lv_obj_t* CreateStatusBar(lv_obj_t* screen, HomeStatusState* st) {
    // 左右两块容器宽度直接锁死，避免 LVGL flex 在 SIZE_CONTENT + SPACE_BETWEEN
    // 组合下对内容宽度的二次评估把右边「电量 100% 充电中」末尾几个字裁掉。
    //   - 左侧 300px：足够放下「图标 + 4G + 外置卡」
    //   - 右侧 400px：「电量 100% 充电中 HH:MM」是 ~18 字宽，预留充分
    //   - 中间空隙由父 flex SPACE_BETWEEN 自动撑开
    constexpr int kStatusLeftWidth  = 300;
    constexpr int kStatusRightWidth = 400;

    lv_obj_t* bar = lv_obj_create(screen);
    st->bar = bar;
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, kPanelSize, kStatusBarHeight);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kStatusBarBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bar, 8, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* left = lv_obj_create(bar);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, kStatusLeftWidth, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 10, LV_PART_MAIN);

    st->network_icon_lbl = lv_label_create(left);
    lv_label_set_text(st->network_icon_lbl, FONT_AWESOME_WIFI);
    lv_obj_set_style_text_font(st->network_icon_lbl, &font_awesome_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->network_icon_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);

    st->network_type_lbl = lv_label_create(left);
    lv_label_set_long_mode(st->network_type_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->network_type_lbl, LV_SIZE_CONTENT);
    lv_label_set_text(st->network_type_lbl, "WiFi");
    lv_obj_set_style_text_font(st->network_type_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->network_type_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);

    // SIM 卡标签（外置卡 / 内置卡），用稍浅一些的灰色与「4G」做视觉区分。
    // 默认隐藏，UpdateHomeStatusBar 会根据当前网络类型决定显隐。
    st->sim_slot_lbl = lv_label_create(left);
    lv_label_set_long_mode(st->sim_slot_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->sim_slot_lbl, LV_SIZE_CONTENT);
    lv_label_set_text(st->sim_slot_lbl, "");
    lv_obj_set_style_text_font(st->sim_slot_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->sim_slot_lbl, lv_color_hex(0xC9D1D9),
                                LV_PART_MAIN);
    lv_obj_add_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);

    // ---- 状态栏右侧：电池电量文字（时间单独居中浮在状态栏正中） ----
    // 故意不再放 font_awesome 电池图标，避免字符渲染不到 / 占位问题。
    // 右容器宽度固定 400px，靠右排电量文字，这样无论文本变长
    // （"电量 100% 充电中"）还是变短（"电量 --%"）都不会被父级 flex
    // 重排时挤压裁切。
    lv_obj_t* right = lv_obj_create(bar);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, kStatusRightWidth, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
#if HOME_STATUS_SHOW_BATTERY_ICON
    // 图标模式：只放一个 Font Awesome 电池图标，靠最右边
    st->battery_icon_lbl = lv_label_create(right);
    lv_label_set_text(st->battery_icon_lbl, FONT_AWESOME_BATTERY_FULL);
    lv_obj_set_style_text_font(st->battery_icon_lbl, &font_awesome_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->battery_icon_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
#else
    // 文字模式：电量百分比 + 充电中 + 电压（原有行为）
    lv_obj_set_style_pad_column(right, 14, LV_PART_MAIN);

    st->battery_pct_lbl = lv_label_create(right);
    lv_label_set_long_mode(st->battery_pct_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->battery_pct_lbl, 380);
    lv_label_set_text(st->battery_pct_lbl, "电量 --%");
    lv_obj_set_style_text_align(st->battery_pct_lbl, LV_TEXT_ALIGN_RIGHT,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(st->battery_pct_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->battery_pct_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
#endif

    // 时间 + 激活码居中：FLOATING 容器脱离 bar 的 flex 布局，整体锚在
    // 状态栏正中央；未激活时激活码显示在时间右侧，激活成功后隐藏。
    lv_obj_t* center = lv_obj_create(bar);
    lv_obj_add_flag(center, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_style_all(center);
    lv_obj_set_size(center, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(center, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(center, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(center, 8, LV_PART_MAIN);
    lv_obj_align(center, LV_ALIGN_CENTER, 0, 0);

    st->time_lbl = lv_label_create(center);
    lv_label_set_long_mode(st->time_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->time_lbl, 80);
    lv_label_set_text(st->time_lbl, "--:--");
    lv_obj_set_style_text_align(st->time_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(st->time_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->time_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    st->activation_code_lbl = lv_label_create(center);
    lv_label_set_long_mode(st->activation_code_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->activation_code_lbl, LV_SIZE_CONTENT);
    lv_label_set_text(st->activation_code_lbl, "");
    lv_obj_set_style_text_font(st->activation_code_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->activation_code_lbl, lv_color_hex(0xFBBF24),
                                LV_PART_MAIN);
    lv_obj_add_flag(st->activation_code_lbl, LV_OBJ_FLAG_HIDDEN);

    UpdateHomeStatusBar(st);
    st->update_timer = lv_timer_create(OnHomeStatusTimer, 1000, st);
    s_home_status = st;
    ScheduleBootSimSlotQuery();

    lv_obj_add_event_cb(screen, OnHomeStatusDeleted, LV_EVENT_DELETE, st);
    return bar;
}

void HighlightDot(PagerState* state, int page) {
    if (page < 0 || page >= state->page_count) {
        return;
    }
    state->current_page = page;
    for (int i = 0; i < state->page_count; ++i) {
        // Active dot is fully opaque, idle dots are subtle.  We keep both
        // the color the same so the row reads as a connected element.
        lv_opa_t opa = (i == page) ? LV_OPA_COVER : LV_OPA_40;
        lv_obj_set_style_bg_opa(state->dots[i], opa, LV_PART_MAIN);
    }
}

// 翻页：把 pager 直接跳到目标页（LV_ANIM_OFF 立刻定位，无过渡动画）。
void GoToPage(PagerState* state, int target_page) {
    if (state == nullptr || state->pager == nullptr) {
        return;
    }
    if (target_page < 0 || target_page >= state->page_count) {
        return;
    }
    if (target_page == state->current_page) {
        return;
    }
    lv_obj_scroll_to_x(state->pager, target_page * kPanelSize, LV_ANIM_OFF);
    HighlightDot(state, target_page);
    s_last_home_page = target_page;
    ResetHomeIdleTimer();
}

// ---------------------------------------------------------------------------
// 触摸会话：PRESSED 锁定目标 → PRESSING/RELEASED 判滑动 → RELEASED 判单击/长按
//
//   |dx|、|dy| 均 < kHomeMoveThreshold：
//     按下→松开 < kHomeLongPressMs → Click（缩放 + launch）
//     按下→松开 ≥ kHomeLongPressMs → LongPress（仅缩放，预留）
//   |dx| >= |dy| 且 peak >= kHomeMoveThreshold → 左右滑（PRESSING 或 RELEASED 翻页）
//   |dx| <  |dy| 且 peak >= kHomeMoveThreshold → 上下滑（预留，无动作）
// ---------------------------------------------------------------------------

bool HomeTouchIsTapLike(int dx, int dy) {
    return std::abs(dx) < kHomeMoveThreshold && std::abs(dy) < kHomeMoveThreshold;
}

HomeTouchKind HomeTouchClassifySwipe(int dx, int dy) {
    const int adx = std::abs(dx);
    const int ady = std::abs(dy);
    const int peak = std::max(adx, ady);
    if (peak < kHomeMoveThreshold) {
        return HomeTouchKind::None;
    }
    if (adx >= ady) {
        return dx < 0 ? HomeTouchKind::SwipeLeft : HomeTouchKind::SwipeRight;
    }
    return dy < 0 ? HomeTouchKind::SwipeUp : HomeTouchKind::SwipeDown;
}

void HomeTouchHandleSwipe(PagerState* state, HomeTouchKind kind) {
    if (state == nullptr) {
        return;
    }
    switch (kind) {
        case HomeTouchKind::SwipeLeft:
            GoToPage(state, state->current_page + 1);
            break;
        case HomeTouchKind::SwipeRight:
            GoToPage(state, state->current_page - 1);
            break;
        case HomeTouchKind::SwipeUp:
        case HomeTouchKind::SwipeDown:
            break;
        default:
            break;
    }
}

void HomeTouchDispatchTapLike(HomeTouchKind kind) {
    if (s_home_touch.press_cell == nullptr) {
        return;
    }
    if (kind == HomeTouchKind::Click) {
        if (s_home_touch.app == nullptr) {
            return;
        }
        PlayAppCellPressScale(s_home_touch.press_cell, s_home_touch.app);
        return;
    }
    if (kind == HomeTouchKind::LongPress) {
        PlayAppCellPressScale(s_home_touch.press_cell, nullptr);
    }
}

void HomeTouchTrySwipeDuringPress(PagerState* state, int dx, int dy) {
    if (s_home_touch.consumed) {
        return;
    }
    const HomeTouchKind kind = HomeTouchClassifySwipe(dx, dy);
    if (kind == HomeTouchKind::None) {
        return;
    }
    s_home_touch.consumed = true;
    HomeTouchHandleSwipe(state, kind);
}

void OnHomePressed(lv_event_t* e) {
    if (s_home_touch.active) {
        return;
    }
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_obj_t* screen = lv_event_get_current_target_obj(e);
    lv_obj_t* cell = FindAppCellFromTarget(lv_event_get_target_obj(e), screen);

    s_home_touch.active = true;
    s_home_touch.consumed = false;
    s_home_touch.start_x = static_cast<int16_t>(p.x);
    s_home_touch.start_y = static_cast<int16_t>(p.y);
    s_home_touch.press_tick = lv_tick_get();
    s_home_touch.press_cell = cell;
    s_home_touch.app =
        cell != nullptr
            ? static_cast<const AppEntry*>(lv_obj_get_user_data(cell))
            : nullptr;
    ResetHomeIdleTimer();
}

void OnHomePressing(lv_event_t* e) {
    if (!s_home_touch.active || s_home_touch.consumed) {
        return;
    }
    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const int dx = p.x - s_home_touch.start_x;
    const int dy = p.y - s_home_touch.start_y;
    if (!HomeTouchIsTapLike(dx, dy)) {
        ResetHomeIdleTimer();
    }
    HomeTouchTrySwipeDuringPress(state, dx, dy);
}

void OnHomeReleased(lv_event_t* e) {
    if (!s_home_touch.active) {
        return;
    }

    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));

    if (!s_home_touch.consumed) {
        lv_indev_t* indev = lv_event_get_indev(e);
        int dx = 0;
        int dy = 0;
        if (indev != nullptr) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            dx = p.x - s_home_touch.start_x;
            dy = p.y - s_home_touch.start_y;
        }

        const uint32_t elapsed = lv_tick_elaps(s_home_touch.press_tick);
        if (HomeTouchIsTapLike(dx, dy)) {
            const HomeTouchKind kind =
                elapsed < kHomeLongPressMs ? HomeTouchKind::Click
                                           : HomeTouchKind::LongPress;
            HomeTouchDispatchTapLike(kind);
        } else {
            const HomeTouchKind kind = HomeTouchClassifySwipe(dx, dy);
            if (state != nullptr && kind != HomeTouchKind::None) {
                HomeTouchHandleSwipe(state, kind);
            }
        }
    }

    s_home_touch.active = false;
    s_home_touch.consumed = false;
    s_home_touch.press_cell = nullptr;
    s_home_touch.app = nullptr;
}

// 递归在子树上打 LV_OBJ_FLAG_EVENT_BUBBLE，让 cell / icon 上的 PRESSED /
// PRESSING / RELEASED 都能冒泡到屏幕被上面三个 handler 接住。
void EnableHomeEventBubble(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        EnableHomeEventBubble(lv_obj_get_child(obj, i));
    }
}

void OnHomeScreenLoaded(lv_event_t* e) {
    lv_obj_t* scr = lv_event_get_current_target_obj(e);
    EnableHomeEventBubble(scr);

    // 屏幕加载完成、layout 已经算好，这时再把 pager 滚到「上次停留的页」。
    // 直接在 Create() 末尾 scroll_to_x 经常因为 layout 还没算赶不上首帧。
    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));
    if (state != nullptr && state->pager != nullptr &&
        s_last_home_page > 0 && s_last_home_page < state->page_count) {
        lv_obj_update_layout(state->pager);
        lv_obj_scroll_to_x(state->pager, s_last_home_page * kPanelSize,
                           LV_ANIM_OFF);
        HighlightDot(state, s_last_home_page);
    }

    StartHomeIdleTimer();
}

void OnScreenDeleted(lv_event_t* e) {
    CancelCellScaleTimer();
    StopHomeIdleTimer();
    delete static_cast<PagerState*>(lv_event_get_user_data(e));
}

// ---------------------------------------------------------------------------
// 电源对话框 (PWR_KEY 长按 2s 触发)
//
// 长按 PWR_KEY 满 2 秒弹出居中模态框，里面两个大按钮：重启 / 关机。
// 点击空白处（mask 自身，不在 card 区域内）关闭对话框。
//
// 线程模型：
//   - IOExpander 在它自己的 monitor task 里检测到长按后通过 lv_async_call
//     把 ShowPowerDialog 调度到 LVGL 线程执行。
//   - 关机操作要发 PWR_KEY_PULSE 序列（10ms 高 / 10ms 低 × 10 次），需要
//     vTaskDelay 级别的延迟精度，所以单开一个 FreeRTOS task 在那里完成
//     脉冲后自删除；不阻塞 LVGL 线程，也不污染 IOExpander 的 monitor。
//
// EVENT_BUBBLE 默认关闭，所以 card / 按钮上的点击不会冒泡到 mask 触发
// 误关闭；为了给后续修改做兜底，OnPwrMaskClicked 里再做一次 target 校验。
// ---------------------------------------------------------------------------
struct PowerDialogUi {
    lv_obj_t* mask = nullptr;
    lv_obj_t* card = nullptr;
};

PowerDialogUi s_pwr_dlg;

void ClosePowerDialog() {
    if (s_pwr_dlg.mask != nullptr) {
        lv_obj_delete(s_pwr_dlg.mask);
    }
    s_pwr_dlg = PowerDialogUi{};
}

void OnPwrMaskClicked(lv_event_t* e);  // forward decl

lv_obj_t* s_shutdown_screen = nullptr;

void AppendShutdownProgressContent(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_row(box, 24, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* spin = lv_spinner_create(box);
    lv_obj_set_size(spin, 140, 140);
    lv_spinner_set_anim_params(spin, 1000, 200);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_INDICATOR);
    lv_obj_remove_flag(spin, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(box);
    lv_label_set_text(lbl, "正在关机...");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* CreateShutdownScreen() {
    lv_obj_t* screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    AppendShutdownProgressContent(screen);
    return screen;
}

// 切到独立黑底关机关机页，避免在原页面半透明遮罩上显示。
void ShowShutdownScreen() {
    ClosePowerDialog();

    if (s_shutdown_screen == nullptr) {
        s_shutdown_screen = CreateShutdownScreen();
    }
    lv_screen_load(s_shutdown_screen);
}

// 关机脉冲 task：高 10ms / 低 10ms × 10 次。跑完自删除，不返回。
// 如果硬件确实在脉冲过程中切电，task 会被电源切断带走，无需特别处理。
void PwrShutdownPulseTask(void* /*arg*/) {
    auto& io = IOExpander::getInstance();
    constexpr int kPulseHalfMs = 100;
    constexpr int kPulseCount  = 10;
    for (int i = 0; i < kPulseCount; ++i) {
        io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, true);
        vTaskDelay(pdMS_TO_TICKS(kPulseHalfMs));
        io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, false);
        vTaskDelay(pdMS_TO_TICKS(kPulseHalfMs));
    }
    vTaskDelete(NULL);
}

void BeginSystemShutdown(const char* reason) {
    static bool shutting_down = false;
    if (shutting_down) {
        return;
    }
    shutting_down = true;

    ESP_LOGW(TAG_HOME, "%s：开始 PWR_KEY_PULSE 脉冲序列", reason);
    StopHomeIdleTimer();
    ShowShutdownScreen();
    xTaskCreate(PwrShutdownPulseTask, "pwr_off_pulse", 2048, nullptr, 5, nullptr);
}

void OnPwrShutdownClicked(lv_event_t* /*e*/) {
    BeginSystemShutdown("用户选择 [关机]");
}

void OnPwrRebootClicked(lv_event_t* /*e*/) {
    ESP_LOGW(TAG_HOME, "用户选择 [重启]：调用 Application::Reboot()");
    ClosePowerDialog();
    Application::GetInstance().Reboot();
}

void OnPwrMaskClicked(lv_event_t* e) {
    // 只处理「点在 mask 自身」的事件。card / 按钮的点击因为 EVENT_BUBBLE
    // 默认关闭根本不会冒泡到这里；这里再做一道 target 校验是为了万一以后
    // 谁动了 EVENT_BUBBLE 标志也不会误关。
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) {
        return;
    }
    ESP_LOGI(TAG_HOME, "点击模态框外，关闭电源对话框");
    ClosePowerDialog();
}

lv_obj_t* CreatePowerActionBtn(lv_obj_t* parent,
                               const char* icon_src,
                               const char* text,
                               lv_event_cb_t on_click) {
    constexpr int kBtnSize     = 180;
    constexpr int kBtnIconSize = 96;

    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kBtnSize, kBtnSize);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 24, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* icon = lv_image_create(btn);
    lv_image_set_src(icon, icon_src);
    lv_obj_set_size(icon, kBtnIconSize, kBtnIconSize);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_pad_top(lbl, 12, LV_PART_MAIN);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, nullptr);
    return btn;
}

void ShowPowerDialog() {
    if (s_pwr_dlg.mask != nullptr) {
        return;  // 已经打开
    }
    lv_obj_t* parent = lv_screen_active();
    if (parent == nullptr) {
        return;
    }

    // ---- 全屏遮罩 ----
    // FLOATING：让 mask 脱离父屏的 flex / grid 布局。
    // 比如 gps_screen 在根对象上挂了 LV_FLEX_FLOW_COLUMN，没有这个 flag
    // 时 mask 会被父屏的 flex 接管 —— `lv_obj_set_pos(0,0)` 会被布局覆盖
    // 重排到列尾，加上 720x720 比屏幕还大就只能看到底部一截。挂了
    // FLOATING 后 mask 完全被父布局忽略，set_pos 重新生效，dialog 在任何
    // 屏幕上都能正确居中铺满。
    lv_obj_t* mask = lv_obj_create(parent);
    lv_obj_remove_style_all(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelSize, kPanelSize);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    lv_obj_add_event_cb(mask, OnPwrMaskClicked, LV_EVENT_CLICKED, nullptr);
    s_pwr_dlg.mask = mask;

    // ---- 中心卡片 ----
    constexpr int kCardW = 480;
    constexpr int kCardH = 360;
    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // card 必须 clickable —— 这样点中 card 范围内的事件被 card 自己消费，
    // 不会落到 mask 触发关闭逻辑。
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    s_pwr_dlg.card = card;

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "电源选项");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    // ---- 按钮行 ----
    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 32, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    CreatePowerActionBtn(row, "A:ic_s_home_reboot.spng", "重启",
                         OnPwrRebootClicked);
    CreatePowerActionBtn(row, "A:ic_s_home_power.spng", "关机",
                         OnPwrShutdownClicked);

    // ---- 底部提示：硬件强制关机说明 ----
    lv_obj_t* hint = lv_label_create(card);
    lv_label_set_text(hint, "长按关机键 5 秒可强制关机");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9CA3AF), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);
}

// IOExpander 的 monitor task 里调用此函数 —— 必须 lv_async_call 切到
// LVGL 线程才能安全 ShowPowerDialog。
void OnPwrLongPressAsync(void* /*arg*/) { ShowPowerDialog(); }

void RegisterPowerLongPress() {
    static bool registered = false;
    if (registered) {
        return;
    }
    const esp_err_t err = IOExpander::getInstance().onLongPress(
        IOExpander::Pin::PWR_KEY, 1500,
        []() { lv_async_call(OnPwrLongPressAsync, nullptr); });
    if (err == ESP_OK) {
        registered = true;
        ESP_LOGI(TAG_HOME,
                 "PWR_KEY 长按 1.5s 已注册：弹出 [重启 / 关机] 对话框");
    } else {
        ESP_LOGE(TAG_HOME, "PWR_KEY 长按注册失败: 0x%x", err);
    }
}

lv_obj_t* CreatePage(lv_obj_t* pager, int page_index, int total_apps) {
    lv_obj_t* page = lv_obj_create(pager);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, kPanelSize, kPagerHeight);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(page, kPagePadHor, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(page, kPagePadVer, LV_PART_MAIN);
    lv_obj_set_style_pad_column(page, kGridColGap, LV_PART_MAIN);
    lv_obj_set_style_pad_row(page, kGridRowGap, LV_PART_MAIN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    // Fixed 3x3 grid -- each app sits in its natural (col, row) slot so an
    // under-filled page (e.g. a single app on page 2) anchors top-left
    // instead of getting visually centered by a flex space-distribute.
    lv_obj_set_grid_dsc_array(page, s_col_dsc, s_row_dsc);
    lv_obj_set_layout(page, LV_LAYOUT_GRID);

    const int start = page_index * kAppsPerPage;
    for (int i = 0; i < kAppsPerPage; ++i) {
        const int idx = start + i;
        if (idx >= total_apps)
            break;
        const AppEntry& app = kApps[idx];
        if (app.icon_suffix == nullptr)
            continue;
        lv_obj_t* cell = CreateAppCell(page, app, idx);
        const int col = i % kPageCols;
        const int row = i / kPageCols;
        lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
    }
    return page;
}

void CreateIndicator(lv_obj_t* screen, PagerState* state) {
    // The pill-shaped capsule under the dots gives the indicator enough
    // contrast against any wallpaper / page colour without competing for
    // attention.  It only shows when there are 2+ pages.
    lv_obj_t* indicator = lv_obj_create(screen);
    lv_obj_remove_style_all(indicator);
    lv_obj_set_size(indicator, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(indicator, LV_ALIGN_BOTTOM_MID, 0, -kIndicatorYOffset);
    lv_obj_set_style_bg_color(indicator, lv_color_hex(kIndicatorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_40, LV_PART_MAIN);  // ~40% black
    lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(indicator, kIndicatorPadHor, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(indicator, kIndicatorPadVer, LV_PART_MAIN);
    lv_obj_set_style_pad_column(indicator, kDotGap, LV_PART_MAIN);
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(indicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicator, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    // The indicator is purely decorative -- touch events fall through to
    // the pager underneath so the user can grab it to swipe pages.
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < state->page_count; ++i) {
        lv_obj_t* dot = lv_obj_create(indicator);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, kDotSize, kDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(kDotColor), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_40, LV_PART_MAIN);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        state->dots[i] = dot;
    }

    HighlightDot(state, 0);
}

}  // namespace

lv_obj_t* HomeScreen::Create() {
    // PWR_KEY 长按 2s 触发 [重启 / 关机] 模态框。RegisterPowerLongPress 内部
    // 用 static bool 守护：第一次构建主屏时挂上回调，后续从子页面返回再
    // 重建主屏不会重复注册。
    RegisterPowerLongPress();

    // 主题相关：根据 NVS 里的当前主题 id 把 kApps 的 icon_suffix 拼成完整
    // 路径，写入 s_icon_paths 缓存。后续 CreateAppCell 直接索引这份缓存。
    EnsureIconPathsBuilt();

    lv_obj_t* screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // ----- Figure out how many pages we need -----
    // kTotalApps 已经在 namespace 作用域里声明，直接复用。
    int page_count = (kTotalApps + kAppsPerPage - 1) / kAppsPerPage;
    if (page_count < 1)
        page_count = 1;
    if (page_count > kMaxPages)
        page_count = kMaxPages;

    // PagerState owns the dot pointers + current_page; freed on screen del.
    auto* state = new PagerState{};
    state->page_count = page_count;
    state->current_page = 0;

    auto* status = new HomeStatusState{};
    CreateStatusBar(screen, status);

    // ----- Pager (无动画翻页容器) -----
    // 关掉 LVGL 自带的横向 scroll-snap，避免拖动时出现「跟手 + 回弹」过渡。
    // 翻页由 OnHomeGesture 监听水平手势后用 lv_obj_scroll_to_x(..., LV_ANIM_OFF)
    // 直接定位到目标页，呈现「啪」一下切换的瞬时效果。
    lv_obj_t* pager = lv_obj_create(screen);
    state->pager = pager;
    lv_obj_remove_style_all(pager);
    lv_obj_set_size(pager, kPanelSize, kPagerHeight);
    lv_obj_align(pager, LV_ALIGN_TOP_LEFT, 0, kStatusBarHeight);
    lv_obj_set_style_bg_opa(pager, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(pager, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(pager, LV_SCROLLBAR_MODE_OFF);
    // Row flex；每页固定宽度并列排布，pager 的 scroll 偏移由我们手动控制。
    lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pager, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int p = 0; p < page_count; ++p) {
        CreatePage(pager, p, kTotalApps);
    }

    // ----- Page indicator -----
    // Only worth drawing when there is more than one page; otherwise it's
    // a lonely single dot which just adds noise.
    if (page_count > 1) {
        CreateIndicator(screen, state);
    }

    // ----- 触摸：screen 级 PRESSED / PRESSING / RELEASED 统一分类分发 -----
    // LV_EVENT_SCREEN_LOADED 后再铺 EVENT_BUBBLE，让子树按压事件冒泡到 screen。
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, OnHomePressed, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(screen, OnHomePressing, LV_EVENT_PRESSING, state);
    lv_obj_add_event_cb(screen, OnHomeReleased, LV_EVENT_RELEASED, state);
    lv_obj_add_event_cb(screen, OnHomeScreenLoaded, LV_EVENT_SCREEN_LOADED, state);
    lv_obj_add_event_cb(screen, OnScreenDeleted, LV_EVENT_DELETE, state);

    return screen;
}

void OnRefreshStatusBarAsync(void* /*user_data*/) {
    if (s_home_status != nullptr) {
        UpdateHomeStatusBar(s_home_status);
    }
}

void HomeScreen::ResetToFirstPage() {
    s_last_home_page = 0;
}

void HomeScreen::RefreshStatusBar() {
    // Application::CheckNewVersion / ShowActivationCode 跑在 app_main
    // 线程，不能直接碰 LVGL；切到 LVGL 线程再刷新，避免与 adapter 争用锁
    // 导致 lv_obj_invalidate 在 Core0 上死循环占满 CPU。
    lv_async_call(OnRefreshStatusBarAsync, nullptr);
}
