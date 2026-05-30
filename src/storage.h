#pragma once
#include <Preferences.h>

struct BasePosition {
    double lat;
    double lon;
    double height;
    bool   valid;
};

struct ServiceCreds {
    String mountpoint;
    String password;
};

class Storage {
public:
    void begin() {
        _prefs.begin("gps_base", false);
    }

    // -------------------------------------------------------------------------
    // Base position
    // -------------------------------------------------------------------------
    BasePosition loadPosition() {
        BasePosition p;
        p.valid  = _prefs.getBool("pos_valid", false);
        p.lat    = _prefs.getDouble("lat", 0.0);
        p.lon    = _prefs.getDouble("lon", 0.0);
        p.height = _prefs.getDouble("hgt", 0.0);
        return p;
    }

    void savePosition(double lat, double lon, double height) {
        _prefs.putDouble("lat", lat);
        _prefs.putDouble("lon", lon);
        _prefs.putDouble("hgt", height);
        _prefs.putBool("pos_valid", true);
        Serial.printf("[Storage] Position saved: %.8f, %.8f, %.4f\n", lat, lon, height);
    }

    void clearPosition() {
        _prefs.putBool("pos_valid", false);
        Serial.println("[Storage] Position cleared.");
    }

    // -------------------------------------------------------------------------
    // Admin password
    // First boot: isAdminPasswordSet() returns false → show setup page.
    // -------------------------------------------------------------------------
    bool isAdminPasswordSet() {
        return _prefs.getBool("admin_set", false);
    }

    // Returns true if supplied password matches stored password.
    bool checkAdminPassword(const String &pw) {
        return pw == _prefs.getString("admin_pw", "");
    }

    void setAdminPassword(const String &pw) {
        _prefs.putString("admin_pw", pw);
        _prefs.putBool("admin_set", true);
        Serial.println("[Storage] Admin password updated.");
    }

    String getAdminPassword() {
        return _prefs.getString("admin_pw", "");
    }

    // -------------------------------------------------------------------------
    // Service credentials (RTK2go / Onocoy)
    // -------------------------------------------------------------------------
    ServiceCreds loadCreds(const char *service) {
        String keyMount = String(service) + "_mp";
        String keyPw    = String(service) + "_pw";
        ServiceCreds c;
        c.mountpoint = _prefs.getString(keyMount.c_str(), "");
        c.password   = _prefs.getString(keyPw.c_str(), "");
        return c;
    }

    void saveCreds(const char *service, const String &mountpoint, const String &password) {
        _prefs.putString((String(service) + "_mp").c_str(), mountpoint);
        _prefs.putString((String(service) + "_pw").c_str(), password);
        Serial.printf("[Storage] %s credentials saved.\n", service);
    }

    void savePassword(const char *service, const String &password) {
        _prefs.putString((String(service) + "_pw").c_str(), password);
    }

    bool serviceEnabled(const char *service) {
        return _prefs.getBool((String(service) + "_en").c_str(), true);
    }

    void setServiceEnabled(const char *service, bool enabled) {
        _prefs.putBool((String(service) + "_en").c_str(), enabled);
        Serial.printf("[Storage] %s %s.\n", service, enabled ? "enabled" : "disabled");
    }

    // -------------------------------------------------------------------------
    // WiFi credentials
    // -------------------------------------------------------------------------
    struct WiFiCreds {
        String ssid;
        String password;
        bool   valid = false;
    };

    WiFiCreds loadWiFi() {
        WiFiCreds c;
        c.ssid     = _prefs.getString("wifi_ssid", "");
        c.password = _prefs.getString("wifi_pw",   "");
        c.valid    = c.ssid.length() > 0;
        return c;
    }

    void saveWiFi(const String &ssid, const String &password) {
        _prefs.putString("wifi_ssid", ssid);
        _prefs.putString("wifi_pw",   password);
        Serial.printf("[Storage] WiFi credentials saved for: %s\n", ssid.c_str());
    }

    bool hasCredentials(const char *service) {
        ServiceCreds c = loadCreds(service);
        return c.mountpoint.length() > 0 && c.password.length() > 0;
    }

private:
    Preferences _prefs;
};
