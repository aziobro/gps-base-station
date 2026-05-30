#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include "config.h"

// Maximum simultaneous NTRIP clients
#define MAX_CLIENTS 4

class NtripCaster {
public:
    void begin() {
        _server = new WiFiServer(NTRIP_PORT);
        _server->begin();
        Serial.printf("[NTRIP] Caster listening on port %d, mountpoint /%s\n",
                      NTRIP_PORT, NTRIP_MOUNTPOINT);
    }

    // Call from loop() — accepts new clients and pushes data to existing ones.
    void update(const uint8_t *rtcmBuf, size_t len) {
        acceptClients();
        if (len > 0) {
            broadcastRTCM(rtcmBuf, len);
        }
    }

    int clientCount() {
        int n = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (_clients[i].connected()) n++;
        }
        return n;
    }

private:
    WiFiServer *_server = nullptr;
    WiFiClient  _clients[MAX_CLIENTS];

    void acceptClients() {
        if (!_server->hasClient()) return;

        WiFiClient newClient = _server->accept();
        if (!newClient) return;

        // Read the HTTP-style NTRIP request (with timeout)
        String request = readRequest(newClient);

        if (request.indexOf("GET /") == -1) {
            newClient.stop();
            return;
        }

        // Check mountpoint
        String mountLine = "GET /" + String(NTRIP_MOUNTPOINT);
        if (request.indexOf(mountLine) == -1) {
            // Return source table
            sendSourceTable(newClient);
            newClient.stop();
            return;
        }

        // Optional password check (Basic Auth in NTRIP v1 is Base64, but many
        // rovers just send plain "Authorization: Basic <base64>".
        // For simplicity we accept any client when NTRIP_PASSWORD is "".
#ifndef NTRIP_PASSWORD
        // no auth
#else
        if (strlen(NTRIP_PASSWORD) > 0) {
            if (request.indexOf("Authorization:") == -1) {
                newClient.print("HTTP/1.1 401 Unauthorized\r\n"
                                "WWW-Authenticate: Basic realm=\"NTRIP\"\r\n\r\n");
                newClient.stop();
                return;
            }
            // Full Base64 validation omitted for embedded simplicity;
            // add if security is required on an open network.
        }
#endif

        // Find a free slot
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!_clients[i] || !_clients[i].connected()) {
                slot = i;
                break;
            }
        }

        if (slot == -1) {
            newClient.print("SOURCETABLE 200 OK\r\nContent-Length: 0\r\n\r\n");
            newClient.stop();
            Serial.println("[NTRIP] Client rejected — slots full.");
            return;
        }

        // Send NTRIP v1 OK response
        newClient.print("ICY 200 OK\r\n\r\n");
        _clients[slot] = newClient;
        Serial.printf("[NTRIP] Client %d connected from %s\n",
                      slot, newClient.remoteIP().toString().c_str());
    }

    void broadcastRTCM(const uint8_t *buf, size_t len) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!_clients[i] || !_clients[i].connected()) continue;
            if (_clients[i].write(buf, len) == 0) {
                Serial.printf("[NTRIP] Client %d stalled, disconnecting.\n", i);
                _clients[i].stop();
            }
        }
    }

    void sendSourceTable(WiFiClient &client) {
        String entry = "STR;" + String(NTRIP_MOUNTPOINT) +
                       ";BASE;RTCM 3.2;1005(5),1074(1),1084(1),1094(1),1124(1)"
                       ";2;GPS+GLO+GAL+BDS;NONE;NONE;0;0;SNIP;NONE;0;0;1;0;xN;N;0\r\n";
        String body = entry + "ENDSOURCETABLE\r\n";
        client.print("SOURCETABLE 200 OK\r\n");
        client.print("Content-Type: text/plain\r\n");
        client.printf("Content-Length: %d\r\n\r\n", body.length());
        client.print(body);
    }

    String readRequest(WiFiClient &client) {
        String req = "";
        unsigned long t = millis();
        while (millis() - t < 1000) {
            while (client.available()) {
                char c = client.read();
                req += c;
                if (req.endsWith("\r\n\r\n")) return req;
            }
            yield();  // let WiFi stack process while waiting
        }
        return req;
    }
};
