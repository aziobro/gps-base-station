#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "storage.h"
#include "config.h"

// ---------------------------------------------------------------------------
// WiFiManager
//
// Boot sequence:
//   1. Try saved NVS credentials
//   2. Try compiled-in WIFI_SSID / WIFI_PASSWORD from config.h (if set)
//   3. If both fail → start AP "GPS-BaseStation" with captive portal
//
// In AP mode the provisioning server runs at 192.168.4.1.
// User scans for networks, picks one, enters password → credentials saved
// to NVS and device restarts in STA mode.
// ---------------------------------------------------------------------------

static constexpr const char *AP_SSID     = "GPS-BaseStation";
static constexpr uint16_t    DNS_PORT    = 53;
static constexpr uint32_t    CONNECT_TIMEOUT_MS = 15000;

class WiFiManager {
public:
    enum class State { CONNECTED, AP_MODE };

    // Call once from setup(). Returns CONNECTED or AP_MODE.
    State begin(Storage &storage) {
        _storage = &storage;

        // 1. Try NVS credentials
        auto saved = storage.loadWiFi();
        if (saved.valid) {
            Serial.printf("[WiFi] Trying saved SSID: %s\n", saved.ssid.c_str());
            if (tryConnect(saved.ssid.c_str(), saved.password.c_str()))
                return State::CONNECTED;
            Serial.println("[WiFi] Saved credentials failed.");
        }

        // 2. Try compiled-in defaults (only if WIFI_SSID is non-empty)
        if (strlen(WIFI_SSID) > 0 && String(WIFI_SSID) != saved.ssid) {
            Serial.printf("[WiFi] Trying config.h SSID: %s\n", WIFI_SSID);
            if (tryConnect(WIFI_SSID, WIFI_PASSWORD)) {
                storage.saveWiFi(WIFI_SSID, WIFI_PASSWORD);
                return State::CONNECTED;
            }
            Serial.println("[WiFi] config.h credentials failed.");
        }

        // 3. AP mode
        startAP();
        return State::AP_MODE;
    }

    // Call from loop() while in AP_MODE.
    void update() {
        _dns.processNextRequest();
        _server.handleClient();
    }

    String ip() {
        return WiFi.localIP().toString();
    }

private:
    Storage   *_storage = nullptr;
    DNSServer  _dns;
    WebServer  _server{80};

    // ------------------------------------------------------------------
    bool tryConnect(const char *ssid, const char *pw) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, pw ? pw : "");
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - start > CONNECT_TIMEOUT_MS) {
                WiFi.disconnect(true);
                return false;
            }
            delay(250);
            Serial.print(".");
        }
        Serial.printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }

    // ------------------------------------------------------------------
    void startAP() {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID);  // open network — no password needed to connect
        delay(200);
        Serial.printf("[WiFi] AP mode. SSID='%s'  http://%s\n",
                      AP_SSID, WiFi.softAPIP().toString().c_str());

        // Captive portal: redirect all DNS to our IP
        _dns.start(DNS_PORT, "*", WiFi.softAPIP());

        _server.on("/",             HTTP_GET,  [this]() { handleRoot(); });
        _server.on("/wifi/scan",    HTTP_GET,  [this]() { handleScan(); });
        _server.on("/wifi/connect", HTTP_POST, [this]() { handleConnect(); });
        // Captive-portal detection paths used by iOS/Android/Windows
        _server.onNotFound([this]() {
            _server.sendHeader("Location", "http://192.168.4.1/");
            _server.send(302);
        });
        _server.begin();
    }

    // ------------------------------------------------------------------
    void handleRoot() {
        _server.send(200, "text/html", provisionPage());
    }

    void handleScan() {
        int n = WiFi.scanNetworks(false, true);  // blocking, include hidden
        String json = "[";
        for (int i = 0; i < n; i++) {
            if (i) json += ",";
            String ssid = WiFi.SSID(i);
            ssid.replace("\\", "\\\\");
            ssid.replace("\"", "\\\"");
            json += "{\"ssid\":\"" + ssid + "\""
                    ",\"rssi\":"   + String(WiFi.RSSI(i)) +
                    ",\"secure\":" + (WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") +
                    "}";
        }
        json += "]";
        WiFi.scanDelete();
        _server.sendHeader("Cache-Control", "no-cache");
        _server.send(200, "application/json", json);
    }

    void handleConnect() {
        String ssid = _server.arg("ssid");
        String pw   = _server.arg("password");

        if (ssid.length() == 0) {
            _server.send(400, "text/html", provisionPage(
                "<p style='color:#f44'>No SSID entered.</p>"));
            return;
        }

        _storage->saveWiFi(ssid, pw);

        _server.send(200, "text/html", R"(<!DOCTYPE html><html>
<head><meta charset='utf-8'><title>Connecting…</title>
<style>body{font-family:monospace;background:#111;color:#cfc;
padding:2em;text-align:center;}</style></head>
<body><h2>Credentials saved.</h2>
<p>The device is restarting and will connect to <strong>)" + ssid + R"(</strong>.</p>
<p style='color:#aaa'>Reconnect your device to that network and open
<strong>http://gps-base.local</strong> or find the device IP on your router.</p>
</body></html>)");

        delay(1500);
        ESP.restart();
    }

    // ------------------------------------------------------------------
    String provisionPage(const String &msg = "") {
        return R"HTML(<!DOCTYPE html><html>
<head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>GPS Base Station &mdash; WiFi Setup</title>
<style>
*{box-sizing:border-box}
body{font-family:monospace;background:#111;color:#cfc;padding:1em;max-width:480px;margin:0 auto}
h1{color:#0f0;font-size:1.3em}h2{color:#0d0;font-size:1.1em;border-bottom:1px solid #333;padding-bottom:4px}
input{width:100%;padding:8px;background:#1a1a1a;color:#cfc;border:1px solid #444;margin:4px 0 10px;font-family:monospace}
button{width:100%;padding:10px;background:#1a1a1a;color:#0f0;border:1px solid #0f0;font-family:monospace;cursor:pointer;margin-top:4px}
button:hover{background:#0f0;color:#111}
.net{padding:8px;margin:3px 0;background:#1a1a1a;border:1px solid #333;cursor:pointer;display:flex;justify-content:space-between;align-items:center}
.net:hover,.net.sel{border-color:#0f0;background:#0a1a0a}
.rssi{font-size:.8em;color:#888}.lock{color:#fa0}
.err{color:#f44;margin-bottom:.5em}
#scan-status{color:#888;font-size:.85em;margin:6px 0}
</style></head><body>
<h1>GPS Base Station</h1><h2>WiFi Setup</h2>
)HTML" + msg + R"HTML(
<div id='scan-status'>Scanning for networks&#8230;</div>
<div id='net-list'></div>
<form method='POST' action='/wifi/connect' style='margin-top:1em'>
  <label>Network (SSID)<input type='text' name='ssid' id='ssid' required placeholder='Select above or type manually'></label>
  <label>Password<input type='password' name='password' id='pw' placeholder='Leave blank for open networks'></label>
  <button type='submit'>Connect &amp; Save</button>
</form>
<script>
function bars(r){return r>=-60?'||||':r>=-70?'||| ':r>=-80?'||  ':'|   ';}
function loadNetworks(){
  fetch('/wifi/scan').then(function(r){return r.json();}).then(function(nets){
    document.getElementById('scan-status').textContent=nets.length+' network'+(nets.length!==1?'s':'')+' found';
    nets.sort(function(a,b){return b.rssi-a.rssi;});
    var html='';
    for(var i=0;i<nets.length;i++){
      var n=nets[i];
      var esc=n.ssid.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/"/g,'&quot;');
      var js=n.ssid.replace(/\\/g,'\\\\').replace(/'/g,"\\'");
      html+='<div class="net" onclick="pick(this,\''+js+'\')"><span>'+esc+'</span>'
           +'<span class="rssi">'+bars(n.rssi)+(n.secure?' <span class="lock">&#128274;</span>':'')+'</span></div>';
    }
    if(!html)html='<p style="color:#888">No networks found. <a href="/" style="color:#0f0">Retry</a></p>';
    document.getElementById('net-list').innerHTML=html;
  }).catch(function(){document.getElementById('scan-status').textContent='Scan failed.';});
}
function pick(el,ssid){
  document.querySelectorAll('.net').forEach(function(n){n.classList.remove('sel');});
  el.classList.add('sel');
  document.getElementById('ssid').value=ssid;
  document.getElementById('pw').focus();
}
loadNetworks();
</script>
</body></html>)HTML";
    }
};
