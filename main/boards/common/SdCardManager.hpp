#ifndef SD_CARD_MANAGER_HPP
#define SD_CARD_MANAGER_HPP

#include <dirent.h>
#include <sys/stat.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <driver/sdmmc_host.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#include "config.h"  // SDMMC_*_PIN（板级提供）

// ---------------------------------------------------------------------------
// SdCardManager
//
// 板级 SDMMC SD 卡挂载封装，header-only 单例（和 IOExpander.hpp /
// SimpleUart.hpp 风格一致）。
//
// 设计目的：
//   - 板级 init 阶段调一次 Mount()，把 SD 卡挂到 /sdcard。后续 UI / 业务
//     模块只读状态、不再做 mount/unmount，App 之间切换不会反复重 init
//     SDMMC peripheral。
//   - 单例：任何子系统通过 SdCardManager::GetInstance().XXX() 拿到状态和
//     sdmmc_card_t* 句柄。
//
// 行为要点：
//   - Mount() 幂等：已挂上时直接返回 true。底层 SDMMC slot 用 4-bit、SDR50。
//   - 失败不会 ESP_ERROR_CHECK 崩溃，只 LOGE 并返回 false；调用方根据
//     IsMounted() 显示「未检测到 SD 卡」之类的占位。
//   - Mount() 成功后会把根目录文件列表打印到日志，方便调试。
//   - Unmount() 一般不会被业务路径调用（SD 卡常驻挂载），仅保留给
//     调试 / 后续热插拔扩展用。
//
// 板级前置依赖：
//   - 上层负责给 SDMMC PHY 上电（ESP32-P4 是 LDO chan 4，xingzhi-395 在
//     InitializeSDWIFIPower() 里已申请）。
//   - 板级 config.h 需要定义 SDMMC_CLK_PIN / SDMMC_CMD_PIN /
//     SDMMC_D0_PIN..SDMMC_D3_PIN（在 SOC_SDMMC_USE_GPIO_MATRIX 平台上
//     生效，例如 ESP32-P4）。
//
// Typical use:
//
//     auto& sd = SdCardManager::GetInstance();
//     sd.Mount();                        // 板级 init 调一次
//
//     if (sd.IsMounted()) {
//         sdmmc_card_t* card = sd.GetCard();
//         // ... read csd / 列目录 ...
//     }
// ---------------------------------------------------------------------------
class SdCardManager {
public:
    static constexpr const char* kMountPoint = "/sdcard";

    static SdCardManager& GetInstance() {
        static SdCardManager instance;
        return instance;
    }

    SdCardManager(const SdCardManager&) = delete;
    SdCardManager& operator=(const SdCardManager&) = delete;

    // 挂载 SD 卡。重复调用安全，已挂载时直接返回 true。
    bool Mount() {
        if (mounted_) {
            ESP_LOGD(kTag, "SD card already mounted");
            return true;
        }

        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 16 * 1024,
        };

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_0;
        host.max_freq_khz = SDMMC_FREQ_SDR50;
        host.flags &= ~SDMMC_HOST_FLAG_DDR;

        sd_pwr_ctrl_ldo_config_t ldo_config = {
            .ldo_chan_id = SDMMC_LDO_CHAN_ID,
        };
        sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
        esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "Failed to create SD power control driver");
            return false;
        }
        host.pwr_ctrl_handle = pwr_ctrl_handle;

        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
#ifdef SOC_SDMMC_USE_GPIO_MATRIX
        slot_config.clk = SDMMC_CLK_PIN;
        slot_config.cmd = SDMMC_CMD_PIN;
        slot_config.d0 = SDMMC_D0_PIN;
        slot_config.d1 = SDMMC_D1_PIN;
        slot_config.d2 = SDMMC_D2_PIN;
        slot_config.d3 = SDMMC_D3_PIN;
#endif
        slot_config.width = 4;
        slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        ESP_LOGI(kTag, "Mounting SD card filesystem at %s ...", kMountPoint);
        ret = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot_config, &mount_config, &card_);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "Failed to mount SD card: %s", esp_err_to_name(ret));
            card_ = nullptr;
            return false;
        }

        mounted_ = true;
        ESP_LOGI(kTag, "SD card mounted successfully");
        LogRootListing();
        return true;
    }

    // 卸载 SD 卡。一般不需要主动调；保留接口给调试 / 热插拔扩展用。
    void Unmount() {
        if (!mounted_ || card_ == nullptr) {
            return;
        }
        ESP_LOGI(kTag, "Unmounting SD card ...");
        esp_vfs_fat_sdcard_unmount(kMountPoint, card_);
        card_ = nullptr;
        mounted_ = false;
        ESP_LOGI(kTag, "SD card unmounted");
    }

    bool IsMounted() const { return mounted_; }

    sdmmc_card_t* GetCard() const { return card_; }

    const char* GetMountPoint() const { return kMountPoint; }

private:
    SdCardManager() = default;

    static constexpr const char* kTag = "SdCardManager";

    // 挂载成功后扫一次根目录，把每条文件名 + 大小（或 [DIR]）打印出来。
    // 主要给开机日志看的，量大时会刷屏，所以只列一级、不递归。
    //
    // 注意：ESP-IDF 的 FATFS VFS 在 readdir() 循环里直接调 stat() 会破坏
    // 内部目录句柄状态（stat 内部会自己 open/close 一次目录），下一次
    // readdir() 拿到的 dirent 指针就成了野指针，崩在小整数地址上。所以
    // 这里分两段：先 readdir 把名字 + 类型全收集出来、closedir，再统一
    // stat 取大小。
    void LogRootListing() {
        DIR* dir = opendir(kMountPoint);
        if (dir == nullptr) {
            ESP_LOGW(kTag, "opendir(%s) failed, skip listing", kMountPoint);
            return;
        }

        struct Entry {
            std::string name;
            bool is_dir;
        };
        std::vector<Entry> entries;
        entries.reserve(16);

        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            if (ent->d_name[0] == '.' &&
                (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
                continue;
            }
            // d_type 在 ESP-IDF FATFS VFS 里被填好了（DT_DIR / DT_REG），
            // 这里先记下来，避免后面再 stat 一次去判断类型。
            entries.push_back({std::string(ent->d_name), ent->d_type == DT_DIR});
        }
        closedir(dir);

        ESP_LOGI(kTag, "Root directory of %s:", kMountPoint);
        for (const auto& e : entries) {
            if (e.is_dir) {
                ESP_LOGI(kTag, "  [DIR]  %s", e.name.c_str());
                continue;
            }

            char full_path[300];
            snprintf(full_path, sizeof(full_path), "%s/%s", kMountPoint, e.name.c_str());

            struct stat st;
            if (stat(full_path, &st) == 0) {
                // 项目用了 newlib-nano（CONFIG_LIBC_NEWLIB_NANO_FORMAT=y），
                // printf 不支持 %lld / %llu，PRIu64 会原样打出 "llu"。SD 卡
                // 单个文件不会超 4GB，直接用 %lu + unsigned long 即可。
                ESP_LOGI(kTag, "  %10lu  %s", (unsigned long)st.st_size, e.name.c_str());
            } else {
                ESP_LOGI(kTag, "  [???]  %s", e.name.c_str());
            }
        }
        ESP_LOGI(kTag, "Total %u entries", (unsigned)entries.size());
    }

    sdmmc_card_t* card_ = nullptr;
    bool mounted_ = false;
};

#endif  // SD_CARD_MANAGER_HPP
