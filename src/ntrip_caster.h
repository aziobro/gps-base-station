#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "config.h"

#define MAX_CLIENTS 4

class NtripCaster {
public:
    static constexpr int QUEUE_DEPTH = 12;
    static constexpr int MAX_PKT_BYTES = 1024;
    static constexpr int TASK_STACK = 5120;

    struct Packet {
        uint8_t data[MAX_PKT_BYTES];
        uint16_t len;
    };

    bool begin() {
        _queue = xQueueCreate(QUEUE_DEPTH, sizeof(Packet));
        if (!_queue) {
            Serial.println("[NTRIP] Failed to allocate local caster queue.");
            return false;
        }

        _server = new WiFiServer(NTRIP_PORT);
        if (!_server) {
            Serial.println("[NTRIP] Failed to allocate local caster server.");
            return false;
        }
        _server->begin();

        BaseType_t created = xTaskCreatePinnedToCore(
            taskEntry, "LocalNTRIP", TASK_STACK, this, 1, &_taskHandle, 0);
        if (created != pdPASS) {
            Serial.println("[NTRIP] Failed to create local caster task.");
            _taskHandle = nullptr;
            return false;
        }

        Serial.printf("[NTRIP] Caster listening on port %d, mountpoint /%s\n",
                      NTRIP_PORT, NTRIP_MOUNTPOINT);
        return true;
    }

    // Non-blocking. Network writes are performed by the Core 0 worker.
    void update(const uint8_t *rtcmBuf, size_t len) {
        if (!_queue || _suspendRequested || _suspended || !rtcmBuf || len == 0) return;

        Packet pkt;
        pkt.len = (uint16_t)min(len, (size_t)MAX_PKT_BYTES);
        memcpy(pkt.data, rtcmBuf, pkt.len);

        if (xQueueSend(_queue, &pkt, 0) != pdTRUE) {
            // Dropping arbitrary stream bytes would corrupt all clients. Force
            // clean TCP reconnects. The worker resets the queue on its core.
            _resetClients = true;
        }
    }

    int clientCount() const { return _clientCount; }

    void requestSuspend() {
        _suspendRequested = true;
    }

    bool suspendAndWait(uint32_t timeoutMs = 2000) {
        if (!_taskHandle) return true;
        requestSuspend();
        uint32_t started = millis();
        while (!_suspended && millis() - started < timeoutMs) {
            delay(10);
        }
        return _suspended;
    }

    void resume() {
        _suspendRequested = false;
    }

private:
    WiFiServer *_server = nullptr;
    WiFiClient _clients[MAX_CLIENTS];
    QueueHandle_t _queue = nullptr;
    TaskHandle_t _taskHandle = nullptr;
    volatile int _clientCount = 0;
    volatile bool _resetClients = false;
    volatile bool _suspendRequested = false;
    volatile bool _suspended = false;

    static void taskEntry(void *arg) {
        static_cast<NtripCaster *>(arg)->run();
    }

    void run() {
        Packet pkt;
        while (true) {
            if (_suspendRequested) {
                stopAllClients();
                xQueueReset(_queue);
                _suspended = true;
                while (_suspendRequested) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                _suspended = false;
                continue;
            }

            if (_resetClients) {
                stopAllClients();
                xQueueReset(_queue);
                _resetClients = false;
            }

            acceptClient();

            if (xQueueReceive(_queue, &pkt, pdMS_TO_TICKS(20)) == pdTRUE) {
                broadcastRTCM(pkt.data, pkt.len);
            }
        }
    }

    void acceptClient() {
        if (!_server || !_server->hasClient()) return;

        WiFiClient newClient = _server->accept();
        if (!newClient) return;

        String request = readRequest(newClient);
        if (request.indexOf("GET /") == -1) {
            newClient.stop();
            return;
        }

        int requestLineEnd = request.indexOf("\r\n");
        String requestLine = requestLineEnd >= 0
            ? request.substring(0, requestLineEnd)
            : request;
        String expectedV1 = "GET /" + String(NTRIP_MOUNTPOINT) + " HTTP/1.0";
        String expectedV11 = "GET /" + String(NTRIP_MOUNTPOINT) + " HTTP/1.1";
        if (requestLine != expectedV1 && requestLine != expectedV11) {
            sendSourceTable(newClient);
            newClient.stop();
            return;
        }

        if (strlen(NTRIP_PASSWORD) > 0 && !validAuthorization(request)) {
            newClient.print("HTTP/1.1 401 Unauthorized\r\n"
                            "WWW-Authenticate: Basic realm=\"NTRIP\"\r\n\r\n");
            newClient.stop();
            return;
        }

        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!_clients[i] || !_clients[i].connected()) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            newClient.print("SOURCETABLE 200 OK\r\nContent-Length: 0\r\n\r\n");
            newClient.stop();
            Serial.println("[NTRIP] Client rejected - slots full.");
            return;
        }

        newClient.print("ICY 200 OK\r\n\r\n");
        _clients[slot] = newClient;
        updateClientCount();
        Serial.printf("[NTRIP] Client %d connected from %s\n",
                      slot, newClient.remoteIP().toString().c_str());
    }

    void broadcastRTCM(const uint8_t *buf, size_t len) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!_clients[i] || !_clients[i].connected()) continue;
            if (!writeAll(_clients[i], buf, len, 1000)) {
                Serial.printf("[NTRIP] Client %d stalled, disconnecting.\n", i);
                _clients[i].stop();
            }
        }
        updateClientCount();
    }

    static bool writeAll(WiFiClient &client, const uint8_t *buf,
                         size_t len, uint32_t timeoutMs) {
        size_t offset = 0;
        uint32_t started = millis();
        while (offset < len && client.connected()) {
            size_t written = client.write(buf + offset, len - offset);
            if (written > 0) {
                offset += written;
                started = millis();
                continue;
            }
            if (millis() - started >= timeoutMs) return false;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return offset == len;
    }

    bool validAuthorization(const String &request) const {
        const String prefix = "Authorization: Basic ";
        int start = request.indexOf(prefix);
        if (start < 0) return false;
        start += prefix.length();
        int end = request.indexOf("\r\n", start);
        if (end < 0) return false;

        String decoded = base64Decode(request.substring(start, end));
        int separator = decoded.indexOf(':');
        if (separator < 0) return false;
        return decoded.substring(separator + 1) == NTRIP_PASSWORD;
    }

    static int base64Value(char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    }

    static String base64Decode(const String &input) {
        String output;
        uint32_t accumulator = 0;
        int bits = 0;
        for (size_t i = 0; i < input.length(); i++) {
            if (input[i] == '=') break;
            int value = base64Value(input[i]);
            if (value < 0) return "";
            accumulator = (accumulator << 6) | (uint32_t)value;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                output += (char)((accumulator >> bits) & 0xff);
            }
        }
        return output;
    }

    void stopAllClients() {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (_clients[i]) _clients[i].stop();
        }
        updateClientCount();
    }

    void updateClientCount() {
        int count = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (_clients[i] && _clients[i].connected()) count++;
        }
        _clientCount = count;
    }

    void sendSourceTable(WiFiClient &client) {
        String entry = "STR;" + String(NTRIP_MOUNTPOINT) +
                       ";BASE;RTCM 3.2;1005(5),1074(1),1084(1),1094(1),1124(1)"
                       ";2;GPS+GLO+GAL+BDS;NONE;NONE;0;0;ESP32;NONE;0;0;1;0;xN;N;0\r\n";
        String body = entry + "ENDSOURCETABLE\r\n";
        client.print("SOURCETABLE 200 OK\r\n");
        client.print("Content-Type: text/plain\r\n");
        client.printf("Content-Length: %d\r\n\r\n", body.length());
        client.print(body);
    }

    static String readRequest(WiFiClient &client) {
        String request;
        uint32_t started = millis();
        while (millis() - started < 1000) {
            while (client.available()) {
                char c = client.read();
                request += c;
                if (request.endsWith("\r\n\r\n")) return request;
                if (request.length() > 2048) return request;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        return request;
    }
};
