#include "sd_card_screen.h"
#include "i18n.h"

#include "home_screen/home_screen.h"
#include "screen_util.h"

#include <dirent.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <sdmmc_cmd.h>
#include "ff.h"

#include "SdCardManager.hpp"
#include "usb_virtual_disk.h"

LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_20_4);

namespace {

constexpr const char* TAG_SD = "SdCardScreen";
constexpr int kPanelSize = 720;
constexpr int kHeaderH = 80;
constexpr int kPad = 16;
constexpr size_t kMaxNameLen = 256;  // POSIX NAME_MAX = 255

// ----- color palette -----
constexpr uint32_t kColorBg = 0x000000;
constexpr uint32_t kColorTextPrimary = 0xFFFFFF;
constexpr uint32_t kColorTextSecondary = 0x9A9A9A;
constexpr uint32_t kColorHintText = 0x6E6E70;
constexpr uint32_t kColorDeleteBg = 0xE74C3C;
constexpr uint32_t kColorDeleteBgPress = 0xC0392B;
constexpr uint32_t kColorFileBg = 0x1A1A1A;
constexpr uint32_t kColorFileBorder = 0x333333;

// ----- UI elements that need updating -----
lv_obj_t* s_status_lbl = nullptr;
lv_obj_t* s_capacity_lbl = nullptr;
lv_obj_t* s_file_list = nullptr;
lv_obj_t* s_no_files_lbl = nullptr;
lv_obj_t* s_status_dot = nullptr;
lv_obj_t* s_usb_btn = nullptr;
lv_obj_t* s_usb_btn_lbl = nullptr;
lv_obj_t* s_usb_hint_lbl = nullptr;
lv_obj_t* s_screen = nullptr;

constexpr int kUsbBtnY = kHeaderH + kPad + 64;
constexpr int kUsbHintY = kUsbBtnY + 56;
constexpr int kDividerY = kUsbHintY + 36;
constexpr int kListY = kDividerY + 16;

// ----- helper: human-readable size (integer-only, safe with newlib-nano) -----
void FormatSize(uint64_t bytes, char* buf, size_t buf_size) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        // e.g. "3.21 GB"
        unsigned gb_int = bytes / (1024ULL * 1024 * 1024);
        unsigned gb_frac = (bytes % (1024ULL * 1024 * 1024)) / (10ULL * 1024 * 1024);
        snprintf(buf, buf_size, "%u.%02u GB", gb_int, gb_frac);
    } else if (bytes >= 1024 * 1024) {
        unsigned mb_int = bytes / (1024 * 1024);
        unsigned mb_frac = (bytes % (1024 * 1024)) / (10 * 1024);
        snprintf(buf, buf_size, "%u.%02u MB", mb_int, mb_frac);
    } else if (bytes >= 1024) {
        unsigned kb_int = bytes / 1024;
        unsigned kb_frac = ((bytes % 1024) * 100) / 1024;
        snprintf(buf, buf_size, "%u.%02u KB", kb_int, kb_frac);
    } else {
        snprintf(buf, buf_size, "%" PRIu64 " B", bytes);
    }
}

// ----- navigation -----
void OnSwipeBack() {
    UsbVirtualDisk::GetInstance().DisableIfActive();
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

// ----- file deletion -----
struct FileEntry {
    char name[kMaxNameLen];
    char path[kMaxNameLen + 16];  // mount point + '/' + name
};

// Forward declarations
void UpdateStatusUI();
void RebuildFileList(lv_obj_t* parent);
void RefreshUsbUi();

void OnUsbVirtualDiskClicked(lv_event_t* /*e*/) {
    auto& vd = UsbVirtualDisk::GetInstance();
    if (!vd.IsSupported() || vd.IsBusy()) {
        return;
    }
    vd.Toggle();
}

void OnUsbUiNotifyAsync(void* /*user_data*/) {
    // 页面已卸载则忽略过期回调，避免碰已释放的 LVGL 对象
    if (s_screen == nullptr) {
        return;
    }
    RefreshUsbUi();
    UpdateStatusUI();
    RebuildFileList(s_screen);
}

void OnUsbVirtualDiskNotify() {
    lv_async_call(OnUsbUiNotifyAsync, nullptr);
}

void RefreshUsbUi() {
    auto& vd = UsbVirtualDisk::GetInstance();
    if (s_usb_btn_lbl != nullptr) {
        const char* btn_text =
            vd.IsGadgetActive() ? I18n::T("停用虚拟 U 盘") : I18n::T("启用虚拟 U 盘");
        lv_label_set_text(s_usb_btn_lbl, btn_text);
    }
    if (s_usb_btn != nullptr) {
        const uint32_t bg =
            vd.IsGadgetActive() ? static_cast<uint32_t>(0xC44536) : 0x2F6FED;
        lv_obj_set_style_bg_color(s_usb_btn, lv_color_hex(bg), LV_PART_MAIN);
        if (vd.IsBusy() || !vd.IsSupported()) {
            lv_obj_add_state(s_usb_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(s_usb_btn, LV_STATE_DISABLED);
        }
    }
    if (s_usb_hint_lbl != nullptr) {
        lv_label_set_text(s_usb_hint_lbl, I18n::T(UsbVirtualDisk::HintMsgid(vd.GetUiHint())));
    }
}

void OnDeleteFile(lv_event_t* e) {
    auto& vd = UsbVirtualDisk::GetInstance();
    // 仅在 SD 被主机占用或切换中禁止删除
    if (vd.IsSdExportedToHost() || vd.IsBusy()) {
        return;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        return;
    }
    auto* entry = static_cast<FileEntry*>(lv_event_get_user_data(e));
    if (entry == nullptr)
        return;

    ESP_LOGI(TAG_SD, "Deleting file: %s", entry->path);
    if (unlink(entry->path) != 0) {
        ESP_LOGE(TAG_SD, "Failed to delete: %s", entry->path);
        return;
    }

    UpdateStatusUI();
    if (s_file_list != nullptr) {
        RebuildFileList(lv_obj_get_parent(s_file_list));
    }
}

// ----- file list builder -----
void ClearFileList() {
    if (s_file_list == nullptr)
        return;
    lv_obj_clean(s_file_list);
}

// Cast helper to silence -Wdeprecated-enum-enum-conversion
static inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

// Build a single file row.  Stat is done inside this function so the caller
// doesn't need a large stack buffer.
void BuildFileRow(const char* name, const char* path) {
    struct stat st;
    bool is_dir = false;
    uint64_t size = 0;
    if (stat(path, &st) == 0) {
        is_dir = S_ISDIR(st.st_mode);
        size = st.st_size;
    }

    // Row container
    lv_obj_t* row = lv_obj_create(s_file_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, kPanelSize - 2 * kPad, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(kColorFileBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(kColorFileBorder), LV_PART_MAIN);
    lv_obj_set_style_border_opa(row, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // File info column (name + size)
    lv_obj_t* info_col = lv_obj_create(row);
    lv_obj_remove_style_all(info_col);
    lv_obj_set_size(info_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(info_col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(info_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(info_col, 1);

    // File/dir name
    lv_obj_t* name_lbl = lv_label_create(info_col);
    lv_label_set_text(name_lbl, name);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_lbl, 450);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(kColorTextPrimary), LV_PART_MAIN);
    lv_obj_set_style_text_font(name_lbl, &font_puhui_20_4, LV_PART_MAIN);

    // File size
    char size_str[64];
    if (is_dir) {
        snprintf(size_str, sizeof(size_str), I18n::T("目录"));
    } else {
        FormatSize(size, size_str, sizeof(size_str));
    }
    lv_obj_t* size_lbl = lv_label_create(info_col);
    lv_label_set_text(size_lbl, size_str);
    lv_obj_set_style_text_color(size_lbl, lv_color_hex(kColorTextSecondary), LV_PART_MAIN);
    lv_obj_set_style_text_font(size_lbl, &font_puhui_20_4, LV_PART_MAIN);

    // Delete button (only for files, not directories)
    if (!is_dir) {
        lv_obj_t* del_btn = lv_button_create(row);
        lv_obj_set_size(del_btn, 64, 40);
        lv_obj_set_style_bg_color(del_btn, lv_color_hex(kColorDeleteBg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(del_btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(del_btn, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_color(del_btn, lv_color_hex(kColorDeleteBgPress),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_set_style_shadow_width(del_btn, 0, LV_PART_MAIN);

        lv_obj_t* del_lbl = lv_label_create(del_btn);
        lv_label_set_text(del_lbl, I18n::T("删除"));
        lv_obj_set_style_text_font(del_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(del_lbl, lv_color_hex(kColorTextPrimary), LV_PART_MAIN);
        lv_obj_center(del_lbl);

        // Allocate FileEntry on heap so it outlives this function
        auto* file_ctx = new FileEntry;
        strlcpy(file_ctx->name, name, sizeof(file_ctx->name));
        strlcpy(file_ctx->path, path, sizeof(file_ctx->path));

        // CLICKED -> delete file
        lv_obj_add_event_cb(del_btn, OnDeleteFile, LV_EVENT_CLICKED, file_ctx);

        // DELETE -> free the FileEntry
        lv_obj_add_event_cb(
            del_btn,
            [](lv_event_t* ev) { delete static_cast<FileEntry*>(lv_event_get_user_data(ev)); },
            LV_EVENT_DELETE, file_ctx);
    }
}

void RebuildFileList(lv_obj_t* parent) {
    (void)parent;
    ClearFileList();

    auto& vd = UsbVirtualDisk::GetInstance();
    if (vd.IsSdExportedToHost()) {
        if (s_no_files_lbl != nullptr) {
            lv_label_set_text(s_no_files_lbl, I18n::T("SD 正被电脑占用，停用或弹出后可浏览"));
            lv_obj_remove_flag(s_no_files_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (!SdCardManager::GetInstance().IsMounted()) {
        if (s_no_files_lbl != nullptr) {
            lv_label_set_text(s_no_files_lbl, I18n::T("请插入 SD 卡"));
            lv_obj_remove_flag(s_no_files_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    const char* mount_point = SdCardManager::GetInstance().GetMountPoint();
    DIR* dir = opendir(mount_point);
    if (dir == nullptr) {
        ESP_LOGE(TAG_SD, "Failed to open directory: %s", mount_point);
        if (s_no_files_lbl != nullptr) {
            lv_label_set_text(s_no_files_lbl, I18n::T("SD 卡内没有文件"));
            lv_obj_remove_flag(s_no_files_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    char name_buf[kMaxNameLen];
    char path_buf[kMaxNameLen + 16];
    bool has_files = false;
    struct dirent* entry;

    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        has_files = true;
        strlcpy(name_buf, entry->d_name, sizeof(name_buf));
        snprintf(path_buf, sizeof(path_buf), "%s/%s", mount_point, name_buf);
        BuildFileRow(name_buf, path_buf);
    }
    closedir(dir);

    if (!has_files) {
        if (s_no_files_lbl != nullptr) {
            lv_label_set_text(s_no_files_lbl, I18n::T("SD 卡内没有文件"));
            lv_obj_remove_flag(s_no_files_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (s_no_files_lbl != nullptr) {
            lv_obj_add_flag(s_no_files_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ----- header builder -----
void BuildHeader(lv_obj_t* parent) {
    // 返回按钮：透明圆形按钮 + "←" 图标，按下时白色半透明叠加
    lv_obj_t* back_btn = lv_button_create(parent);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, 72, 72);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_20,
                            Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, kPad + 8, kPad + 12);
    screen_swipe_back_ignore(back_btn, true);

    lv_obj_t* back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    auto back_fn = [](lv_event_t* /*e*/) { OnSwipeBack(); };
    lv_obj_add_event_cb(back_btn, back_fn, LV_EVENT_CLICKED, nullptr);

    // Title
    lv_obj_t* title = lv_label_create(parent);
    lv_label_set_text(title, I18n::T("SD 卡"));
    lv_obj_set_style_text_color(title, lv_color_hex(kColorTextPrimary), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, kPad + 20);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);
}

// ----- status section -----
void BuildStatusSection(lv_obj_t* parent) {
    // Status indicator row (dot + text)
    lv_obj_t* status_row = lv_obj_create(parent);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_size(status_row, kPanelSize - 2 * kPad, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(status_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_row, 12, LV_PART_MAIN);
    lv_obj_set_pos(status_row, kPad, kHeaderH + kPad);

    // Status dot (colored circle)
    s_status_dot = lv_obj_create(status_row);
    lv_obj_remove_style_all(s_status_dot);
    lv_obj_set_size(s_status_dot, 12, 12);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(0xFF0000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, LV_PART_MAIN);

    // Status text
    s_status_lbl = lv_label_create(status_row);
    lv_label_set_text(s_status_lbl, I18n::T("检测中..."));
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorTextPrimary), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status_lbl, &font_puhui_20_4, LV_PART_MAIN);

    // Capacity label
    s_capacity_lbl = lv_label_create(parent);
    lv_label_set_text(s_capacity_lbl, "");
    lv_obj_set_style_text_color(s_capacity_lbl, lv_color_hex(kColorTextSecondary), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_capacity_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_pos(s_capacity_lbl, kPad, kHeaderH + kPad + 32);

    // Virtual USB drive toggle
    s_usb_btn = lv_button_create(parent);
    lv_obj_set_size(s_usb_btn, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_pad_hor(s_usb_btn, 20, LV_PART_MAIN);
    lv_obj_set_style_radius(s_usb_btn, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_usb_btn, lv_color_hex(0x2F6FED), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_usb_btn, 0, LV_PART_MAIN);
    lv_obj_align(s_usb_btn, LV_ALIGN_TOP_LEFT, kPad, kUsbBtnY);
    lv_obj_add_event_cb(s_usb_btn, OnUsbVirtualDiskClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(s_usb_btn, true);

    s_usb_btn_lbl = lv_label_create(s_usb_btn);
    lv_label_set_text(s_usb_btn_lbl, I18n::T("启用虚拟 U 盘"));
    lv_obj_set_style_text_color(s_usb_btn_lbl, lv_color_hex(kColorTextPrimary), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_usb_btn_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(s_usb_btn_lbl);

    s_usb_hint_lbl = lv_label_create(parent);
    lv_obj_set_width(s_usb_hint_lbl, kPanelSize - 2 * kPad);
    lv_label_set_long_mode(s_usb_hint_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_usb_hint_lbl, I18n::T("启用后电脑可将本机识别为 U 盘"));
    lv_obj_set_style_text_color(s_usb_hint_lbl, lv_color_hex(kColorTextSecondary), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_usb_hint_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_pos(s_usb_hint_lbl, kPad, kUsbHintY);

    // Divider line
    lv_obj_t* divider = lv_obj_create(parent);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, kPanelSize - 2 * kPad, 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorFileBorder), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_pos(divider, kPad, kDividerY);
}

// Build the file list container
void BuildFileListSection(lv_obj_t* parent) {
    // "No files" placeholder
    s_no_files_lbl = lv_label_create(parent);
    lv_label_set_text(s_no_files_lbl, "");
    lv_obj_set_style_text_color(s_no_files_lbl, lv_color_hex(kColorHintText), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_no_files_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_no_files_lbl, LV_ALIGN_CENTER, 0, 40);

    // Scrollable file list
    constexpr int kListH = kPanelSize - kListY - kPad;
    s_file_list = lv_obj_create(parent);
    lv_obj_remove_style_all(s_file_list);
    lv_obj_set_size(s_file_list, kPanelSize - 2 * kPad, kListH);
    lv_obj_set_pos(s_file_list, kPad, kListY);
    lv_obj_set_style_bg_opa(s_file_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(s_file_list, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_file_list, LV_SCROLLBAR_MODE_OFF);
}

void UpdateStatusUI() {
    auto& sd = SdCardManager::GetInstance();
    auto& vd = UsbVirtualDisk::GetInstance();
    sdmmc_card_t* card = sd.GetCard();
    const bool usable = sd.IsMounted() && card != nullptr && !vd.IsSdExportedToHost();
    if (usable || (card != nullptr && vd.IsGadgetActive())) {
        if (s_status_dot != nullptr) {
            lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(0x00CC00), LV_PART_MAIN);
        }
        if (s_status_lbl != nullptr) {
            lv_label_set_text(s_status_lbl, I18n::T("SD 卡已插入"));
        }

        // Update capacity using FatFs free-cluster info (only when APP-mounted)
        if (s_capacity_lbl != nullptr) {
            if (!usable) {
                lv_label_set_text(s_capacity_lbl, I18n::T("SD 正被电脑占用，停用或弹出后可浏览"));
            } else {
                FATFS* fs = nullptr;
                DWORD free_clusters = 0;
                uint64_t total_bytes = 0;
                uint64_t free_bytes = 0;

                FRESULT res = f_getfree("0:", &free_clusters, &fs);
                if (res == FR_OK && fs != nullptr) {
                    // FatFs csize is in sectors; convert to bytes
                    DWORD ssize = 512;  // default sector size for SD cards
                    total_bytes = (uint64_t)(fs->n_fatent - 2) * fs->csize * ssize;
                    free_bytes = (uint64_t)free_clusters * fs->csize * ssize;
                } else {
                    // Fallback: use CSD capacity
                    total_bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
                    free_bytes = 0;
                }

                char total_str[32], free_str[32];
                FormatSize(total_bytes, total_str, sizeof(total_str));
                FormatSize(free_bytes, free_str, sizeof(free_str));

                char cap_buf[96];
                snprintf(cap_buf, sizeof(cap_buf), I18n::T("剩余 %s / 总容量 %s"), free_str,
                         total_str);
                lv_label_set_text(s_capacity_lbl, cap_buf);
            }
        }
    } else {
        if (s_status_dot != nullptr) {
            lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(0xFF0000), LV_PART_MAIN);
        }
        if (s_status_lbl != nullptr) {
            lv_label_set_text(s_status_lbl, I18n::T("未检测到 SD 卡"));
        }
        if (s_capacity_lbl != nullptr) {
            lv_label_set_text(s_capacity_lbl, I18n::T("请插入 SD 卡"));
        }
        if (s_no_files_lbl != nullptr) {
            lv_label_set_text(s_no_files_lbl, I18n::T("请插入 SD 卡"));
            lv_obj_remove_flag(s_no_files_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

}  // namespace

lv_obj_t* SdCardScreen::Create() {
    lv_obj_t* scr = lv_obj_create(NULL);
    s_screen = scr;
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);

    BuildHeader(scr);
    BuildStatusSection(scr);
    BuildFileListSection(scr);

    auto& vd = UsbVirtualDisk::GetInstance();
    vd.Init();
    vd.SetUiNotify(OnUsbVirtualDiskNotify);
    RefreshUsbUi();

    // SD 卡的挂载已经在板级 init（METALIO_CLAW_4::InitializeSdCard()）里完成。
    // 这里只做一次状态读取并刷新 UI；如果开机时 mount 失败（卡没插），就只
    // 显示状态文字、不去重试，等用户回到首页 / 后续手动 reboot 时再处理。
    UpdateStatusUI();
    RebuildFileList(scr);

    // Right-swipe to go back
    screen_attach_swipe_back(scr, OnSwipeBack);

    return scr;
}

void SdCardScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_SD, "load: sd_card_screen (mounted=%d gadget=%d)",
                 SdCardManager::GetInstance().IsMounted() ? 1 : 0,
                 UsbVirtualDisk::GetInstance().IsGadgetActive() ? 1 : 0);
        RefreshUsbUi();
        UpdateStatusUI();
        if (s_screen != nullptr) {
            RebuildFileList(s_screen);
        }
    } else {
        ESP_LOGI(TAG_SD, "unload: sd_card_screen");
        UsbVirtualDisk::GetInstance().DisableIfActive();
        UsbVirtualDisk::GetInstance().SetUiNotify(nullptr);
        s_screen = nullptr;
        s_usb_btn = nullptr;
        s_usb_btn_lbl = nullptr;
        s_usb_hint_lbl = nullptr;
        s_status_lbl = nullptr;
        s_capacity_lbl = nullptr;
        s_file_list = nullptr;
        s_no_files_lbl = nullptr;
        s_status_dot = nullptr;
    }
}
