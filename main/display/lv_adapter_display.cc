#include "lv_adapter_display.h"

#include <cstring>
#include <memory>

#include <esp_lcd_panel_io.h>
#include <esp_log.h>

#include "esp_lv_adapter.h"
#include "esp_lv_fs.h"
#include "esp_mmap_assets.h"
#include "touch_feed.h"   

#include "mmap_generate_resources.h"

#include "screen/boot_screen/boot_screen.h"
#include "screen/chat_screen/chat_screen.h"
#include "screen/digital_people_screen/digital_people_screen.h"
#include "screen/home_screen/home_screen.h"

#include "application.h"

static const char* TAG = "LVAdapterDisplay";

namespace {

// ---------------------------------------------------------------------------
// 表情大类映射
//
// 服务器 / LLM 返回的细分表情多达 21 种，但端侧 SD 卡只准备了 6 个大类的
// .eaf 动画（crying / happy / loving / neutral / surprised / thinking）。
// 这里把细分名收敛到大类代表，再交给 DigitalPeopleScreen 拼路径加载。
// 表中没收录的（或 nullptr）一律 fallback 到 neutral，保证永远有动画播放。
// ---------------------------------------------------------------------------
struct EmoteCategoryEntry {
    const char* emote;     // 细分表情名
    const char* category;  // 所属大类（代表表情名）
};

constexpr EmoteCategoryEntry kEmoteCategoryMap[] = {
    // 开心类 -> happy
    {"happy",       "happy"},
    {"laughing",    "happy"},
    {"funny",       "happy"},
    {"silly",       "happy"},
    {"winking",     "happy"},
    {"cool",        "happy"},
    {"confident",   "happy"},
    // 爱意类 -> loving
    {"loving",      "loving"},
    {"kissy",       "loving"},
    {"delicious",   "loving"},
    // 悲伤 / 负面类 -> crying（6 大类里用 crying 这个名字而不是 sad）
    {"sad",         "crying"},
    {"crying",      "crying"},
    {"angry",       "crying"},
    // 惊讶类 -> surprised
    {"surprised",   "surprised"},
    {"shocked",     "surprised"},
    {"embarrassed", "surprised"},
    // 思考类 -> thinking
    {"thinking",    "thinking"},
    {"confused",    "thinking"},
    // 平静类 -> neutral
    {"neutral",     "neutral"},
    {"relaxed",     "neutral"},
    {"sleepy",      "neutral"},
};

// 输入任一细分表情名，返回所属大类代表名；找不到时返回 "neutral"。
// 表大小固定 < 32，O(N) 线性比较完全够用。
const char* GetEmoteCategory(const char* emote) {
    if (emote == nullptr) return "neutral";
    for (const auto& e : kEmoteCategoryMap) {
        if (std::strcmp(e.emote, emote) == 0) {
            return e.category;
        }
    }
    return "neutral";
}

}  // namespace

LVAdapterDisplay::LVAdapterDisplay(const esp_lcd_panel_handle_t panel,
                                   const esp_lcd_panel_io_handle_t panel_io,
                                   const esp_lcd_touch_handle_t touch_handle, const int width,
                                   const int height) {
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.stack_in_psram = true;
    adapter_cfg.task_priority = 1;
    adapter_cfg.task_core_id = 1;

    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    // 性能调优要点（720x720 RGB565 屏）：
    //   - enable_ppa_accel: 打开 ESP32-P4 内置 PPA 硬件单元，专门做 alpha
    //     混合 / 像素格式转换 / 旋转，过去全靠软件遍历像素。开了之后，主屏
    //     滑动翻页这种大面积半透明叠加最受益（用户场景里 9 个磁贴 × 20%
    //     alpha + 圆角 + 文字）。
    //   - tear_avoid_mode = TRIPLE_FULL：直接把 LCD 驱动里 num_fbs=3 的 3 张
    //     panel 帧缓冲（PSRAM 上 3×720×720×2 ≈ 3MB）当成 LVGL 的 draw buffer
    //     用，渲染→DMA 三级流水，无撕裂。
    //     之前用 DEFAULT_MIPI_DSI（= TRIPLE_PARTIAL）会额外要一块
    //     720×buffer_height×2 ≈ 280KB 的内部 SRAM partial buffer，而片上 SRAM
    //     被 FreeRTOS / WiFi / SDIO 吃掉后根本剩不下，导致启动日志里报
    //     「alloc partial draw buffer failed」+「tear mode 4 setup failed」，
    //     adapter 还会再 fallback 申请 ~576KB PSRAM 当双缓冲，3MB+576KB 双重
    //     浪费。TRIPLE_FULL 彻底避开这条 fallback 路径。
    //   - buffer_height / require_double_buffer 在 TRIPLE_FULL 模式下不再生效
    //     （buffer 直接用 panel FB），保留是为了将来切回 partial 模式方便。
    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = panel_io,
        .profile =
            {
                .interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI,
                .hor_res = static_cast<uint16_t>(width),
                .ver_res = static_cast<uint16_t>(height),
                .buffer_height = 200,
                .use_psram = true,
                .enable_ppa_accel = true,
                .require_double_buffer = true,
            },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_FULL,
    };

    lv_display_t* disp = esp_lv_adapter_register_display(&disp_cfg);
    esp_lv_adapter_touch_config_t touch_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_handle);
    lv_indev_t* touch_indev = esp_lv_adapter_register_touch(&touch_cfg);
    touch_feed_init(touch_handle, 20);
    touch_feed_attach_indev(touch_indev);

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    mmap_assets_handle_t assets;
    const mmap_assets_config_t mmap_cfg = {
        .partition_label = "resources",
        .max_files = MMAP_RESOURCES_FILES,
        .checksum = MMAP_RESOURCES_CHECKSUM,
        .flags = {.mmap_enable = true},
    };
    ESP_ERROR_CHECK(mmap_assets_new(&mmap_cfg, &assets));

    esp_lv_fs_handle_t fs_handle;
    const fs_cfg_t fs_cfg = {
        .fs_letter = 'A',
        .fs_nums = MMAP_RESOURCES_FILES,
        .fs_assets = assets,
    };
    ESP_ERROR_CHECK(esp_lv_adapter_fs_mount(&fs_cfg, &fs_handle));

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        SetupUI();
        esp_lv_adapter_unlock();
    }

    // Application::GetInstance().ForceReturnToIdle();
}

void LVAdapterDisplay::SetupUI() {
    lv_obj_t* boot_scr = BootScreen::Create();
    lv_screen_load(boot_scr);

    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            lv_obj_t* old_scr = lv_screen_active();

            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                lv_obj_t* home_scr = HomeScreen::Create();
                lv_screen_load(home_scr);
                if (old_scr != NULL && old_scr != home_scr) {
                    lv_obj_delete(old_scr);
                }
                esp_lv_adapter_unlock();
            }

            lv_timer_delete(t);
        },
        2000, nullptr);
    lv_timer_set_repeat_count(timer, 1);
}

LVAdapterDisplay::~LVAdapterDisplay() = default;

void LVAdapterDisplay::SetEmotion(const char* const emotion) {
    // 1) 把细分表情名映射到 6 个大类之一（兜底 neutral）
    const char* category = GetEmoteCategory(emotion);
    ESP_LOGI(TAG, "SetEmotion: %s -> %s",
             emotion != nullptr ? emotion : "<null>", category);

    // 2) 转交给数字人屏。这把锁和 SetChatMessage 共用，确保 lv_eaf_set_src
    //    与屏幕生命周期事件互斥。屏幕不在前台时 DigitalPeopleScreen::SetEmotion
    //    也只是更新静态缓存，等下一次 Create() 时再加载，所以这里
    //    无需判断 IsActive()，无条件转发即可。
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    DigitalPeopleScreen::SetEmotion(category);
    esp_lv_adapter_unlock();
}

void LVAdapterDisplay::SetChatMessage(const char* const role, const char* const content) {
    if (role == nullptr || content == nullptr || content[0] == '\0') {
        return;
    }

    // role 归一化：
    //   user             -> 用户发言
    //   assistant/system -> 设备 / AI 回应
    const bool is_user = (std::strcmp(role, "user") == 0);
    const bool is_bot  = (std::strcmp(role, "assistant") == 0 ||
                          std::strcmp(role, "system") == 0);
    if (!is_user && !is_bot) {
        return;
    }

    // 路由策略：
    //   1) 聊天屏在前台 -> 历史滚动气泡（双侧）。
    //   2) 数字人屏在前台 -> user 走底部气泡，bot 走 gif 左上方气泡。
    //   3) 其它屏 -> 直接丢弃，避免在后台无界堆积。
    const bool chat_active = ChatScreen::IsActive();
    const bool dp_active   = DigitalPeopleScreen::IsActive();
    if (!chat_active && !dp_active) {
        return;
    }

    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (chat_active) {
        ChatScreen::AddMessage(content,
                               is_user ? ChatMsgDir::Right : ChatMsgDir::Left);
    } else {
        if (is_user) {
            DigitalPeopleScreen::ShowUserMessage(content);
        } else {
            DigitalPeopleScreen::ShowSystemMessage(content);
        }
    }
    esp_lv_adapter_unlock();
}

void LVAdapterDisplay::SetStatus(const char* const status) {}

void LVAdapterDisplay::ShowNotification(const char* notification, int duration_ms) {}

void LVAdapterDisplay::UpdateStatusBar(bool update_all) {}

void LVAdapterDisplay::SetPowerSaveMode(bool on) {}

void LVAdapterDisplay::SetPreviewImage(const void* image) {}

void LVAdapterDisplay::SetTheme(Theme* const theme) { ESP_LOGI(TAG, "SetTheme: %p", theme); }

bool LVAdapterDisplay::Lock(const int timeout_ms) { return true; }

void LVAdapterDisplay::Unlock() {}
