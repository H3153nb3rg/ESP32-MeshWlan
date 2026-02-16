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
    delay(500);
    Serial.println("\n[SYSTEM] SwarmConfigManager startet...");
    
    pinMode(LED_PIN, OUTPUT);
    pinMode(TRIGGER_PIN, INPUT_PULLUP);
    
    if(!LittleFS.begin(true)) {
        Serial.println("[ERROR] LittleFS konnte nicht gemountet werden!");
    } else {
        Serial.println("[FS] LittleFS erfolgreich geladen.");
    }

    // 1. WLAN-Liste laden
    updateWiFiMulti();

    // 2. Erster Verbindungsversuch (WiFiMulti)
    Serial.println("[WLAN] Suche bekannte Netzwerke...");
    if (_wifiMulti.run() == WL_CONNECTED) {
        Serial.print("[WLAN] Verbunden mit: ");
        Serial.println(WiFi.SSID());
    } else {
        Serial.println("[WLAN] Keine bekannten Netze gefunden oder Zeitüberschreitung.");

        // 3. Mesh-Sync Versuch (Daten von Nachbarn holen)
        Serial.println("[MESH] Starte passiven Sync-Versuch...");
        _mesh.init(_meshPrefix, _meshPass, &_userScheduler, 5555);
        _mesh.onReceive(&meshReceivedWrapper);
        _meshStarted = true;

        unsigned long start = millis();
        while(!_syncReceived && (millis() - start < 15000)) {
            _mesh.update();
            if (millis() % 3000 == 0) {
                Serial.println("[MESH] Sende SYNC_REQ...");
                JsonDocument req; req["type"] = "SYNC_REQ";
                String r; serializeJson(req, r); 
                _mesh.sendBroadcast(r);
            }
            delay(1);
        }

        // 4. WiFiManager als letzter Ausweg (Nur für Always-On Knoten)
        if (WiFi.status() != WL_CONNECTED && !_isBatteryPowered) {
            Serial.println("[WM] Starte WiFiManager Portal...");
            if(_meshStarted) { 
                _mesh.stop(); 
                _meshStarted = false; 
                Serial.println("[MESH] Mesh gestoppt für WM-Portal.");
            }
            
            _wm.setConfigPortalTimeout(180);
            // Blockierender Aufruf:
            if(_wm.autoConnect("ESP32_SWARM_AP")) {
                Serial.println("[WM] Neue Daten erhalten! Speichere und starte neu...");
                addNewNetwork(WiFi.SSID(), WiFi.psk());
                delay(1000);
                ESP.restart(); // WICHTIG: Heap säubern!
            }
        }
    }

    // 5. Finaler Mesh-Start für den Dauerbetrieb
    if (!_meshStarted) {
        Serial.println("[MESH] Initialisiere Mesh für Dauerbetrieb...");
        _mesh.init(_meshPrefix, _meshPass, &_userScheduler, 5555);
        _mesh.onReceive(&meshReceivedWrapper);
        _meshStarted = true;
    }

    // 6. Webserver Routen
    _server.on("/", [this](){ handleRoot(); });
    _server.on("/scan", [this](){ handleScan(); });
    _server.on("/view", [this](){ handleView(); });
    _server.on("/delete", [this](){ handleDelete(); });
    _server.on("/add", HTTP_POST, [this](){ handleAdd(); });
    _server.on("/blink", [this](){ 
        Serial.println("[WEB] Blink Command ausgelöst.");
        sendBlinkCommand(); 
        _server.sendHeader("Location", "/"); _server.send(303); 
    });
    _server.on("/reboot", [](){ 
        Serial.println("[WEB] Reboot angefordert.");
        ESP.restart(); 
    });

    Serial.println("[SYSTEM] Setup abgeschlossen.");
}

void SwarmConfigManager::loop() {
    if (_meshStarted) {
        _mesh.update();
    }
    
    if (_serverActive) {
        _server.handleClient();
        // Automatisches Beenden nach 5 Minuten Inaktivität
        if (millis() - _serverStartTime > 300000) {
            _server.stop();
            _serverActive = false;
            Serial.println("[WEB] Admin-Server Timeout erreicht. Gestoppt.");
        }
    }

    // Button-Logic für Admin-Interface
    if (digitalRead(TRIGGER_PIN) == LOW) {
        delay(50);
        if (!_serverActive) {
            _server.begin();
            _serverActive = true;
            _serverStartTime = millis();
            Serial.println("[WEB] Admin-Server via Button gestartet.");
            printSerialQRCode("http://" + WiFi.localIP().toString());
        }
    }

    // Verbindungswiederherstellung im Hintergrund (Alle 60s)
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 60000) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WLAN] Verbindung verloren. Versuche Reconnect...");
            _wifiMulti.run();
        }
        lastCheck = millis();
    }

    if (_isBatteryPowered && WiFi.status() == WL_CONNECTED) {
        Serial.println("[POWER] Batterie-Modus: Aufgabe fertig, schlafen...");
        delay(2000);
        ESP.deepSleep(600e6); // 10 Min
    }
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
    }
    Serial.print("[FS] WiFiMulti Liste aktualisiert. Einträge: ");
    Serial.println(arr.size());
}

void SwarmConfigManager::saveFullConfig(JsonDocument& doc, bool propagate) {
    File f = LittleFS.open(CONFIG_FILE, "w");
    if (serializeJson(doc, f) == 0) {
        Serial.println("[ERROR] Konnte Datei nicht schreiben!");
    }
    f.close();
    updateWiFiMulti();
    if (_meshStarted && propagate) {
        doc["type"] = "SYNC_RES";
        String msg;
        serializeJson(doc, msg);
        _mesh.sendBroadcast(msg);
        Serial.println("[MESH] Neue Config an alle gesendet.");
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
    for(JsonObject n : arr) { 
        if(n["ssid"] == ssid) { n["pass"] = pass; found = true; break; } 
    }
    if(!found) { 
        JsonObject n = arr.add<JsonObject>(); 
        n["ssid"] = ssid; n["pass"] = pass; 
    }
    saveFullConfig(doc, true);
    Serial.println("[FS] Netzwerk hinzugefügt: " + ssid);
}

void SwarmConfigManager::meshReceivedWrapper(uint32_t from, String &msg) {
    JsonDocument doc;
    if (deserializeJson(doc, msg)) return;
    
    if (doc["type"] == "SYNC_REQ" && !_instance->_isBatteryPowered) {
        Serial.println("[MESH] SYNC_REQ erhalten von " + String(from));
        if (LittleFS.exists(CONFIG_FILE)) {
            File f = LittleFS.open(CONFIG_FILE, "r");
            JsonDocument res; deserializeJson(res, f); f.close();
            res["type"] = "SYNC_RES";
            String out; serializeJson(res, out);
            _instance->_mesh.sendSingle(from, out);
        }
    } else if (doc["type"] == "SYNC_RES") {
        if (doc["version"].as<uint32_t>() > _instance->getLocalVersion()) {
            Serial.println("[MESH] Neue Config (SYNC_RES) erhalten!");
            _instance->saveFullConfig(doc, false);
            _instance->_syncReceived = true;
        }
    } else if (doc["type"] == "BLINK_CMD") {
        _instance->blinkLED();
    }
}

void SwarmConfigManager::blinkLED() {
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
}

// --- UI & HTML (Zusammengefasst für Stabilität) ---

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
    DeserializationError error = deserializeJson(doc, _mesh.subConnectionJson());
    if (!error) {
        JsonArray subNodes = doc.as<JsonArray>();
        for (JsonObject node : subNodes) {
            int rssi = node["rssi"] | -100;
            out += "• Node: " + node["nodeId"].as<String>() + " " + getRSSILevel(rssi) + "<br>";
        }
    }
    out += "</div>";
    return out;
}

void SwarmConfigManager::handleRoot() {
    String url = "http://" + WiFi.localIP().toString();
    String html = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<script src='https://cdnjs.cloudflare.com/ajax/libs/qrcodejs/1.0.0/qrcode.min.js'></script>";
    html += "<style>body{font-family:sans-serif; background:#f4f7f9; text-align:center; padding:10px;} .card{background:white; padding:20px; border-radius:15px; box-shadow:0 4px 10px rgba(0,0,0,0.1); max-width:400px; margin:auto;} .btn{display:block; padding:12px; background:#1a73e8; color:white; text-decoration:none; border-radius:8px; margin:10px 0; font-weight:bold;} .mesh-list{text-align:left; font-size:0.85em; background:#eee; padding:10px; border-radius:8px; margin:15px 0; border-left:4px solid #1a73e8;} #qrcode{display:flex; justify-content:center; margin:20px;}</style></head><body>";
    html += "<div class='card'><h1>Swarm Admin</h1><p>Free Heap: " + String(ESP.getFreeHeap()) + " B</p><div id='qrcode'></div>";
    html += getMeshStatusHTML();
    html += "<a href='/scan' class='btn' style='background:#34a853;'>WLAN Scannen</a>";
    html += "<a href='/view' class='btn'>Netzwerke verwalten</a>";
    html += "<a href='/blink' class='btn' style='background:#fbbc04; color:black;'>Alle finden (Blink)</a>";
    html += "<script>new QRCode(document.getElementById('qrcode'), {text:'"+url+"', width:140, height:140});</script></div></body></html>";
    _server.send(200, "text/html", html);
}

// ... (Andere Handler handleScan, handleView, handleDelete identisch zum vorherigen Stand)

void SwarmConfigManager::handleScan() {
    int n = WiFi.scanNetworks();
    String html = "<html><body><h2>Scan</h2><table border='1'>";
    for (int i = 0; i < n; ++i) {
        html += "<tr><td>" + WiFi.SSID(i) + "</td><td><form action='/add' method='POST'><input type='hidden' name='s' value='"+WiFi.SSID(i)+"'><input type='password' name='p'><input type='submit' value='Add'></form></td></tr>";
    }
    html += "</table><br><a href='/'>Back</a></body></html>";
    _server.send(200, "text/html", html);
}

void SwarmConfigManager::handleView() {
    String html = "<html><body><h2>Networks</h2><ul>";
    if (LittleFS.exists(CONFIG_FILE)) {
        File f = LittleFS.open(CONFIG_FILE, "r");
        JsonDocument doc; deserializeJson(doc, f); f.close();
        JsonArray arr = doc["networks"].as<JsonArray>();
        for (int i=0; i<arr.size(); i++) {
            html += "<li>" + arr[i]["ssid"].as<String>() + " <a href='/delete?id=" + String(i) + "'>[Del]</a></li>";
        }
    }
    html += "</ul><a href='/'>Back</a></body></html>";
    _server.send(200, "text/html", html);
}

void SwarmConfigManager::handleDelete() {
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
    if(_server.hasArg("s") && _server.hasArg("p")) addNewNetwork(_server.arg("s"), _server.arg("p"));
    _server.sendHeader("Location", "/"); _server.send(303);
}

void SwarmConfigManager::printSerialQRCode(String url) {
    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qrcodeData, 3, 0, url.c_str());
    Serial.println("\n[ QR SCAN ]");
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

WiFiMulti   SwarmConfigManager::getWifiMulti() {
    return _wifiMulti;
}   
