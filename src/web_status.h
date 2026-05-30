#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <Update.h>
#include "survey.h"
#include "storage.h"

// HTTP server on port 80.
//
// First boot (no admin password set):
//   GET  /setup   — set admin password form
//   POST /setup   — save admin password, redirect to /
//   All other routes redirect to /setup
//
// Normal operation (password is set — all routes require HTTP Basic Auth):
//   GET  /           — status + config page
//   POST /config     — save RTK2go / Onocoy credentials, restart
//   POST /survey     — start new survey-in
//   GET  /position   — current position as JSON

class WebStatus {
public:
    WebStatus(SurveyManager &survey, Storage &storage)
        : _survey(survey), _storage(storage), _server(80) {}

    void begin() {
        // Cache the admin password so checkAuth() doesn't need to call back into Storage
        // on every request.  Stays valid until the device restarts after a password change.
        if (_storage.isAdminPasswordSet()) {
            // Storage doesn't expose a getter, so we use a known-correct password
            // to bootstrap: re-check is done via authenticate() anyway.
            // We need the plaintext — add a thin accessor to Storage.
            _cachedAdminPw = _storage.getAdminPassword();
        }

        _server.on("/",             HTTP_GET,  [this]() { handleRoot(); });
        _server.on("/config",       HTTP_GET,  [this]() { handleConfigGet(); });
        _server.on("/config",       HTTP_POST, [this]() { handleConfig(); });
        _server.on("/survey",       HTTP_POST, [this]() { handleStartSurvey(); });
        _server.on("/survey/data",  HTTP_GET,  [this]() { handleSurveyData(); });
        _server.on("/status",       HTTP_GET,  [this]() { handleStatus(); });
        _server.on("/position",     HTTP_GET,  [this]() { handlePosition(); });
        _server.on("/update",       HTTP_GET,  [this]() { handleUpdateGet(); });
        _server.on("/update",       HTTP_POST,
            [this]() { handleUpdateComplete(); },
            [this]() { handleUpdateUpload(); });
        _server.on("/setup",        HTTP_GET,  [this]() { handleSetupGet(); });
        _server.on("/setup",        HTTP_POST, [this]() { handleSetupPost(); });
        _server.onNotFound([this]() { redirectRoot(); });
        _server.begin();
        Serial.println("[Web] Status/config server on port 80.");
    }

    void update() { _server.handleClient(); }

    void onSurveyRequested(std::function<void()> cb) { _surveyCallback = cb; }

    void setNtripLocalClients(int n)    { _ntripLocalClients = n; }
    void setRtk2goConnected(bool b)        { _rtk2goOk = b; }
    void setRtk2goStatus(const String &s)  { _rtk2goStatus = s; }
    void setOnocoyConnected(bool b)        { _onocoyOk = b; }
    void setOnocoyStatus(const String &s)  { _onocoyStatus = s; }
    void setRtkdataConnected(bool b)       { _rtkdataOk = b; }
    void setRtkdataStatus(const String &s) { _rtkdataStatus = s; }
    void setRtcmBytesPerSec(uint32_t b) { _rtcmBps = b; }
    void setBaseTxMode(bool baseTx)     { _inBaseTx = baseTx; }

    void setStats(uint32_t rtcmMin, uint32_t rtcmHr,
                  uint32_t r2gMin,  uint32_t r2gHr,
                  uint32_t oncMin,  uint32_t oncHr,
                  uint32_t rtkMin,  uint32_t rtkHr) {
        _rtcmMin = rtcmMin; _rtcmHr = rtcmHr;
        _r2gMin  = r2gMin;  _r2gHr  = r2gHr;
        _oncMin  = oncMin;  _oncHr  = oncHr;
        _rtkMin  = rtkMin;  _rtkHr  = rtkHr;
    }

private:
    SurveyManager &_survey;
    Storage       &_storage;
    WebServer      _server;
    std::function<void()> _surveyCallback;

    int      _ntripLocalClients = 0;
    bool     _rtk2goOk  = false;
    String   _rtk2goStatus;
    bool     _onocoyOk  = false;
    String   _onocoyStatus;
    bool     _rtkdataOk = false;
    String   _rtkdataStatus;
    uint32_t _rtcmBps = 0;
    bool     _inBaseTx = false;
    uint32_t _rtcmMin = 0, _rtcmHr = 0;
    uint32_t _r2gMin  = 0, _r2gHr  = 0;
    uint32_t _oncMin  = 0, _oncHr  = 0;
    uint32_t _rtkMin  = 0, _rtkHr  = 0;

    static constexpr const char *ADMIN_USER = "admin";

    // -------------------------------------------------------------------------
    // Auth helpers
    // -------------------------------------------------------------------------

    // Check Basic Auth against the stored admin password.
    // On first boot (no password set) redirects to /setup instead.
    bool checkAuth() {
        if (!_storage.isAdminPasswordSet()) {
            _server.sendHeader("Location", "/setup");
            _server.send(303);
            return false;
        }
        if (!_server.authenticate(ADMIN_USER, _cachedAdminPw.c_str())) {
            _server.requestAuthentication(BASIC_AUTH, "GPS Base Station",
                                          "Authentication required.");
            return false;
        }
        return true;
    }

    String _cachedAdminPw;

    void redirectRoot() {
        _server.sendHeader("Location", "/");
        _server.send(303);
    }

    // -------------------------------------------------------------------------
    // First-time setup
    // -------------------------------------------------------------------------
    void handleSetupGet() {
        if (_storage.isAdminPasswordSet()) {
            redirectRoot();
            return;
        }
        String html = page("First-Time Setup",
            "<h2>Set Admin Password</h2>"
            "<p>No password has been configured yet. Set one to protect this page.</p>"
            "<form method='POST' action='/setup'>"
            "<label>Password<br><input type='password' name='pw' required minlength='6' style='width:100%;max-width:300px;padding:6px;background:#222;color:#cfc;border:1px solid #444;'></label><br><br>"
            "<label>Confirm<br><input type='password' name='pw2' required minlength='6' style='width:100%;max-width:300px;padding:6px;background:#222;color:#cfc;border:1px solid #444;'></label><br><br>"
            "<button type='submit'>Set Password &amp; Continue</button>"
            "</form>");
        _server.send(200, "text/html", html);
    }

    void handleSetupPost() {
        if (_storage.isAdminPasswordSet()) {
            redirectRoot();
            return;
        }
        String pw  = _server.arg("pw");
        String pw2 = _server.arg("pw2");

        if (pw.length() < 6 || pw != pw2) {
            String html = page("Setup Error",
                "<h2>Error</h2><p class='err'>Passwords did not match or were too short (min 6 chars).</p>"
                "<p><a href='/setup'>Try again</a></p>");
            _server.send(400, "text/html", html);
            return;
        }
        _storage.setAdminPassword(pw);
        _cachedAdminPw = pw;
        _server.sendHeader("Location", "/");
        _server.send(303);
    }

    // -------------------------------------------------------------------------
    // Main status + config page
    // -------------------------------------------------------------------------
    void handleRoot() {
        if (!checkAuth()) return;

        BasePosition pos  = _storage.loadPosition();

        String stateStr;
        switch (_survey.state()) {
            case SurveyState::IDLE:       stateStr = "idle"; break;
            case SurveyState::COLLECTING: stateStr = "collecting"; break;
            case SurveyState::DONE:       stateStr = "done"; break;
            case SurveyState::ERROR:      stateStr = "error"; break;
        }

        // --- Status table (cells have IDs so JS can update them without page reload) ---
        String content = "<h2>Status</h2><table>";
        content += rowId("st-mode",    "Mode",                _inBaseTx ? "<span class='ok'>Base TX</span>" : "<span class='warn'>Survey-in</span>");
        content += rowId("st-pos",     "Stored position",     pos.valid ? "<span class='ok'>valid</span>" : "<span class='warn'>none</span>");
        if (pos.valid) {
            content += row("Latitude",   String(pos.lat, 8));
            content += row("Longitude",  String(pos.lon, 8));
            content += row("Height (m)", String(pos.height, 4));
        }
        content += rowId("st-clients", "Local NTRIP clients", String(_ntripLocalClients));
        content += rowId("st-r2g", "RTK2go",
                         fmtServiceStatus(_rtk2goOk,
                             _rtk2goStatus.length() ? _rtk2goStatus : String("disconnected"),
                             _r2gMin, _r2gHr));
        content += rowId("st-onc", "Onocoy",
                         fmtServiceStatus(_onocoyOk,
                             _onocoyStatus.length() ? _onocoyStatus : String("disconnected"),
                             _oncMin, _oncHr));
        content += rowId("st-rtk", "RTKdata.online",
                         fmtServiceStatus(_rtkdataOk,
                             _rtkdataStatus.length() ? _rtkdataStatus : String("disconnected"),
                             _rtkMin, _rtkHr));
        {
            const auto &sv = _survey.liveData();
            int total = sv.svGPS + sv.svGLO + sv.svGAL + sv.svBDS;
            String svHtml = "<span style='opacity:.7'>GPS:</span> " + String(sv.svGPS) +
                            " &nbsp;<span style='opacity:.7'>GLO:</span> " + String(sv.svGLO) +
                            " &nbsp;<span style='opacity:.7'>GAL:</span> " + String(sv.svGAL) +
                            " &nbsp;<span style='opacity:.7'>BDS:</span> " + String(sv.svBDS) +
                            " &nbsp;<span style='opacity:.55'>| total " + String(total) + "</span>";
            content += rowId("st-svs", "Satellites", svHtml);
        }
        content += rowId("st-bps", "RTCM throughput",
                         fmtThroughput(_rtcmBps, _rtcmMin, _rtcmHr));
        {
            int rssi = WiFi.RSSI();
            String rssiStr = String(rssi) + " dBm";
            String rssiHtml;
            if      (rssi >= -60) rssiHtml = "<span class='ok'>"   + rssiStr + " (excellent)</span>";
            else if (rssi >= -70) rssiHtml = "<span class='ok'>"   + rssiStr + " (good)</span>";
            else if (rssi >= -80) rssiHtml = "<span class='warn'>" + rssiStr + " (fair)</span>";
            else                  rssiHtml = "<span class='err'>"  + rssiStr + " (weak)</span>";
            String ssid = WiFi.SSID();
            content += rowId("st-rssi", "WiFi signal",
                             rssiHtml + " &nbsp;<small style='opacity:.6'>" + ssid + "</small>");
        }
        content += "</table>";

        // JS: poll /status every 10 s and patch only the status cells — form is untouched
        content += R"(<script>
function stPoll(){
  fetch('/status').then(function(r){return r.json();}).then(function(d){
    function set(id,html){var e=document.getElementById(id);if(e)e.innerHTML=html;}
    set('st-mode',    d.base_tx?"<span class='ok'>Base TX</span>":"<span class='warn'>Survey-in</span>");
    set('st-clients', d.clients);
    var svT=d.sv_gps+d.sv_glo+d.sv_gal+d.sv_bds;
    set('st-svs',"<span style='opacity:.7'>GPS:</span> "+d.sv_gps+" &nbsp;<span style='opacity:.7'>GLO:</span> "+d.sv_glo+" &nbsp;<span style='opacity:.7'>GAL:</span> "+d.sv_gal+" &nbsp;<span style='opacity:.7'>BDS:</span> "+d.sv_bds+" &nbsp;<span style='opacity:.55'>| total "+svT+"</span>");
    function fb(b){return b>=1048576?(b/1048576).toFixed(1)+' MB':b>=1024?(b/1024).toFixed(1)+' KB':b+' B';}
    function svc(ok,st,mn,hr){var s=ok?"<span class='ok'>connected</span>":"<span class='err'>"+st+"</span>";if(ok||mn>0||hr>0)s+=" <span style='opacity:.55'>| "+fb(mn)+"/min | "+fb(hr)+"/hr</span>";return s;}
    set('st-r2g', svc(d.rtk2go,  d.rtk2go_st,  d.r2g_min, d.r2g_hr));
    set('st-onc', svc(d.onocoy,  d.onocoy_st,  d.onc_min, d.onc_hr));
    set('st-rtk', svc(d.rtkdata, d.rtkdata_st, d.rtk_min, d.rtk_hr));
    set('st-bps',  d.bps+' B/s <span style=\'opacity:.55\'>| '+fb(d.rtcm_min)+'/min | '+fb(d.rtcm_hr)+'/hr</span>');
    set('st-pos',  d.pos_valid?"<span class='ok'>valid</span>":"<span class='warn'>none</span>");
    var r=parseInt(d.rssi),rc=r>=-60?"ok":r>=-70?"ok":r>=-80?"warn":"err",rl=r>=-60?"excellent":r>=-70?"good":r>=-80?"fair":"weak";
    set('st-rssi',"<span class='"+rc+"'>"+r+" dBm ("+rl+")</span> <small style='opacity:.6'>"+d.ssid+"</small>");
  }).catch(function(){});
  setTimeout(stPoll, 10000);
}
stPoll();
</script>)";

        // --- Live survey panel (only when collecting) ---
        if (_survey.state() == SurveyState::COLLECTING) {
            content += R"(
<div id='survey-panel' style='margin:1.2em 0;padding:1em;border:1px solid #333;background:#0a0a0a;'>
  <h2 style='margin-top:0'>Survey-in Progress</h2>
  <table id='sv-table'>
    <tr><td>Elapsed</td><td id='sv-elapsed'>—</td></tr>
    <tr><td>Samples collected</td><td id='sv-samples'>—</td></tr>
    <tr><td>Mean position σ (3D)</td><td id='sv-sigma'>—</td></tr>
    <tr><td>Per-fix σ (instantaneous)</td><td id='sv-sigma-inst'>—</td></tr>
    <tr><td>Target σ</td><td id='sv-target'>—</td></tr>
    <tr><td>Mean Latitude</td><td id='sv-lat'>—</td></tr>
    <tr><td>Mean Longitude</td><td id='sv-lon'>—</td></tr>
    <tr><td>Mean Height (m)</td><td id='sv-hgt'>—</td></tr>
    <tr><td>Satellites used / tracked</td><td id='sv-svs'>—</td></tr>
    <tr><td>Constellations</td><td id='sv-const'>—</td></tr>
  </table>
  <canvas id='sigma-chart' width='620' height='200'
    style='margin-top:1em;display:block;border:1px solid #222;background:#0d0d0d;'></canvas>
</div>
<script>
function fmtTime(s){
  var m=Math.floor(s/60),sec=s%60;
  return m>0?(m+'m '+sec+'s'):(sec+'s');
}
function drawChart(history, target) {
  var c = document.getElementById('sigma-chart');
  if (!c) return;
  var ctx = c.getContext('2d');
  var W = c.width, H = c.height;
  var pad = {l:52,r:12,t:12,b:30};
  ctx.clearRect(0,0,W,H);

  if (!history || history.length === 0) {
    ctx.fillStyle='#444'; ctx.font='12px monospace';
    ctx.fillText('Waiting for data...', W/2-60, H/2);
    return;
  }

  var maxT = history[history.length-1][0];
  var maxS = 0;
  for (var i=0;i<history.length;i++) maxS = Math.max(maxS, history[i][1]);
  maxS = Math.max(maxS, target * 1.5, 1.0);

  function tx(t){ return pad.l + (t/maxT)*(W-pad.l-pad.r); }
  function ty(s){ return pad.t + (1 - s/maxS)*(H-pad.t-pad.b); }

  // Grid lines
  ctx.strokeStyle='#1e1e1e'; ctx.lineWidth=1;
  for (var g=0;g<=4;g++){
    var gy = pad.t + g*(H-pad.t-pad.b)/4;
    ctx.beginPath(); ctx.moveTo(pad.l,gy); ctx.lineTo(W-pad.r,gy); ctx.stroke();
    var sv = (maxS*(1-g/4)).toFixed(2);
    ctx.fillStyle='#555'; ctx.font='10px monospace'; ctx.textAlign='right';
    ctx.fillText(sv+'m', pad.l-4, gy+3);
  }

  // Target line
  ctx.strokeStyle='#fa0'; ctx.lineWidth=1; ctx.setLineDash([4,4]);
  var ty_ = ty(target);
  ctx.beginPath(); ctx.moveTo(pad.l,ty_); ctx.lineTo(W-pad.r,ty_); ctx.stroke();
  ctx.setLineDash([]);
  ctx.fillStyle='#fa0'; ctx.font='10px monospace'; ctx.textAlign='left';
  ctx.fillText('target '+target.toFixed(2)+'m', pad.l+4, ty_-3);

  // Sigma line
  ctx.strokeStyle='#0f0'; ctx.lineWidth=2;
  ctx.beginPath();
  for (var i=0;i<history.length;i++){
    var x=tx(history[i][0]), y=ty(history[i][1]);
    i===0 ? ctx.moveTo(x,y) : ctx.lineTo(x,y);
  }
  ctx.stroke();

  // Axes
  ctx.strokeStyle='#444'; ctx.lineWidth=1;
  ctx.beginPath();
  ctx.moveTo(pad.l,pad.t); ctx.lineTo(pad.l,H-pad.b);
  ctx.lineTo(W-pad.r,H-pad.b); ctx.stroke();

  // X labels
  ctx.fillStyle='#555'; ctx.font='10px monospace'; ctx.textAlign='center';
  for (var g=0;g<=4;g++){
    var xt = (maxT*g/4)|0;
    ctx.fillText(fmtTime(xt), tx(xt), H-pad.b+14);
  }
}
function pollSurvey(){
  fetch('/survey/data').then(function(r){return r.json();}).then(function(d){
    if(d.state==='done'){ location.reload(); return; }
    if(d.state!='collecting') return;
    document.getElementById('sv-elapsed').textContent = fmtTime(d.elapsed);
    document.getElementById('sv-samples').textContent = d.samples;
    var sigEl = document.getElementById('sv-sigma');
    sigEl.textContent = d.sigma.toFixed(4)+' m';
    sigEl.style.color = d.sigma <= d.target_sigma ? '#0f0' : '#fa0';
    document.getElementById('sv-sigma-inst').textContent = d.sigma_inst.toFixed(3)+' m';
    document.getElementById('sv-target').textContent = d.target_sigma.toFixed(2)+' m';
    document.getElementById('sv-lat').textContent = d.lat.toFixed(8);
    document.getElementById('sv-lon').textContent = d.lon.toFixed(8);
    document.getElementById('sv-hgt').textContent = d.hgt.toFixed(3)+' m';
    document.getElementById('sv-svs').textContent = d.svs_used+' / '+d.svs_tracked;
    var cb='';
    if(d.gps) cb+='GPS:'+d.gps+' ';
    if(d.glo) cb+='GLO:'+d.glo+' ';
    if(d.gal) cb+='GAL:'+d.gal+' ';
    if(d.bds) cb+='BDS:'+d.bds;
    document.getElementById('sv-const').textContent = cb||'—';
    drawChart(d.history, d.target_sigma);
  }).catch(function(){});
  setTimeout(pollSurvey, 5000);
}
pollSurvey();
</script>
)";
        }

        // --- Survey button ---
        content += "<form method='POST' action='/survey' style='margin-top:1em'>"
                   "<button type='submit'>Start New Survey-in</button>"
                   "</form>";

        content += "<p style='margin-top:2em;'>"
                   "<a href='/config' style='color:#0d0;'>&#9881; Configuration</a>"
                   "&nbsp;&nbsp;&nbsp;"
                   "<a href='/update' style='color:#555;font-size:0.8em;'>Firmware Update (OTA)</a>"
                   "</p>";

        _server.send(200, "text/html", page("GPS Base Station", content));
    }

    // -------------------------------------------------------------------------
    // Config page (GET)
    // -------------------------------------------------------------------------
    void handleConfigGet() {
        if (!checkAuth()) return;

        ServiceCreds r2g  = _storage.loadCreds("rtk2go");
        ServiceCreds onc  = _storage.loadCreds("onocoy");
        ServiceCreds rtkd = _storage.loadCreds("rtkdata");

        String content = "<p><a href='/' style='color:#555;font-size:0.85em'>&larr; Status</a></p>"
                         "<h2>Service Configuration</h2>"
                         "<p style='color:#aaa;font-size:0.85em'>Changes are saved to flash and the device restarts automatically.</p>"
                         "<form method='POST' action='/config'>"

                         "<h3>RTK2go</h3>"
                         + enableToggle("r2g_en", "Enable RTK2go", _storage.serviceEnabled("rtk2go")) +
                         "<label>Mountpoint<br>" + textInput("r2g_mount", r2g.mountpoint) + "</label><br><br>"
                         "<label>Password<br>"   + pwInput("r2g_pw", "r2g_pw_set", r2g.password) + "</label><br><br>"

                         "<h3>Onocoy</h3>"
                         + enableToggle("onc_en", "Enable Onocoy", _storage.serviceEnabled("onocoy")) +
                         "<label>Mountpoint<br>" + textInput("onc_mount", onc.mountpoint) + "</label><br><br>"
                         "<label>Password<br>"   + pwInput("onc_pw", "onc_pw_set", onc.password) + "</label><br><br>"

                         "<h3>RTKdata.online</h3>"
                         + enableToggle("rtk_en", "Enable RTKdata.online", _storage.serviceEnabled("rtkdata")) +
                         "<label>Mountpoint<br>" + textInput("rtk_mount", rtkd.mountpoint) + "</label><br><br>"
                         "<label>Password<br>"   + pwInput("rtk_pw", "rtk_pw_set", rtkd.password) + "</label><br><br>"

                         "<h3>Admin Password</h3>"
                         "<label>New password (leave blank to keep current)<br>"
                         + pwInput("admin_pw", "admin_pw_set", "") + "</label><br><br>"
                         "<label>Confirm new password<br>"
                         + pwInput("admin_pw2", "admin_pw2_set", "") + "</label><br><br>"

                         "<button type='submit'>Save &amp; Restart</button>"
                         "</form>";

        _server.send(200, "text/html", page("Configuration", content));
    }

    // -------------------------------------------------------------------------
    // Save config and restart
    // -------------------------------------------------------------------------
    void handleConfig() {
        if (!checkAuth()) return;

        String r2gMount   = _server.arg("r2g_mount");
        String r2gPw      = _server.arg("r2g_pw");
        bool   r2gPwSet   = _server.arg("r2g_pw_set") == "1";
        String oncMount   = _server.arg("onc_mount");
        String oncPw      = _server.arg("onc_pw");
        bool   oncPwSet   = _server.arg("onc_pw_set") == "1";
        String rtkMount   = _server.arg("rtk_mount");
        String rtkPw      = _server.arg("rtk_pw");
        bool   rtkPwSet   = _server.arg("rtk_pw_set") == "1";
        bool   r2gEnabled = _server.hasArg("r2g_en");
        bool   oncEnabled = _server.hasArg("onc_en");
        bool   rtkEnabled = _server.hasArg("rtk_en");
        String adminPw    = _server.arg("admin_pw");
        String adminPw2   = _server.arg("admin_pw2");

        _storage.setServiceEnabled("rtk2go",  r2gEnabled);
        _storage.setServiceEnabled("onocoy",  oncEnabled);
        _storage.setServiceEnabled("rtkdata", rtkEnabled);

        // Save credentials independently — only update password if the user actually typed one.
        if (r2gMount.length() > 0) {
            if (r2gPwSet && r2gPw.length() > 0)
                _storage.saveCreds("rtk2go", r2gMount, r2gPw);
            else
                _storage.saveCreds("rtk2go", r2gMount, _storage.loadCreds("rtk2go").password);
        }
        if (oncMount.length() > 0) {
            if (oncPwSet && oncPw.length() > 0)
                _storage.saveCreds("onocoy", oncMount, oncPw);
            else
                _storage.saveCreds("onocoy", oncMount, _storage.loadCreds("onocoy").password);
        }
        if (rtkMount.length() > 0) {
            if (rtkPwSet && rtkPw.length() > 0)
                _storage.saveCreds("rtkdata", rtkMount, rtkPw);
            else
                _storage.saveCreds("rtkdata", rtkMount, _storage.loadCreds("rtkdata").password);
        }

        if (adminPw.length() > 0) {
            if (adminPw != adminPw2) {
                String html = page("Config Error",
                    "<h2>Error</h2><p class='err'>Admin passwords did not match.</p>"
                    "<p><a href='/config'>Go back</a></p>");
                _server.send(400, "text/html", html);
                return;
            }
            if (adminPw.length() < 6) {
                String html = page("Config Error",
                    "<h2>Error</h2><p class='err'>Password must be at least 6 characters.</p>"
                    "<p><a href='/config'>Go back</a></p>");
                _server.send(400, "text/html", html);
                return;
            }
            _storage.setAdminPassword(adminPw);
        }

        _server.send(200, "text/html", page("Saved",
            "<h2>Configuration Saved</h2>"
            "<p>Device is restarting...</p>"
            "<script>setTimeout(()=>location.href='/',5000)</script>"));

        delay(1000);
        ESP.restart();
    }

    // -------------------------------------------------------------------------
    // Start survey
    // -------------------------------------------------------------------------
    void handleStartSurvey() {
        if (!checkAuth()) return;
        if (_surveyCallback) _surveyCallback();
        _server.sendHeader("Location", "/");
        _server.send(303);
    }

    // -------------------------------------------------------------------------
    // OTA firmware update
    // -------------------------------------------------------------------------
    bool _updateError = false;

    void handleUpdateGet() {
        if (!checkAuth()) return;
        String content =
            "<h2>Firmware Update (OTA)</h2>"
            "<p style='color:#aaa;font-size:0.85em'>Build the firmware with PlatformIO, then upload "
            "<code>.pio/build/esp32dev/firmware.bin</code>. "
            "The device will restart automatically after a successful flash.</p>"
            "<form method='POST' action='/update' enctype='multipart/form-data'>"
            "<input type='file' name='firmware' accept='.bin' required "
            "  style='color:#cfc;background:#1a1a1a;border:1px solid #444;padding:6px;width:100%;max-width:400px;'>"
            "<br><br>"
            "<button type='submit'>Upload &amp; Flash</button>"
            "</form>"
            "<p><a href='/' style='color:#555;font-size:0.85em'>&larr; Back</a></p>";
        _server.send(200, "text/html", page("Firmware Update", content));
    }

    void handleUpdateUpload() {
        if (!_server.authenticate(ADMIN_USER, _cachedAdminPw.c_str())) return;

        HTTPUpload &upload = _server.upload();

        if (upload.status == UPLOAD_FILE_START) {
            _updateError = false;
            Serial.printf("[OTA] Receiving: %s (%u bytes expected)\n",
                          upload.filename.c_str(), upload.totalSize);
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                _updateError = true;
                Serial.printf("[OTA] begin() error: %s\n", Update.errorString());
            }

        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (!_updateError) {
                if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                    _updateError = true;
                    Serial.printf("[OTA] write() error: %s\n", Update.errorString());
                }
            }

        } else if (upload.status == UPLOAD_FILE_END) {
            if (!_updateError) {
                if (!Update.end(true)) {
                    _updateError = true;
                    Serial.printf("[OTA] end() error: %s\n", Update.errorString());
                } else {
                    Serial.printf("[OTA] Flash complete (%u bytes). Restarting.\n",
                                  upload.totalSize);
                }
            }
        }
    }

    void handleUpdateComplete() {
        if (!checkAuth()) return;
        if (_updateError) {
            _server.send(500, "text/html", page("OTA Failed",
                "<h2>Update Failed</h2>"
                "<p class='err'>" + String(Update.errorString()) + "</p>"
                "<p><a href='/update'>Try again</a></p>"));
        } else {
            _server.send(200, "text/html", page("OTA Success",
                "<h2>Update Successful</h2>"
                "<p class='ok'>Firmware flashed. Device is restarting&hellip;</p>"
                "<script>setTimeout(()=>location.href='/',8000)</script>"));
            delay(500);
            ESP.restart();
        }
    }

    // -------------------------------------------------------------------------
    // Survey live data JSON  (polled every 5 s by the survey panel JS)
    // -------------------------------------------------------------------------
    void handleSurveyData() {
        if (!checkAuth()) return;

        const auto &d = _survey.liveData();
        String state;
        switch (_survey.state()) {
            case SurveyState::COLLECTING: state = "collecting"; break;
            case SurveyState::DONE:       state = "done";       break;
            default:                      state = "idle";        break;
        }

        String json = "{";
        json += "\"state\":\"" + state + "\"";
        json += ",\"elapsed\":"      + String(d.elapsed);
        json += ",\"lat\":"          + String(d.lat, 8);
        json += ",\"lon\":"          + String(d.lon, 8);
        json += ",\"hgt\":"          + String(d.hgt, 4);
        json += ",\"sigma\":"        + String(d.sigma, 4);
        json += ",\"sigma_inst\":"   + String(d.sigmaInst, 4);
        json += ",\"samples\":"      + String(d.samples);
        json += ",\"svs_used\":"     + String(d.svUsed);
        json += ",\"svs_tracked\":"  + String(d.svTracked);
        json += ",\"gps\":"          + String(d.svGPS);
        json += ",\"glo\":"          + String(d.svGLO);
        json += ",\"gal\":"          + String(d.svGAL);
        json += ",\"bds\":"          + String(d.svBDS);
        json += ",\"target_sigma\":" + String(SURVEY_MAX_SIGMA, 2);

        // Sigma history array
        SurveyManager::HistorySample hist[SurveyManager::HISTORY_SIZE];
        int n = _survey.getHistory(hist, SurveyManager::HISTORY_SIZE);
        json += ",\"history\":[";
        for (int i = 0; i < n; i++) {
            if (i) json += ",";
            json += "[" + String(hist[i].t) + "," + String(hist[i].sigma, 4) + "]";
        }
        json += "]}";

        _server.sendHeader("Cache-Control", "no-cache");
        _server.send(200, "application/json", json);
    }

    // -------------------------------------------------------------------------
    // Status JSON  (polled every 10 s by the status table JS)
    // -------------------------------------------------------------------------
    void handleStatus() {
        if (!checkAuth()) return;
        BasePosition pos = _storage.loadPosition();
        String json = "{";
        auto jsonStr = [](const String &s) -> String {
            String out = "\"";
            for (char c : s) { if (c=='"') out+="\\\""; else out+=c; }
            return out + "\"";
        };
        json += "\"base_tx\":"      + String(_inBaseTx ? "true" : "false");
        json += ",\"rtk2go\":"      + String(_rtk2goOk ? "true" : "false");
        json += ",\"rtk2go_st\":"   + jsonStr(_rtk2goOk ? "connected" : _rtk2goStatus);
        json += ",\"onocoy\":"      + String(_onocoyOk ? "true" : "false");
        json += ",\"onocoy_st\":"   + jsonStr(_onocoyOk ? "connected" : _onocoyStatus);
        json += ",\"rtkdata\":"     + String(_rtkdataOk ? "true" : "false");
        json += ",\"rtkdata_st\":"  + jsonStr(_rtkdataOk ? "connected" : _rtkdataStatus);
        json += ",\"clients\":"     + String(_ntripLocalClients);
        {
            const auto &sv = _survey.liveData();
            json += ",\"sv_gps\":"  + String(sv.svGPS);
            json += ",\"sv_glo\":"  + String(sv.svGLO);
            json += ",\"sv_gal\":"  + String(sv.svGAL);
            json += ",\"sv_bds\":"  + String(sv.svBDS);
        }
        json += ",\"bps\":"         + String(_rtcmBps);
        json += ",\"rtcm_min\":"    + String(_rtcmMin);
        json += ",\"rtcm_hr\":"     + String(_rtcmHr);
        json += ",\"r2g_min\":"     + String(_r2gMin);
        json += ",\"r2g_hr\":"      + String(_r2gHr);
        json += ",\"onc_min\":"     + String(_oncMin);
        json += ",\"onc_hr\":"      + String(_oncHr);
        json += ",\"rtk_min\":"     + String(_rtkMin);
        json += ",\"rtk_hr\":"      + String(_rtkHr);
        json += ",\"rssi\":"        + String(WiFi.RSSI());
        json += ",\"ssid\":"        + jsonStr(WiFi.SSID());
        json += ",\"pos_valid\":"   + String(pos.valid ? "true" : "false");
        json += "}";
        _server.sendHeader("Cache-Control", "no-cache");
        _server.send(200, "application/json", json);
    }

    // -------------------------------------------------------------------------
    // Position JSON
    // -------------------------------------------------------------------------
    void handlePosition() {
        if (!checkAuth()) return;
        BasePosition pos = _storage.loadPosition();
        String json = "{\"valid\":" + String(pos.valid ? "true" : "false");
        if (pos.valid) {
            json += ",\"lat\":"    + String(pos.lat, 8);
            json += ",\"lon\":"    + String(pos.lon, 8);
            json += ",\"height\":" + String(pos.height, 4);
        }
        json += "}";
        _server.send(200, "application/json", json);
    }

    // -------------------------------------------------------------------------
    // HTML helpers
    // -------------------------------------------------------------------------
    String page(const String &title, const String &content) {
        return "<!DOCTYPE html><html><head>"
               "<meta charset='utf-8'>"
               "<title>" + title + "</title>"
               "<style>"
               "body{font-family:monospace;background:#111;color:#cfc;padding:1em 2em;max-width:700px;margin:0 auto;}"
               "h1{color:#0f0;} h2{color:#0d0;border-bottom:1px solid #333;padding-bottom:4px;}"
               "h3{color:#0b0;margin-bottom:4px;}"
               "table{border-collapse:collapse;width:100%;}"
               "td,th{border:1px solid #333;padding:5px 10px;text-align:left;}"
               "th{background:#1a1a1a;}"
               "input[type=text],input[type=password]{width:100%;max-width:340px;padding:6px;"
               "  background:#1a1a1a;color:#cfc;border:1px solid #444;box-sizing:border-box;}"
               "button{background:#1a1a1a;color:#0f0;border:1px solid #0f0;padding:8px 18px;"
               "  cursor:pointer;margin-top:6px;}"
               "button:hover{background:#0f0;color:#111;}"
               ".ok{color:#0f0;} .warn{color:#fa0;} .err{color:#f44;}"
               "label{color:#aaa;font-size:0.9em;}"
               "</style></head><body>"
               "<h1>GPS Base Station</h1>"
               + content +
               "<p style='color:#444;font-size:0.75em;margin-top:2em'>Status updates every 10s</p>"
               "</body></html>";
    }

    String row(const String &label, const String &value) {
        return "<tr><td>" + label + "</td><td>" + value + "</td></tr>";
    }

    String rowId(const String &id, const String &label, const String &value) {
        return "<tr><td>" + label + "</td><td id='" + id + "'>" + value + "</td></tr>";
    }

    // Format bytes as "1.2 KB" / "3.4 MB" / "512 B"
    static String fmtBytes(uint32_t b) {
        if (b >= 1048576) return String(b / 1048576.0f, 1) + " MB";
        if (b >= 1024)    return String(b / 1024.0f,    1) + " KB";
        return String(b) + " B";
    }

    // "847 B/s | 50.8 KB/min | 3.0 MB/hr"  (grayed-out min/hr until first window)
    static String fmtThroughput(uint32_t bps, uint32_t perMin, uint32_t perHr) {
        String s = String(bps) + " B/s";
        s += " <span style='opacity:.55'>| " + fmtBytes(perMin) + "/min";
        s += " | " + fmtBytes(perHr) + "/hr</span>";
        return s;
    }

    // "connected | 50.8 KB/min | 3.0 MB/hr"
    static String fmtServiceStatus(bool ok, const String &status,
                                    uint32_t perMin, uint32_t perHr) {
        String s;
        if (ok) s = "<span class='ok'>connected</span>";
        else    s = "<span class='err'>" + status + "</span>";
        if (ok || perMin > 0 || perHr > 0) {
            s += " <span style='opacity:.55'>| " + fmtBytes(perMin) + "/min";
            s += " | " + fmtBytes(perHr) + "/hr</span>";
        }
        return s;
    }

    String textInput(const String &name, const String &value) {
        return "<input type='text' name='" + name + "' value='" + value + "'>";
    }

    // changedFlag is a hidden field name that JS sets to "1" when user edits the field
    String pwInput(const String &name, const String &changedFlag, const String &value) {
        String placeholder = value.length() > 0 ? "••••••••" : "";
        return "<input type='hidden' name='" + changedFlag + "' id='" + changedFlag + "' value='0'>"
               "<input type='password' name='" + name + "' placeholder='" + placeholder + "'"
               " oninput=\"document.getElementById('" + changedFlag + "').value='1'\">";
    }

    String enableToggle(const String &name, const String &label, bool enabled) {
        String chk = enabled ? " checked" : "";
        return "<label style='display:flex;align-items:center;gap:8px;margin-bottom:10px;cursor:pointer;'>"
               "<input type='checkbox' name='" + name + "'" + chk +
               " style='width:18px;height:18px;accent-color:#0f0;'>"
               "<span>" + label + "</span></label>";
    }
};
