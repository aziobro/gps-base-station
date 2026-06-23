#include "sd_manager.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <vector>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
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

// Record one failed entry into a DeleteResult (captures errno on the first failure).
void note_fail(SdManager::DeleteResult &res, const char *path) {
    res.failed++;
    if (res.first_error[0] == '\0') {
        snprintf(res.first_error, sizeof(res.first_error), "%s: errno %d", path, errno);
    }
}

}  // namespace

SdManager::~SdManager() {
    unmount();
}

esp_err_t SdManager::mount() {
    if (mounted_) return ESP_OK;

    // The microSD supply (VDDPST_5 / LDO VO4) is acquired once by the
    // application at startup (see app_main), so there is a single owner of that
    // channel; the SDSPI host does not manage card power here.

    // Initialise the SPI bus that the SD card sits on.
    esp_err_t ret = ESP_OK;
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
        return ret;
    }
    spi_bus_initialized_ = true;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

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

        // Use the directory-entry type from readdir (reliable on FATFS);
        // stat() here was misclassifying regular files. stat only for size.
        bool is_dir;
        long long fsize = 0;
        if (ent->d_type == DT_DIR) {
            is_dir = true;
        } else if (ent->d_type == DT_REG) {
            is_dir = false;
            struct stat st = {};
            if (stat(fp, &st) == 0) fsize = static_cast<long long>(st.st_size);
        } else {  // DT_UNKNOWN — fall back to stat for both type and size
            struct stat st = {};
            is_dir = (stat(fp, &st) == 0) && S_ISDIR(st.st_mode);
            if (!is_dir) fsize = static_cast<long long>(st.st_size);
        }

        if (!first) ok = ok && buf_append_str(&buf, &len, &cap, ",");
        first = false;

        char num[32];
        snprintf(num, sizeof(num), "%lld", fsize);
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
    // Even the legacy single-item path is guarded so a protected entry can't be
    // removed. (Only files / empty dirs here; recursion goes through delete_recursive.)
    if (check_deletable(path) != DeleteGuard::kAllowed) {
        ESP_LOGW(kTag, "delete_entry refused (protected/bad): %s", path);
        return false;
    }
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

// ── Bulk / recursive delete ─────────────────────────────────────────────────────

SdManager::DeleteGuard SdManager::check_deletable(const char *path) {
    if (!safe_path(path)) return DeleteGuard::kBadPath;

    // Normalize: ignore a single trailing slash (except the mount root itself).
    char norm[260];
    snprintf(norm, sizeof(norm), "%s", path);
    const size_t len = strlen(norm);
    const size_t mp  = strlen(kMountPoint);
    if (len > mp && norm[len - 1] == '/') norm[len - 1] = '\0';

    if (strcmp(norm, kMountPoint) == 0) return DeleteGuard::kMountRoot;

    char managed[64];
    snprintf(managed, sizeof(managed), "%s/logs", kMountPoint);
    if (strcmp(norm, managed) == 0) return DeleteGuard::kManagedDir;
    snprintf(managed, sizeof(managed), "%s/rawdata", kMountPoint);
    if (strcmp(norm, managed) == 0) return DeleteGuard::kManagedDir;

    return DeleteGuard::kAllowed;
}

bool SdManager::is_deletable(const char *path) const {
    if (!mounted_) return false;
    if (check_deletable(path) != DeleteGuard::kAllowed) return false;
    struct stat st;
    return stat(path, &st) == 0;
}

void SdManager::preview_walk(const char *path, DeletePreview &pv) const {
    if (pv.files + pv.dirs >= kPreviewWalkCap) { pv.truncated = true; return; }

    struct stat st;
    if (stat(path, &st) != 0) return;

    if (S_ISDIR(st.st_mode)) {
        std::vector<std::string> names;
        DIR *d = opendir(path);
        if (!d) return;
        struct dirent *ent;
        while ((ent = readdir(d)) != nullptr) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            names.emplace_back(ent->d_name);
        }
        closedir(d);
        for (const std::string &name : names) {
            if (pv.files + pv.dirs >= kPreviewWalkCap) { pv.truncated = true; break; }
            std::string child = std::string(path) + "/" + name;
            preview_walk(child.c_str(), pv);
        }
        pv.dirs++;  // this directory's own shell
    } else {
        pv.files++;
        pv.bytes += static_cast<uint64_t>(st.st_size);
    }
}

SdManager::DeletePreview SdManager::preview_delete(const char *path) const {
    DeletePreview pv;
    if (!mounted_ || !path) return pv;
    struct stat st;
    if (stat(path, &st) != 0) return pv;  // ok stays false (not found)
    preview_walk(path, pv);
    // A managed dir is emptied but its shell is kept — don't count the shell.
    if (S_ISDIR(st.st_mode) &&
        check_deletable(path) == DeleteGuard::kManagedDir && pv.dirs > 0) {
        pv.dirs--;
    }
    pv.ok = true;
    return pv;
}

SdManager::DeletePreview SdManager::preview_delete_many(
    const char *const *paths, size_t n) const {
    DeletePreview total;
    if (!mounted_ || !paths) return total;
    for (size_t i = 0; i < n; ++i) {
        if (!paths[i]) continue;
        if (total.files + total.dirs >= kPreviewWalkCap) { total.truncated = true; break; }
        DeletePreview one = preview_delete(paths[i]);
        if (!one.ok) continue;
        total.files += one.files;
        total.dirs  += one.dirs;
        total.bytes += one.bytes;
        if (one.truncated) total.truncated = true;
    }
    total.ok = true;
    return total;
}

void SdManager::remove_tree(std::string &path, bool keep_shell,
                            DeleteResult &res, std::atomic<uint32_t> *progress) const {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) { note_fail(res, path.c_str()); return; }

    if (S_ISDIR(st.st_mode)) {
        // Defense in depth: re-validate every directory path before recursing.
        if (!safe_path(path.c_str())) { res.skipped++; return; }

        // Snapshot child names, then CLOSE the handle before recursing so we never
        // hold more than one DIR open at a time (FATFS max_files is small).
        std::vector<std::string> names;
        DIR *d = opendir(path.c_str());
        if (!d) { note_fail(res, path.c_str()); return; }
        struct dirent *ent;
        while ((ent = readdir(d)) != nullptr) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            names.emplace_back(ent->d_name);
        }
        closedir(d);

        const size_t base = path.size();
        for (const std::string &name : names) {
            path += '/';
            path += name;
            remove_tree(path, false, res, progress);  // children never keep their shell
            path.resize(base);
        }

        if (keep_shell) {
            res.skipped++;  // managed dir: contents removed, shell intentionally kept
        } else if (rmdir(path.c_str()) == 0) {
            res.deleted++;
            if (progress) progress->fetch_add(1, std::memory_order_relaxed);
        } else {
            note_fail(res, path.c_str());
        }
    } else {
        if (unlink(path.c_str()) == 0) {
            res.deleted++;
            if (progress) progress->fetch_add(1, std::memory_order_relaxed);
        } else {
            note_fail(res, path.c_str());
        }
    }
}

SdManager::DeleteResult SdManager::delete_recursive(
    const char *path, std::atomic<uint32_t> *progress) const {
    DeleteResult res;
    res.requested = 1;
    if (!mounted_ || !path) {
        res.skipped = 1;
        snprintf(res.first_error, sizeof(res.first_error), "SD not mounted");
        return res;
    }
    const DeleteGuard g = check_deletable(path);
    if (g == DeleteGuard::kBadPath || g == DeleteGuard::kMountRoot) {
        res.skipped = 1;
        snprintf(res.first_error, sizeof(res.first_error), "refused: %s", path);
        return res;
    }
    std::string p(path);
    remove_tree(p, g == DeleteGuard::kManagedDir, res, progress);
    return res;
}

SdManager::DeleteResult SdManager::delete_paths(
    const char *const *paths, size_t n, std::atomic<uint32_t> *progress) const {
    DeleteResult res;
    res.requested = static_cast<uint32_t>(n);
    if (!mounted_ || !paths) {
        res.skipped = static_cast<uint32_t>(n);
        snprintf(res.first_error, sizeof(res.first_error), "SD not mounted");
        return res;
    }

    // Atomic pre-validation: any bad / mount-root path rejects the WHOLE batch so a
    // single crafted path can't slip through a partial destructive run.
    for (size_t i = 0; i < n; ++i) {
        const DeleteGuard g = paths[i] ? check_deletable(paths[i]) : DeleteGuard::kBadPath;
        if (g == DeleteGuard::kBadPath || g == DeleteGuard::kMountRoot) {
            res.skipped = static_cast<uint32_t>(n);
            snprintf(res.first_error, sizeof(res.first_error), "refused: %s",
                     paths[i] ? paths[i] : "(null)");
            return res;  // nothing touched
        }
    }

    // Execute best-effort, continue-on-error.
    for (size_t i = 0; i < n; ++i) {
        const DeleteGuard g = check_deletable(paths[i]);
        std::string p(paths[i]);
        remove_tree(p, g == DeleteGuard::kManagedDir, res, progress);
    }
    return res;
}
