#pragma once

#include <string>

// In-memory ring buffer that captures ESP-IDF console output so it can be
// viewed remotely (e.g. on the /logs web page) for debugging without a serial
// cable attached. The captured text is still forwarded to the original UART
// logger, so the serial monitor keeps working unchanged.
namespace log_buffer {

// Installs the esp_log vprintf hook. Call once early in app_main(), before
// other subsystems start logging, so their output is captured too.
void init();

// Returns a copy of the currently buffered log text (oldest first). Safe to
// call from any task; takes the buffer lock briefly.
std::string snapshot();

}  // namespace log_buffer
