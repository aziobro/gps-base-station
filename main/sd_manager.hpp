#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

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

    // ── Bulk / recursive delete ──────────────────────────────────────────────────
    // See docs/redesign/03_bulk_sd_management_design.md §1. POD results, no exceptions.
    struct DeleteResult {
        uint32_t requested = 0;   // paths/entries the caller asked to delete
        uint32_t deleted   = 0;   // files + dirs actually removed
        uint32_t failed    = 0;   // entries that could not be removed
        uint32_t skipped   = 0;   // protected / rejected-by-guard entries
        char     first_error[96] = {0};  // first failing path + errno; "" if none
    };
    struct DeletePreview {
        bool     ok        = false;  // false => not mounted / bad path / stat error
        uint32_t files     = 0;
        uint32_t dirs      = 0;
        uint64_t bytes     = 0;
        bool     truncated = false;  // walk stopped at kPreviewWalkCap (huge tree)
    };
    enum class DeleteGuard : uint8_t {
        kAllowed = 0,
        kBadPath,     // safe_path() failed (empty / outside /sdcard / contains "..")
        kMountRoot,   // path == /sdcard
        kManagedDir,  // /sdcard/logs or /sdcard/rawdata (the shell itself)
    };

    static constexpr size_t kPreviewWalkCap  = 4096;  // bound inline preview cost
    static constexpr size_t kInlineDeleteMax = 16;    // <= this: delete inline (UI hint)

    // Pure validator: re-runs safe_path() then the protected-path deny-list. Static so
    // any layer can pre-flight a path. Does NOT check mount state or existence.
    static DeleteGuard check_deletable(const char *path);
    // True iff check_deletable()==kAllowed, the card is mounted, and the entry exists.
    bool is_deletable(const char *path) const;

    // Recursively count what a delete would remove, WITHOUT deleting. Bounded by
    // kPreviewWalkCap (sets truncated=true past the cap). Feeds the confirm dialog.
    DeletePreview preview_delete(const char *path) const;
    DeletePreview preview_delete_many(const char *const *paths, size_t n) const;

    // Recursive single-path delete. A managed dir (logs/rawdata) is emptied but its
    // shell is kept. Continue-on-error; optional atomic progress (one increment per
    // removed entry, may be null). NEVER call on the LVGL or httpd task for large trees.
    DeleteResult delete_recursive(const char *path,
                                  std::atomic<uint32_t> *progress = nullptr) const;
    // Batch multi-path delete (the primary bulk entry point). Pre-validation is ATOMIC:
    // if ANY path is bad / the mount root, the whole batch is rejected and nothing is
    // touched. After validation, deletes best-effort, continue-on-error.
    DeleteResult delete_paths(const char *const *paths, size_t n,
                              std::atomic<uint32_t> *progress = nullptr) const;

private:
    // DFS removal of the tree rooted at `path` (used in place as a scratch buffer).
    // keep_shell=true empties a directory but leaves the directory itself (managed dirs).
    void remove_tree(std::string &path, bool keep_shell,
                     DeleteResult &res, std::atomic<uint32_t> *progress) const;
    // DFS counter for preview_delete (bounded by kPreviewWalkCap).
    void preview_walk(const char *path, DeletePreview &pv) const;

    sdmmc_card_t *card_ = nullptr;
    bool mounted_ = false;
    bool spi_bus_initialized_ = false;
};
