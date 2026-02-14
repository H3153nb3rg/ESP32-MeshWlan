#ifndef SWARM_CONFIG_MANAGER_H
#define SWARM_CONFIG_MANAGER_H

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <painlessMesh.h>


// =====================
// MESH
// =====================

#define MESH_ENABLED true
#define MESH_PREFIX "ESP32_SWARM_NET"
#define MESH_PASSWORD "meshpassword123"
#define MESH_PORT 5555

// =====================
// ACCESS POINT
// =====================
#define ESP32_SWARM_AP "ESP32_SWARM_AP"

class SwarmConfigManager {
public:
    // Konstruktor: batteryPowered (true/false), Mesh Name, Mesh Passwort
    SwarmConfigManager(bool batteryPowered, const char* meshPrefix, const char* meshPass);

    void setup();
    void loop();

    WiFiMulti getWifiMulti();

private:
    // Variablen
    bool _isBatteryPowered;
    const char* _meshPrefix;
    const char* _meshPass;
    bool _meshStarted = false;
    bool _serverActive = false;
    bool _syncReceived = false;
    unsigned long _serverStartTime = 0;

    // Objekte
    WiFiMulti _wifiMulti;
    WiFiManager _wm;
    WebServer _server;
    painlessMesh _mesh;
    Scheduler _userScheduler;

    // Interne Logik
    uint32_t getLocalVersion();
    void updateWiFiMulti();
    void saveFullConfig(JsonDocument& doc, bool propagate);
    void addNewNetwork(String ssid, String pass);
    void sendBlinkCommand();
    void blinkLED();
    
    // UI & Diagnose
    String getMeshStatusHTML();
    String getRSSILevel(int rssi);
    void printSerialQRCode(String url);

    // Web Handler
    void handleRoot();
    void handleScan();
    void handleView();
    void handleDelete();
    void handleAdd();

    // Mesh Callbacks
    static void meshReceivedWrapper(uint32_t from, String &msg);
    static SwarmConfigManager* _instance; 
};

#endif