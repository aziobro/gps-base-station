#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

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
          _label(label), _proto(proto) {}

    // Call once after WiFi is up — creates the queue and starts the task on Core 0
    void startTask() {
        _queue = xQueueCreate(QUEUE_DEPTH, sizeof(Packet));
        xTaskCreatePinnedToCore(taskEntry, _label, TASK_STACK,
                                this, 1, &_taskHandle, 0);
    }

    // Non-blocking — drops oldest packet if queue full (RTCM is time-sensitive)
    void push(const uint8_t *data, size_t len) {
        if (!_queue || _suspended || len == 0) return;
        Packet pkt;
        pkt.len = (uint16_t)min(len, (size_t)MAX_PKT_BYTES);
        memcpy(pkt.data, data, pkt.len);
        if (xQueueSend(_queue, &pkt, 0) != pdTRUE) {
            Packet discard;
            xQueueReceive(_queue, &discard, 0);
            xQueueSend(_queue, &pkt, 0);
        }
    }

    void setCredentials(const String &mountpoint, const String &password) {
        _mountpoint = mountpoint;
        _password   = password;
        _reconnect  = true;
    }

    // Suspend sending (survey mode) — disconnects and drains the queue
    void disconnect() {
        _suspended = true;
        if (_queue) {
            Packet discard;
            while (xQueueReceive(_queue, &discard, 0) == pdTRUE) {}
        }
    }

    // Resume sending (base TX mode)
    void resume() { _suspended = false; }

    // Safe to read from main loop (volatile primitives; String races are benign for display)
    bool     connected()      const { return _connected; }
    String   lastStatus()     const { return _lastStatus; }
    String   lastError()      const { return _lastError; }
    uint32_t bytesSent()      const { return _bytesSent; }
    void     clearBytesSent()       { _bytesSent = 0; }

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

    volatile bool     _connected  = false;
    volatile bool     _suspended  = false;
    volatile bool     _reconnect  = false;
    volatile uint32_t _bytesSent  = 0;

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
                _connected = false;
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            if (_reconnect) {
                _client.stop();
                _connected = false;
                _reconnect = false;
            }

            if (!_client.connected()) {
                _connected = false;
                doConnect();
                continue;
            }

            // Block up to 2 s waiting for a packet, then loop to check connection
            if (xQueueReceive(_queue, &pkt, pdMS_TO_TICKS(2000)) != pdTRUE) {
                continue;
            }

            size_t written = _client.write(pkt.data, pkt.len);
            _bytesSent += written;

            if (written == 0) {
                if (++_stalls >= 5) {
                    setError("Send stalled x5 — reconnecting");
                    _client.stop();
                    _connected = false;
                    _stalls = 0;
                }
            } else {
                _stalls = 0;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Connection (runs inside the task — blocking is fine here)
    // -------------------------------------------------------------------------
    void doConnect() {
        if (_mountpoint.length() == 0) {
            _lastStatus = "no mountpoint configured";
            vTaskDelay(pdMS_TO_TICKS(5000));
            return;
        }
        if (WiFi.status() != WL_CONNECTED) {
            _lastStatus = "waiting for WiFi";
            vTaskDelay(pdMS_TO_TICKS(1000));
            return;
        }

        unsigned long now = millis();
        if (now - _lastAttemptMs < _retryIntervalMs) {
            vTaskDelay(pdMS_TO_TICKS(500));
            return;
        }
        _lastAttemptMs = now;

        _lastStatus = "connecting";
        Serial.printf("[%s] Connecting to %s:%d/%s (v%s)\n",
                      _label, _host, _port, _mountpoint.c_str(),
                      _proto == Protocol::V2 ? "2" : "1");

        _client.setTimeout(3000);
        if (!_client.connect(_host, _port)) {
            setError("TCP connect failed");
            return;
        }

        if (_proto == Protocol::V2) connectV2();
        else                        connectV1();
    }

    void connectV1() {
        _client.printf("SOURCE %s /%s\r\n", _password.c_str(), _mountpoint.c_str());
        _client.printf("Source-Agent: NTRIP ESP32BaseStation/1.0\r\n");
        _client.printf("\r\n");

        String line = readLine(3000);
        Serial.printf("[%s] << %s\n", _label, line.c_str());

        if (line.startsWith("ICY 200 OK")) {
            setConnected("connected (v1)");
        } else {
            setError("rejected: " + line);
            _client.stop();
        }
    }

    void connectV2() {
        String auth = base64Encode(_mountpoint + ":" + _password);
        _client.printf("POST /%s HTTP/1.1\r\n",       _mountpoint.c_str());
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
            while (true) {
                String hdr = readLine(1000);
                if (hdr.length() == 0) break;
                Serial.printf("[%s] hdr: %s\n", _label, hdr.c_str());
            }
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
        _lastError  = msg;
        _lastStatus = msg;
        _failCount++;
        _retryIntervalMs = min(RETRY_BASE_MS << min(_failCount, 3), RETRY_MAX_MS);
        Serial.printf("[%s] Error: %s (retry in %lus)\n",
                      _label, msg.c_str(), _retryIntervalMs / 1000);
    }

    void setConnected(const String &msg) {
        _lastError       = "";
        _lastStatus      = msg;
        _connected       = true;
        _failCount       = 0;
        _retryIntervalMs = RETRY_BASE_MS;
        Serial.printf("[%s] %s\n", _label, msg.c_str());
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
