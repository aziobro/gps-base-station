// Route all LVGL heap allocations to PSRAM so the 64 KB internal-SRAM built-in
// pool is not exhausted by complex UI widgets (keyboard, modals, file browser).
// Called when CONFIG_LV_USE_CUSTOM_MALLOC=y is set.

#include "esp_heap_caps.h"

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

void *lv_malloc_core(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lv_free_core(void *p) {
    heap_caps_free(p);
}

void *lv_realloc_core(void *p, size_t new_size) {
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
