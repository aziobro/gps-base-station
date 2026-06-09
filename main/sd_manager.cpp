#include "sd_manager.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

namespace {

constexpr char kTag[] = "sd_manager";

// Waveshare ESP32-P4-WIFI6 microSD card pins (same traces as SDMMC slot 0,
// but driven in SPI mode to avoid the SDMMC peripheral used by esp_hosted).
// SD D1/D2 (GPIO40/41) are unused in SPI mode; the card holds them high
// internally once SPI mode is selected.
constexpr gpio_num_t kClkPin  = GPIO_NUM_43;  // SD CLK  → SPI SCLK
constexpr gpio_num_t kMosiPin = GPIO_NUM_44;  // SD CMD  → SPI MOSI
constexpr gpio_num_t kMisoPin = GPIO_NUM_39;  // SD D0   → SPI MISO
constexpr gpio_num_t kCsPin   = GPIO_NUM_42;  // SD D3   → SPI CS

// Append bytes to a heap buffer, growing with realloc as needed.
bool buf_append(char **buf, size_t *len, size_t *cap, const char *s, size_t slen) {
    if (*len + slen + 1 > *cap) {
        size_t new_cap = *cap + slen + 256;
        char *tmp = static_cast<char *>(realloc(*buf, new_cap));
        if (!tmp) return false;
        *buf = tmp;
        *cap = new_cap;
    }
    memcpy(*buf + *len, s, slen);
    *len += slen;
    (*buf)[*len] = '\0';
    return true;
}

bool buf_append_str(char **buf, size_t *len, size_t *cap, const char *s) {
    return buf_append(buf, len, cap, s, strlen(s));
}

bool buf_append_json_str(char **buf, size_t *len, size_t *cap, const char *s) {
    for (const char *p = s; *p; ++p) {
        char esc[3] = {'\\', '\0', '\0'};
        switch (*p) {
            case '"':  esc[1] = '"';  break;
            case '\\': esc[1] = '\\'; break;
            case '\n': esc[1] = 'n';  break;
            case '\r': esc[1] = 'r';  break;
            case '\t': esc[1] = 't';  break;
            default:
                if (!buf_append(buf, len, cap, p, 1)) return false;
                continue;
        }
        if (!buf_append(buf, len, cap, esc, 2)) return false;
    }
    return true;
}

}  // namespace

SdManager::~SdManager() {
    unmount();
}

esp_err_t SdManager::mount() {
    if (mounted_) return ESP_OK;

    // The Waveshare ESP32-P4-WIFI6 connects VDDPST_5 and the microSD supply
    // to ESP_LDO_VO4. Without enabling channel 4 the bus pins toggle but the
    // card stays unpowered and ACMD41 times out.
    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = 4 };
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &ldo_handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "SD LDO init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialise the SPI bus that the SD card sits on.
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = kMosiPin,
        .miso_io_num = kMisoPin,
        .sclk_io_num = kClkPin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .data_io_default_level = false,
        .max_transfer_sz = 4096,
        .flags = 0,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags = 0,
    };
    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "SPI bus init failed: %s", esp_err_to_name(ret));
        sd_pwr_ctrl_del_on_chip_ldo(ldo_handle_);
        ldo_handle_ = nullptr;
        return ret;
    }
    spi_bus_initialized_ = true;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.pwr_ctrl_handle = ldo_handle_;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = kCsPin;
    slot_cfg.host_id = SPI2_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    ret = esp_vfs_fat_sdspi_mount(kMountPoint, &host, &slot_cfg, &mount_cfg, &card_);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "SD card unavailable: %s", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        spi_bus_initialized_ = false;
        sd_pwr_ctrl_del_on_chip_ldo(ldo_handle_);
        ldo_handle_ = nullptr;
        return ret;
    }

    mounted_ = true;
    sdmmc_card_print_info(stdout, card_);
    ESP_LOGI(kTag, "SD card mounted at %s (SPI mode)", kMountPoint);
    return ESP_OK;
}

void SdManager::unmount() {
    if (!mounted_) return;
    esp_vfs_fat_sdcard_unmount(kMountPoint, card_);
    card_ = nullptr;
    mounted_ = false;
    if (spi_bus_initialized_) {
        spi_bus_free(SPI2_HOST);
        spi_bus_initialized_ = false;
    }
    if (ldo_handle_) {
        sd_pwr_ctrl_del_on_chip_ldo(ldo_handle_);
        ldo_handle_ = nullptr;
    }
    ESP_LOGI(kTag, "SD card unmounted");
}

const char *SdManager::safe_path(const char *path) {
    if (!path || !path[0]) return nullptr;
    const size_t mpLen = strlen(kMountPoint);
    if (strncmp(path, kMountPoint, mpLen) != 0) return nullptr;
    if (path[mpLen] != '\0' && path[mpLen] != '/') return nullptr;
    if (strstr(path, "..")) return nullptr;
    return path;
}

SdManager::DiskStats SdManager::disk_stats() const {
    DiskStats s;
    if (!mounted_) return s;
    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(kMountPoint, &total, &free_bytes) != ESP_OK) return s;
    s.total_bytes = total;
    s.used_bytes  = total - free_bytes;
    s.valid = true;
    return s;
}

char *SdManager::list_dir(const char *path) const {
    if (!mounted_ || !path) return nullptr;
    DIR *d = opendir(path);
    if (!d) return nullptr;

    size_t len = 0, cap = 512;
    char *buf = static_cast<char *>(malloc(cap));
    if (!buf) { closedir(d); return nullptr; }
    buf[0] = '\0';

    bool ok = buf_append_str(&buf, &len, &cap, "[");
    bool first = true;
    struct dirent *ent;
    while (ok && (ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char fp[320];
        snprintf(fp, sizeof(fp), "%s/%s", path, ent->d_name);
        fp[sizeof(fp) - 1] = '\0';
        struct stat st = {};
        stat(fp, &st);
        bool is_dir = S_ISDIR(st.st_mode);

        if (!first) ok = ok && buf_append_str(&buf, &len, &cap, ",");
        first = false;

        char num[32];
        snprintf(num, sizeof(num), "%lld", static_cast<long long>(is_dir ? 0 : st.st_size));
        ok = ok &&
            buf_append_str(&buf, &len, &cap, "{\"name\":\"") &&
            buf_append_json_str(&buf, &len, &cap, ent->d_name) &&
            buf_append_str(&buf, &len, &cap, "\",\"path\":\"") &&
            buf_append_json_str(&buf, &len, &cap, fp) &&
            buf_append_str(&buf, &len, &cap, "\",\"is_dir\":") &&
            buf_append_str(&buf, &len, &cap, is_dir ? "true" : "false") &&
            buf_append_str(&buf, &len, &cap, ",\"size\":") &&
            buf_append_str(&buf, &len, &cap, num) &&
            buf_append_str(&buf, &len, &cap, "}");
    }
    closedir(d);

    if (!ok || !buf_append_str(&buf, &len, &cap, "]")) {
        free(buf);
        return nullptr;
    }
    return buf;
}

bool SdManager::delete_entry(const char *path) const {
    if (!mounted_ || !path) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    int r = S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
    if (r != 0) ESP_LOGW(kTag, "Delete %s failed: errno %d", path, errno);
    return r == 0;
}

bool SdManager::rename_entry(const char *from, const char *to) const {
    if (!mounted_ || !from || !to) return false;
    int r = rename(from, to);
    if (r != 0) ESP_LOGW(kTag, "Rename %s -> %s failed: errno %d", from, to, errno);
    return r == 0;
}

void SdManager::ensure_dirs() const {
    if (!mounted_) return;
    static const char *const dirs[] = {"logs", "rawdata"};
    for (const char *name : dirs) {
        char path[64];
        snprintf(path, sizeof(path), "%s/%s", kMountPoint, name);
        struct stat st;
        if (stat(path, &st) == 0) continue;
        if (mkdir(path, 0755) == 0) {
            ESP_LOGI(kTag, "Created directory: %s", path);
        } else {
            ESP_LOGW(kTag, "mkdir %s failed: errno %d", path, errno);
        }
    }
}
