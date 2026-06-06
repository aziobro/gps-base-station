#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// Pushes RTCM3 data to an upstream NTRIP caster (RTK2go, Onocoy, etc.)
//
// Each client runs on its own FreeRTOS task (Core 0).  The main loop
// calls push() which is non-blocking — it enqueues the packet and returns
// immediately.  All TCP connect/reconnect/write work happens in the task.
//
// Protocol:
//   V1 (RTK2go, RTKdata):  SOURCE <pw> /<mp>   -> expects "ICY 200 OK"
//   V2 (Onocoy):           POST /<mp> HTTP/1.1 -> expects "HTTP/1.1 200"

class NtripPushClient {
public:
    enum class Protocol { V1, V2 };

    static constexpr int  QUEUE_DEPTH   = 12;
    static constexpr int  MAX_PKT_BYTES = 1024;
    static constexpr int  TASK_STACK    = 5120;

    struct Packet {
        uint8_t  data[MAX_PKT_BYTES];
        uint16_t len;
    };

    NtripPushClient(const char *host, uint16_t port,
                    const char *mountpoint, const char *password,
                    const char *label, Protocol proto = Protocol::V1)
        : _host(host), _port(port),
          _mountpoint(mountpoint), _password(password),
          _label(label), _proto(proto),
          _configured(mountpoint && mountpoint[0] != '\0') {}

    // Call once after WiFi is up — creates the queue and starts the task on Core 0
    bool startTask() {
        _mutex = xSemaphoreCreateMutex();
        _queue = xQueueCreate(QUEUE_DEPTH, sizeof(Packet));
        if (!_mutex || !_queue) {
            Serial.printf("[%s] Failed to allocate task resources.\n", _label);
            return false;
        }
        BaseType_t created = xTaskCreatePinnedToCore(
            taskEntry, _label, TASK_STACK, this, 1, &_taskHandle, 0);
        if (created != pdPASS) {
            Serial.printf("[%s] Failed to create task.\n", _label);
            _taskHandle = nullptr;
            return false;
        }
        return true;
    }

    // Non-blocking. If the queue overflows, reconnect rather than continue a
    // TCP stream after silently dropping arbitrary RTCM bytes.
    void push(const uint8_t *data, size_t len) {
        if (!_queue || _suspended || _reconnect || !_configured || len == 0) return;
        Packet pkt;
        pkt.len = (uint16_t)min(len, (size_t)MAX_PKT_BYTES);
        memcpy(pkt.data, data, pkt.len);
        if (xQueueSend(_queue, &pkt, 0) != pdTRUE) {
            _reconnect = true;
        }
    }

    void setCredentials(const String &mountpoint, const String &password) {
        lock();
        _mountpoint = mountpoint;
        _password = password;
        _configured = mountpoint.length() > 0;
        unlock();
        _reconnect  = true;
    }

    // Suspend sending and wait until the worker has closed the socket.
    void requestSuspend() {
        _suspendAck = false;
        _suspended = true;
    }

    bool suspendAndWait(uint32_t timeoutMs = 2000) {
        requestSuspend();
        uint32_t started = millis();
        while (!_suspendAck && millis() - started < timeoutMs) {
            delay(10);
        }
        return _suspendAck;
    }

    // Resume sending (base TX mode)
    void resume() {
        _suspendAck = false;
        _suspended = false;
    }

    bool connected() const {
        lock();
        bool value = _connected;
        unlock();
        return value;
    }

    String lastStatus() const {
        lock();
        String value = _lastStatus;
        unlock();
        return value;
    }

    uint32_t takeBytesSent() {
        lock();
        uint32_t value = _bytesSent;
        _bytesSent = 0;
        unlock();
        return value;
    }

    // Stack headroom in bytes — low values (<512) indicate risk of stack overflow
    uint32_t stackWatermark() const {
        if (!_taskHandle) return 0;
        return uxTaskGetStackHighWaterMark(_taskHandle) * sizeof(StackType_t);
    }

private:
    const char   *_host;
    uint16_t      _port;
    String        _mountpoint;
    String        _password;
    const char   *_label;
    Protocol      _proto;

    WiFiClient    _client;
    QueueHandle_t _queue      = nullptr;
    TaskHandle_t  _taskHandle = nullptr;
    mutable SemaphoreHandle_t _mutex = nullptr;

    bool              _connected  = false;
    volatile bool     _suspended  = false;
    volatile bool     _suspendAck = false;
    volatile bool     _reconnect  = false;
    volatile bool     _configured = false;
    uint32_t          _bytesSent  = 0;

    String        _lastStatus;
    String        _lastError;
    unsigned long _lastAttemptMs   = 0;
    unsigned long _retryIntervalMs = 10000;
    int           _failCount       = 0;
    int           _stalls          = 0;

    static constexpr unsigned long RETRY_BASE_MS = 10000;
    static constexpr unsigned long RETRY_MAX_MS  = 120000;

    // -------------------------------------------------------------------------
    // Task
    // -------------------------------------------------------------------------
    static void taskEntry(void *arg) {
        ((NtripPushClient *)arg)->run();
    }

    void run() {
        Packet pkt;
        while (true) {
            if (_suspended) {
                if (_client.connected()) _client.stop();
                setConnectedState(false);
                if (_queue) xQueueReset(_queue);
                _suspendAck = true;
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            if (_reconnect) {
                _client.stop();
                setConnectedState(false);
                if (_queue) xQueueReset(_queue);
                _reconnect = false;
            }

            if (!_client.connected()) {
                if (connected()) {
                    setError("connection closed - retrying");
                } else {
                    setConnectedState(false);
                }
                doConnect();
                continue;
            }

            // Block up to 2 s waiting for a packet, then loop to check connection
            if (xQueueReceive(_queue, &pkt, pdMS_TO_TICKS(2000)) != pdTRUE) {
                continue;
            }

            if (_suspended) continue;
            writePacket(pkt);
        }
    }

    void writePacket(const Packet &pkt) {
        size_t offset = 0;
        while (offset < pkt.len && !_suspended && _client.connected()) {
            size_t written = _client.write(pkt.data + offset, pkt.len - offset);
            if (written > 0) {
                offset += written;
                _stalls = 0;
                lock();
                _bytesSent += written;
                unlock();
                continue;
            }

            if (++_stalls >= 5) {
                setError("send stalled x5 - reconnecting");
                _client.stop();
                setConnectedState(false);
                _stalls = 0;
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    // -------------------------------------------------------------------------
    // Connection (runs inside the task — blocking is fine here)
    // -------------------------------------------------------------------------
    void doConnect() {
        String mountpoint;
        String password;
        lock();
        mountpoint = _mountpoint;
        password = _password;
        unlock();

        if (mountpoint.length() == 0) {
            setStatus("no mountpoint configured");
            vTaskDelay(pdMS_TO_TICKS(5000));
            return;
        }
        if (WiFi.status() != WL_CONNECTED) {
            setStatus("waiting for WiFi");
            vTaskDelay(pdMS_TO_TICKS(1000));
            return;
        }

        unsigned long now = millis();
        if (now - _lastAttemptMs < _retryIntervalMs) {
            vTaskDelay(pdMS_TO_TICKS(500));
            return;
        }
        _lastAttemptMs = now;

        setStatus("connecting");
        Serial.printf("[%s] Connecting to %s:%d/%s (v%s)\n",
                      _label, _host, _port, mountpoint.c_str(),
                      _proto == Protocol::V2 ? "2" : "1");

        _client.setTimeout(3000);
        if (!_client.connect(_host, _port)) {
            setError("TCP connect failed");
            return;
        }

        if (_proto == Protocol::V2) connectV2(mountpoint, password);
        else                        connectV1(mountpoint, password);
    }

    void connectV1(const String &mountpoint, const String &password) {
        _client.printf("SOURCE %s /%s\r\n", password.c_str(), mountpoint.c_str());
        _client.printf("Source-Agent: NTRIP ESP32BaseStation/1.0\r\n");
        _client.printf("\r\n");

        String line = readLine(3000);
        Serial.printf("[%s] << %s\n", _label, line.c_str());

        if (line.startsWith("ICY 200 OK") ||
            line.startsWith("HTTP/1.1 200") ||
            line.startsWith("HTTP/1.0 200")) {
            if (line.startsWith("HTTP/")) {
                drainHeaders();
            }
            setConnected("connected (v1)");
        } else {
            setError("rejected: " + line);
            _client.stop();
        }
    }

    void connectV2(const String &mountpoint, const String &password) {
        String auth = base64Encode(mountpoint + ":" + password);
        _client.printf("POST /%s HTTP/1.1\r\n",       mountpoint.c_str());
        _client.printf("Host: %s:%d\r\n",             _host, _port);
        _client.printf("Ntrip-Version: Ntrip/2.0\r\n");
        _client.printf("User-Agent: NTRIP ESP32BaseStation/1.0\r\n");
        _client.printf("Authorization: Basic %s\r\n", auth.c_str());
        _client.printf("Content-Type: application/octet-stream\r\n");
        _client.printf("Connection: keep-alive\r\n");
        _client.printf("\r\n");

        String line = readLine(3000);
        Serial.printf("[%s] << %s\n", _label, line.c_str());

        if (line.startsWith("HTTP/1.1 200") || line.startsWith("HTTP/1.0 200")
                || line.startsWith("ICY 200")) {
            if (line.startsWith("HTTP/")) drainHeaders();
            setConnected("connected (v2)");
        } else {
            setError("rejected: " + line);
            _client.stop();
        }
    }

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------
    void setError(const String &msg) {
        lock();
        _lastError  = msg;
        _lastStatus = msg;
        _connected = false;
        unlock();
        _failCount++;
        _retryIntervalMs = min(RETRY_BASE_MS << min(_failCount, 3), RETRY_MAX_MS);
        Serial.printf("[%s] Error: %s (retry in %lus)\n",
                      _label, msg.c_str(), _retryIntervalMs / 1000);
    }

    void setConnected(const String &msg) {
        lock();
        _lastError       = "";
        _lastStatus      = msg;
        _connected       = true;
        unlock();
        _failCount       = 0;
        _retryIntervalMs = RETRY_BASE_MS;
        Serial.printf("[%s] %s\n", _label, msg.c_str());
    }

    void setStatus(const String &msg) {
        lock();
        _lastStatus = msg;
        unlock();
    }

    void setConnectedState(bool connected) {
        lock();
        _connected = connected;
        unlock();
    }

    void lock() const {
        if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    }

    void unlock() const {
        if (_mutex) xSemaphoreGive(_mutex);
    }

    String readLine(uint32_t timeoutMs) {
        String line;
        uint32_t t = millis();
        while (millis() - t < timeoutMs) {
            while (_client.available()) {
                char c = _client.read();
                if (c == '\n') { line.trim(); return line; }
                if (c != '\r') line += c;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        line.trim();
        return line;
    }

    void drainHeaders() {
        while (true) {
            String header = readLine(1000);
            if (header.length() == 0) break;
            Serial.printf("[%s] hdr: %s\n", _label, header.c_str());
        }
    }

    static String base64Encode(const String &input) {
        static const char *b64 =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const uint8_t *d = (const uint8_t *)input.c_str();
        int len = input.length();
        String out;
        for (int i = 0; i < len; i += 3) {
            uint32_t b  = (uint32_t)d[i] << 16;
            if (i+1 < len) b |= (uint32_t)d[i+1] << 8;
            if (i+2 < len) b |= (uint32_t)d[i+2];
            out += b64[(b >> 18) & 0x3F];
            out += b64[(b >> 12) & 0x3F];
            out += (i+1 < len) ? b64[(b >> 6) & 0x3F] : '=';
            out += (i+2 < len) ? b64[(b >> 0) & 0x3F] : '=';
        }
        return out;
    }
};
