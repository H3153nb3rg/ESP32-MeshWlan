#include <qrcode.h>

#include "SwarmConfigManager.h"



#define LED_PIN 2
#define CONFIG_FILE "/networks.json"
#define TRIGGER_PIN 0

SwarmConfigManager* SwarmConfigManager::_instance = nullptr;

SwarmConfigManager::SwarmConfigManager(bool batteryPowered, const char* meshPrefix, const char* meshPass) 
    : _isBatteryPowered(batteryPowered), _meshPrefix(meshPrefix), _meshPass(meshPass), _server(80) {
    _instance = this;
}

void SwarmConfigManager::setup() {
    Serial.begin(115200);

    Serial.println("--- SETUP Start");
    
    pinMode(LED_PIN, OUTPUT);
    pinMode(TRIGGER_PIN, INPUT_PULLUP);
    
    if(!LittleFS.begin(true)) Serial.println("FS Error");

    updateWiFiMulti();
    _wm.setConfigPortalBlocking(false);
    _wm.setConfigPortalTimeout(180);

    _mesh.setDebugMsgTypes(ERROR | STARTUP);
    _mesh.init(_meshPrefix, _meshPass, &_userScheduler, 5555);
    _mesh.onReceive(&meshReceivedWrapper);
    _mesh.stationManual(_meshPrefix, _meshPass);
    _meshStarted = true;

    Serial.println("--- MESH initialized");
    
    if (_wifiMulti.run() != WL_CONNECTED) {
        unsigned long start = millis();
        while(!_syncReceived && millis() - start < 10000) {
            if (millis() % 3000 == 0) {
                JsonDocument req; req["type"] = "SYNC_REQ";
                String r; serializeJson(req, r); _mesh.sendBroadcast(r);
            }
            _mesh.update();
            Serial.println("--- MESH request send");
            delay(1);
        }
        if (WiFi.status() != WL_CONNECTED && !_isBatteryPowered) {
            Serial.println("--- AP Mode");
            _wm.startConfigPortal("ESP32_SWARM_AP");
        }
    }

    _server.on("/", [this](){ handleRoot(); });
    _server.on("/scan", [this](){ handleScan(); });
    _server.on("/view", [this](){ handleView(); });
    _server.on("/delete", [this](){ handleDelete(); });
    _server.on("/add", HTTP_POST, [this](){ handleAdd(); });
    _server.on("/blink", [this](){ sendBlinkCommand(); _server.sendHeader("Location", "/"); _server.send(303); });
    _server.on("/reboot", [](){ _instance->_server.send(200, "text/plain", "Rebooting..."); delay(500); ESP.restart(); });

    Serial.println("--- SETUP Done");
}

void SwarmConfigManager::loop() {
    
    _wm.process();
    
    if (_meshStarted) _mesh.update();
    if (_serverActive) _server.handleClient();

    if (WiFi.status() == WL_CONNECTED && WiFi.getMode() & WIFI_AP) {
        addNewNetwork(WiFi.SSID(), WiFi.psk());
        WiFi.mode(WIFI_STA);
        Serial.println("--- Wifi in use: "+WiFi.SSID());
    }

    if (digitalRead(TRIGGER_PIN) == LOW) {
        delay(50);
        if (!_serverActive) {
            _server.begin();
            _serverActive = true;
            _serverStartTime = millis();
            printSerialQRCode("http://" + WiFi.localIP().toString());
            Serial.println("--- Webserver started");
        }
    }

    if (_isBatteryPowered && WiFi.status() == WL_CONNECTED) {
        delay(2000);
        ESP.deepSleep(600e6);
    }
}

WiFiMulti   SwarmConfigManager::getWifiMulti() {
    return _wifiMulti;
}   

// --- PRIVATER LOGIK-BLOCK ---

uint32_t SwarmConfigManager::getLocalVersion() {
    if (!LittleFS.exists(CONFIG_FILE)) return 0;
    File f = LittleFS.open(CONFIG_FILE, "r");
    JsonDocument doc;
    deserializeJson(doc, f);
    f.close();
    return doc["version"] | 0;
}

void SwarmConfigManager::updateWiFiMulti() {
    if (!LittleFS.exists(CONFIG_FILE)) return;
    File f = LittleFS.open(CONFIG_FILE, "r");
    JsonDocument doc;
    deserializeJson(doc, f);
    f.close();
    JsonArray arr = doc["networks"].as<JsonArray>();
    for (JsonObject n : arr) {
        _wifiMulti.addAP(n["ssid"].as<const char*>(), n["pass"].as<const char*>());
        Serial.println("WifiMulti ssid added:"+n["ssid"].as<const char*>());
    }
}

void SwarmConfigManager::saveFullConfig(JsonDocument& doc, bool propagate) {
    File f = LittleFS.open(CONFIG_FILE, "w");
    serializeJson(doc, f);
    f.close();
    updateWiFiMulti();
    if (_meshStarted && propagate) {
        doc["type"] = "SYNC_RES";
        String msg;
        serializeJson(doc, msg);
        _mesh.sendBroadcast(msg);
    }
}

void SwarmConfigManager::addNewNetwork(String ssid, String pass) {
    JsonDocument doc;
    if (LittleFS.exists(CONFIG_FILE)) {
        File f = LittleFS.open(CONFIG_FILE, "r");
        deserializeJson(doc, f);
        f.close();
    }
    doc["version"] = (doc["version"] | 0) + 1;
    JsonArray arr = doc["networks"].as<JsonArray>();
    if (arr.isNull()) arr = doc["networks"].to<JsonArray>();
    bool found = false;
    for(JsonObject n : arr) { if(n["ssid"] == ssid) { n["pass"] = pass; found = true; break; } }
    if(!found) { JsonObject n = arr.add<JsonObject>(); n["ssid"] = ssid; n["pass"] = pass; }
    saveFullConfig(doc, true);
}

void SwarmConfigManager::meshReceivedWrapper(uint32_t from, String &msg) {
    JsonDocument doc;
    if (deserializeJson(doc, msg)) return;
    if (doc["type"] == "SYNC_REQ" && !_instance->_isBatteryPowered) {
        File f = LittleFS.open(CONFIG_FILE, "r");
        JsonDocument res; deserializeJson(res, f); f.close();
        res["type"] = "SYNC_RES";
        String out; serializeJson(res, out);
        _instance->_mesh.sendSingle(from, out);
    } else if (doc["type"] == "SYNC_RES") {
        if (doc["version"].as<uint32_t>() > _instance->getLocalVersion()) {
            _instance->saveFullConfig(doc, false);
            _instance->_syncReceived = true;
        }
    } else if (doc["type"] == "BLINK_CMD") {            
        _instance->blinkLED();
    }
}

void SwarmConfigManager::blinkLED() {
    Serial.println("Blink LED");
    digitalWrite(LED_PIN, HIGH);
    delay(500);
    digitalWrite(LED_PIN, LOW);
}

// --- UI GENERATOREN ---

String SwarmConfigManager::getRSSILevel(int rssi) {
    if (rssi > -55) return "<span style='color:#34a853;'>▂▄▆█</span>";
    if (rssi > -70) return "<span style='color:#fbbc04;'>▂▄▆</span><span style='color:#ccc;'>█</span>";
    if (rssi > -85) return "<span style='color:#ea4335;'>▂▄</span><span style='color:#ccc;'>▆█</span>";
    return "<span style='color:#ea4335;'>▂</span><span style='color:#ccc;'>▄▆█</span>";
}

String SwarmConfigManager::getMeshStatusHTML() {
    String out = "<div class='mesh-list'><b>Mesh Status:</b><br>";
    out += "• Local ID: " + String(_mesh.getNodeId()) + "<br>";
    JsonDocument doc;
    deserializeJson(doc, _mesh.subConnectionJson());
    JsonArray subNodes = doc.as<JsonArray>();
    for (JsonObject node : subNodes) {
        int rssi = node["rssi"] | -100;
        out += "• Node: " + node["nodeId"].as<String>() + " " + getRSSILevel(rssi) + " <small>(" + String(rssi) + ")</small><br>";
        Serial.println("Web ssid found :"+node["nodeId"].as<String>());
    }
    out += "</div>";
    return out;
}

void SwarmConfigManager::handleRoot() {
    Serial.println("Web -- rootpage");
    String url = "http://" + WiFi.localIP().toString();
    String html = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<script src='https://cdnjs.cloudflare.com/ajax/libs/qrcodejs/1.0.0/qrcode.min.js'></script>";
    html += "<style>body{font-family:sans-serif; background:#f4f7f9; text-align:center; padding:10px;} .card{background:white; padding:20px; border-radius:15px; box-shadow:0 4px 10px rgba(0,0,0,0.1); max-width:400px; margin:auto;} .btn{display:block; padding:12px; background:#1a73e8; color:white; text-decoration:none; border-radius:8px; margin:10px 0; font-weight:bold;} .mesh-list{text-align:left; font-size:0.85em; background:#eee; padding:10px; border-radius:8px; margin:15px 0; border-left:4px solid #1a73e8;} #qrcode{display:flex; justify-content:center; margin:20px;}</style></head><body>";
    html += "<div class='card'><h1>Swarm Admin</h1><p>Version: v" + String(getLocalVersion()) + "</p><div id='qrcode'></div>";
    html += getMeshStatusHTML();
    html += "<a href='/scan' class='btn' style='background:#34a853;'>WLAN Scannen</a>";
    html += "<a href='/view' class='btn'>Netzwerke verwalten</a>";
    html += "<a href='/blink' class='btn' style='background:#fbbc04; color:black;'>Alle finden (Blink)</a>";
    html += "<script>new QRCode(document.getElementById('qrcode'), {text:'"+url+"', width:140, height:140});</script></div></body></html>";
    _server.send(200, "text/html", html);
}

void SwarmConfigManager::handleScan() {
    Serial.println("Web -- scanpage");
    int n = WiFi.scanNetworks();
    String html = "<html><body><h2>Scan Results</h2><table border='1'>";
    for (int i = 0; i < n; ++i) {
        html += "<tr><td>" + WiFi.SSID(i) + "</td><td><form action='/add' method='POST'><input type='hidden' name='s' value='"+WiFi.SSID(i)+"'><input type='password' name='p'><input type='submit' value='Add'></form></td></tr>";
    }
    html += "</table><br><a href='/'>Back</a></body></html>";
    _server.send(200, "text/html", html);
}

void SwarmConfigManager::handleView() {
    Serial.println("Web -- viewpage");
    String html = "<html><body><h2>Networks</h2><ul>";
    if (LittleFS.exists(CONFIG_FILE)) {
        File f = LittleFS.open(CONFIG_FILE, "r");
        JsonDocument doc; deserializeJson(doc, f); f.close();
        JsonArray arr = doc["networks"].as<JsonArray>();
        for (int i=0; i<arr.size(); i++) html += "<li>" + arr[i]["ssid"].as<String>() + " <a href='/delete?id=" + String(i) + "'>[Delete]</a></li>";
    }
    html += "</ul><a href='/'>Back</a></body></html>";
    _server.send(200, "text/html", html);
}

void SwarmConfigManager::handleDelete() {
    Serial.println("Web -- deletepage");
    if (_server.hasArg("id")) {
        JsonDocument doc;
        File f = LittleFS.open(CONFIG_FILE, "r"); deserializeJson(doc, f); f.close();
        doc["networks"].as<JsonArray>().remove(_server.arg("id").toInt());
        doc["version"] = (doc["version"] | 0) + 1;
        saveFullConfig(doc, true);
    }
    _server.sendHeader("Location", "/view"); _server.send(303);
}

void SwarmConfigManager::handleAdd() {
    Serial.println("Web -- addpage");
    if(_server.hasArg("s") && _server.hasArg("p")) addNewNetwork(_server.arg("s"), _server.arg("p"));
    _server.sendHeader("Location", "/"); _server.send(303);
}

void SwarmConfigManager::printSerialQRCode(String url) {
    Serial.println("print QR:");
    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qrcodeData, 3, 0, url.c_str());
    Serial.println("\n[ SCAN ME ]");
    for (uint8_t y = 0; y < qrcode.size; y++) {
        Serial.print("  ");
        for (uint8_t x = 0; x < qrcode.size; x++) Serial.print(qrcode_getModule(&qrcode, x, y) ? "\u2588\u2588" : "  ");
        Serial.println();
    }
    Serial.println("URL: " + url + "\n");
}

void SwarmConfigManager::sendBlinkCommand() {
    JsonDocument doc; doc["type"] = "BLINK_CMD";
    String msg; serializeJson(doc, msg);
    _mesh.sendBroadcast(msg);
    blinkLED();
}
