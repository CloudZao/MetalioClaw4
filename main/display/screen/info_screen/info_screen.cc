#include "info_screen.h"

#include <cstdio>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_psram.h>
#include <soc/soc.h>

#include "board.h"
#include "home_screen/home_screen.h"
#include "screen_util.h"
#include "system_info.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "InfoScreen";

constexpr int kPanelSize = 720;
constexpr int kHeaderH = 90;
constexpr int kBackBtnSize = 72;
constexpr int kPad = 16;
constexpr int kRowH = 72;
constexpr int kRowGap = 8;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorCard = 0x1B2030;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorLabel = 0x9AA3B2;
constexpr uint32_t kColorValue = 0xE5E7EB;

struct InfoItem {
    const char* label;
    char value[96];
};

struct UiState {
    lv_obj_t* screen = nullptr;
};
UiState s_ui;

inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

void FormatFlashSize(char* buf, size_t buf_size, size_t bytes) {
    if (bytes >= 1024 * 1024) {
        std::snprintf(buf, buf_size, "%u MB", static_cast<unsigned>(bytes / (1024 * 1024)));
    } else if (bytes >= 1024) {
        std::snprintf(buf, buf_size, "%u KB", static_cast<unsigned>(bytes / 1024));
    } else {
        std::snprintf(buf, buf_size, "%u B", static_cast<unsigned>(bytes));
    }
}

void CollectInfoItems(InfoItem* items, int* count) {
    auto& board = Board::GetInstance();
    const auto* app_desc = esp_app_get_description();
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    const esp_partition_t* ota_partition = esp_ota_get_running_partition();

    int idx = 0;

    items[idx].label = "设备型号";
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s", "MetalioClaw4");
    ++idx;

    items[idx].label = "芯片型号";
    {
        const std::string chip = SystemInfo::GetChipModelName();
        std::snprintf(items[idx].value, sizeof(items[idx].value), "%s", chip.c_str());
    }
    ++idx;

    items[idx].label = "CPU 核心";
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%u 核", chip_info.cores);
    ++idx;

    items[idx].label = "固件版本";
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s", app_desc->version);
    ++idx;

    items[idx].label = "编译时间";
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s %s",
                  app_desc->date, app_desc->time);
    ++idx;

    items[idx].label = "IDF 版本";
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s", app_desc->idf_ver);
    ++idx;

    items[idx].label = "MAC 地址";
    {
        const std::string mac = SystemInfo::GetMacAddress();
        std::snprintf(items[idx].value, sizeof(items[idx].value), "%s", mac.c_str());
    }
    ++idx;

    items[idx].label = "设备 UUID";
    {
        const std::string uuid = board.GetUuid();
        std::snprintf(items[idx].value, sizeof(items[idx].value), "%s", uuid.c_str());
    }
    ++idx;

    items[idx].label = "Flash 容量";
    FormatFlashSize(items[idx].value, sizeof(items[idx].value), SystemInfo::GetFlashSize());
    ++idx;

    items[idx].label = "SRAM 总大小";
    // 芯片物理 HP SRAM（P4 = 768KB），非 heap 剩余可分配量。
    FormatFlashSize(items[idx].value, sizeof(items[idx].value),
                    static_cast<size_t>(SOC_MEM_INTERNAL_HIGH - SOC_MEM_INTERNAL_LOW));
    ++idx;

    items[idx].label = "PSRAM 总大小";
    {
        const size_t psram_total = esp_psram_get_size();
        if (psram_total == 0) {
            std::snprintf(items[idx].value, sizeof(items[idx].value), "无");
        } else {
            FormatFlashSize(items[idx].value, sizeof(items[idx].value), psram_total);
        }
    }
    ++idx;

    items[idx].label = "OTA 分区";
    if (ota_partition != nullptr) {
        std::snprintf(items[idx].value, sizeof(items[idx].value), "%s",
                      ota_partition->label);
    } else {
        std::snprintf(items[idx].value, sizeof(items[idx].value), "未知");
    }
    ++idx;

    *count = idx;
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

void OnBackClicked(lv_event_t* /*e*/) { OnSwipeBack(); }

void OnScreenUnloaded(lv_event_t* /*e*/) { s_ui.screen = nullptr; }

void BuildHeader(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "系统信息");
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);
}

lv_obj_t* CreateInfoRow(lv_obj_t* parent, const InfoItem& item) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, kRowH);
    lv_obj_set_style_bg_color(row, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, item.label);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorLabel), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);

    lv_obj_t* value = lv_label_create(row);
    lv_label_set_text(value, item.value);
    lv_obj_set_width(value, LV_PCT(100));
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(value, lv_color_hex(kColorValue), LV_PART_MAIN);
    lv_obj_set_style_text_font(value, &font_puhui_20_4, LV_PART_MAIN);

    return row;
}

void BuildInfoList(lv_obj_t* parent) {
    InfoItem items[16];
    int count = 0;
    CollectInfoItems(items, &count);

    lv_obj_t* list = lv_obj_create(parent);
    screen_strip_obj_chrome(list);
    lv_obj_set_size(list, kPanelSize - 2 * kPad, kPanelSize - kHeaderH - kPad);
    lv_obj_set_pos(list, kPad, kHeaderH);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, kRowGap, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(list, kPad, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (int i = 0; i < count; ++i) {
        CreateInfoRow(list, items[i]);
    }
}

}  // namespace

lv_obj_t* InfoScreen::Create() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);
    BuildInfoList(scr);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    return scr;
}

void InfoScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: info_screen");
    } else {
        ESP_LOGI(TAG, "unload: info_screen");
    }
}
