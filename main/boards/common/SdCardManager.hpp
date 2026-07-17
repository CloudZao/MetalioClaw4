#ifndef SD_CARD_MANAGER_HPP
#define SD_CARD_MANAGER_HPP

#include <dirent.h>
#include <sys/stat.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
// SdCardManager — 板级 SDMMC 单例
//
// VFS 路径：Mount()/Unmount()（esp_vfs_fat_sdmmc_*）
// MSC 路径：InitRawCardForMsc() 重新 card_init（不可复用 Unmount 后的 card）
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

    bool Mount() {
        if (mounted_) {
            ESP_LOGD(kTag, "SD card already mounted");
            return true;
        }
        // 若曾给 MSC 做过 raw init，必须先释放 host，否则二次 host.init 会失败。
        ReleaseRawCard();

        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 16 * 1024,
        };

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_0;
        host.max_freq_khz = SDMMC_FREQ_SDR50;
        host.flags &= ~SDMMC_HOST_FLAG_DDR;

        if (!EnsurePwrCtrl()) {
            return false;
        }
        host.pwr_ctrl_handle = pwr_ctrl_;

        sdmmc_slot_config_t slot_config = MakeSlotConfig();

        ESP_LOGI(kTag, "Mounting SD card filesystem at %s ...", kMountPoint);
        esp_err_t ret =
            esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot_config, &mount_config, &card_);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "Failed to mount SD card: %s", esp_err_to_name(ret));
            card_ = nullptr;
            return false;
        }

        raw_owned_ = false;  // card 由 fatfs 拥有，Unmount 时 free
        mounted_ = true;
        ESP_LOGI(kTag, "SD card mounted successfully");
        LogRootListing();
        return true;
    }

    void Unmount() {
        if (!mounted_ || card_ == nullptr) {
            // 可能仅有 raw card
            ReleaseRawCard();
            return;
        }
        ESP_LOGI(kTag, "Unmounting SD card ...");
        // unmount 内部会 host.deinit + free(card)
        esp_err_t err = esp_vfs_fat_sdcard_unmount(kMountPoint, card_);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "sdcard_unmount: %s", esp_err_to_name(err));
        }
        card_ = nullptr;
        mounted_ = false;
        raw_owned_ = false;
        ESP_LOGI(kTag, "SD card unmounted");
    }

    // 卸掉 VFS（若有），重新 sdmmc_card_init，供 TinyUSB MSC 使用。
    // 返回的 card 由本单例 raw_owned_ 管理，失败时已清理干净。
    sdmmc_card_t* InitRawCardForMsc() {
        if (mounted_) {
            Unmount();
        } else {
            ReleaseRawCard();
        }

        if (!EnsurePwrCtrl()) {
            return nullptr;
        }

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_0;
        host.max_freq_khz = SDMMC_FREQ_SDR50;
        host.flags &= ~SDMMC_HOST_FLAG_DDR;
        host.pwr_ctrl_handle = pwr_ctrl_;

        sdmmc_slot_config_t slot_config = MakeSlotConfig();

        sdmmc_card_t* card = static_cast<sdmmc_card_t*>(calloc(1, sizeof(sdmmc_card_t)));
        if (card == nullptr) {
            ESP_LOGE(kTag, "calloc sdmmc_card_t failed");
            return nullptr;
        }

        esp_err_t ret = (*host.init)();
        if (ret == ESP_ERR_INVALID_STATE) {
            // 残留 host：先尝试反初始化再 init
            ESP_LOGW(kTag, "host already inited, force deinit then retry");
            CallHostDeinit(&host);
            ret = (*host.init)();
        }
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "SDMMC host init failed: %s", esp_err_to_name(ret));
            free(card);
            return nullptr;
        }

        ret = sdmmc_host_init_slot(host.slot, &slot_config);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "SDMMC slot init failed: %s", esp_err_to_name(ret));
            CallHostDeinit(&host);
            free(card);
            return nullptr;
        }

        ret = sdmmc_card_init(&host, card);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "sdmmc_card_init failed: %s", esp_err_to_name(ret));
            CallHostDeinit(&host);
            free(card);
            return nullptr;
        }

        // 探测卡是否真正可访问
        ret = sdmmc_get_status(card);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "sdmmc_get_status failed: %s", esp_err_to_name(ret));
            CallHostDeinit(&card->host);
            free(card);
            return nullptr;
        }

        card_ = card;
        raw_owned_ = true;
        mounted_ = false;
        ESP_LOGI(kTag, "Raw SDMMC card ready for MSC");
        return card_;
    }

    // 释放 InitRawCardForMsc 占用的 host/card（不影响 VFS mounted 状态以外的路径）。
    void ReleaseRawCard() {
        if (!raw_owned_ || card_ == nullptr) {
            if (!mounted_) {
                card_ = nullptr;
            }
            raw_owned_ = false;
            return;
        }
        ESP_LOGI(kTag, "Releasing raw SDMMC card");
        CallHostDeinit(&card_->host);
        free(card_);
        card_ = nullptr;
        raw_owned_ = false;
        mounted_ = false;
    }

    void NotifyExternalAppMount(sdmmc_card_t* card) {
        card_ = card;
        // MSC APP 挂载后本机可读；card 仍由 MSC/raw 路径持有
        mounted_ = (card != nullptr);
    }

    void NotifyExportedToHost() {
        mounted_ = false;
    }

    // MSC 失败回滚：释放 raw host，再走完整 VFS Mount。
    bool RemountVfsFromCard() {
        if (mounted_ && !raw_owned_) {
            return true;
        }
        ReleaseRawCard();
        return Mount();
    }

    bool IsMounted() const { return mounted_; }

    sdmmc_card_t* GetCard() const { return card_; }

    bool HasCard() const { return card_ != nullptr; }

    const char* GetMountPoint() const { return kMountPoint; }

private:
    SdCardManager() = default;

    static constexpr const char* kTag = "SdCardManager";

    static void CallHostDeinit(const sdmmc_host_t* host) {
        if (host == nullptr) {
            return;
        }
        if (host->flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
            if (host->deinit_p) {
                host->deinit_p(host->slot);
            }
        } else if (host->deinit) {
            host->deinit();
        }
    }

    bool EnsurePwrCtrl() {
        if (pwr_ctrl_ != nullptr) {
            return true;
        }
        sd_pwr_ctrl_ldo_config_t ldo_config = {
            .ldo_chan_id = SDMMC_LDO_CHAN_ID,
        };
        esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "Failed to create SD power control driver: %s",
                     esp_err_to_name(ret));
            pwr_ctrl_ = nullptr;
            return false;
        }
        return true;
    }

    static sdmmc_slot_config_t MakeSlotConfig() {
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
        return slot_config;
    }

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
                (ent->d_name[1] == '\0' ||
                 (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
                continue;
            }
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
                ESP_LOGI(kTag, "  %10lu  %s", (unsigned long)st.st_size, e.name.c_str());
            } else {
                ESP_LOGI(kTag, "  [???]  %s", e.name.c_str());
            }
        }
        ESP_LOGI(kTag, "Total %u entries", (unsigned)entries.size());
    }

    sdmmc_card_t* card_ = nullptr;
    sd_pwr_ctrl_handle_t pwr_ctrl_ = nullptr;
    bool mounted_ = false;
    bool raw_owned_ = false;  // true：card_ 由 InitRawCardForMsc calloc，需 ReleaseRawCard
};

#endif  // SD_CARD_MANAGER_HPP
