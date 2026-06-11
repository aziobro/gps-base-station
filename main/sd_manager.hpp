#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"

// Manages the microSD card on the Waveshare ESP32-P4-WIFI6 board via SPI.
// SDMMC native mode is unavailable because esp_hosted (WiFi SDIO) holds the
// SDMMC peripheral exclusively in IDF 6.x. SDSPI uses the independent SPI2
// host on the same physical card pins (CLK=43, MOSI=44, MISO=39, CS=42).
class SdManager {
public:
    static constexpr const char *kMountPoint = "/sdcard";

    ~SdManager();

    SdManager(const SdManager &) = delete;
    SdManager &operator=(const SdManager &) = delete;
    SdManager() = default;

    struct DiskStats {
        uint64_t total_bytes = 0;
        uint64_t used_bytes  = 0;
        bool valid = false;
    };

    esp_err_t mount();
    void unmount();
    bool is_mounted() const { return mounted_; }
    DiskStats disk_stats() const;

    // Returns path if safely under mount point and free of traversal sequences.
    static const char *safe_path(const char *path);

    // Returns heap-allocated JSON array of directory entries. Caller must free().
    char *list_dir(const char *path) const;

    bool delete_entry(const char *path) const;
    bool rename_entry(const char *from, const char *to) const;
    void ensure_dirs() const;

private:
    sdmmc_card_t *card_ = nullptr;
    bool mounted_ = false;
    bool spi_bus_initialized_ = false;
};
