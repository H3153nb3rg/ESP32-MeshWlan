/**
 * @file main.cpp
 * @brief Main application entry point for ESP32-S3 Mesh WLAN node with Display.
 *
 * This application handles WiFi connectivity (Multi-AP), Mesh networking (painlessMesh),
 * Configuration persistence (LittleFS/JSON), WebServer for administration, and a GUI using LVGL on an ST7789 display.
 */
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <time.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <painlessMesh.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <qrcode.h>

extern "C"
{
#include "lvgl.h"
}

// Hardware timer for LVGL ticking
hw_timer_t *lv_timer = NULL;

// =====================
// SD Card
// =====================
#define SD_CS 4             // Digital I/O used
bool isSDAvailable = false; // Flag to indicate if SD card is available

// =====================
// MESH
// =====================

#define MESH_ENABLED true
#define MESH_PREFIX "ESP32_SWARM_NET"
#define MESH_PASSWORD "meshpassword123"
#define MESH_PORT 5555

// PING LED PIN
#define LED_PIN 2 // Standard ESP32 DevKit LED

// =====================
// ACCESS POINT
// =====================
#define ESP32_SWARM_AP "ESP32_SWARM_AP"

// =====================
// CONFIG
// =====================
const char *CONFIG_FILE = "/networks.json"; // Path to the configuration file in LittleFS
const int TRIGGER_PIN = 0;                  // Button pin to trigger the webserver (Boot button on many ESP32 boards)

// --- GERÄTE-PROFIL ---
bool isBatteryPowered = false; // TRUE for sensors (Deep Sleep), FALSE for routers/anchors (Always On)
bool meshStarted = false;
bool syncReceived = false;
bool webserverActive = false;
unsigned long serverStartTime = 0;

// --- OBJEKTE ---
WiFiMulti wifiMulti;
WiFiManager wm;
WebServer server(80);
painlessMesh mesh;
Scheduler userScheduler;

uint8_t lastWLANStatus = 0; // Cache for WLAN status: 0 = not connected, 1 = connected

// WiFi connect timeout per AP. Increase when connecting takes longer.
const uint32_t connectTimeoutMs = 10000;

// --- DATENSTRUKTUR ---
struct Config
{
  uint32_t version;
  JsonArray networks;
};

// NTP Info
const char *strNTP = "at.pool.ntp.org";
const long gmtOffset_sec = 3600;     // UTC+1 for Austria
const int daylightOffset_sec = 3600; // +1 hour for DST
// =====================
// Display Größe
// =====================
#define LCD_WIDTH 320
#define LCD_HEIGHT 172

// =====================
// Pins
// =====================
#define TFT_MOSI 45
#define TFT_SCLK 40
#define TFT_CS 42
#define TFT_DC 41
#define TFT_RST 39
#define TFT_BL 48

// =====================
// Task Handles
// =====================
TaskHandle_t TaskWlan_ntp;

// Display Objekt
// =====================
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// =====================
// LVGL Buffer
// =====================
static lv_color_t buf1[LCD_WIDTH * 40];
static lv_disp_draw_buf_t draw_buf;

// =====================
// LVGL Objekte
// =====================
lv_obj_t *label_time;
lv_obj_t *label_wifi;

// Interrupt Service Routine for LVGL tick
void IRAM_ATTR lv_tick_handler()
{
  lv_tick_inc(1);
}

// =====================
// Display Flush
// =====================
// Callback function for LVGL to flush the display buffer to the hardware
void my_disp_flush(lv_disp_drv_t *disp,
                   const lv_area_t *area,
                   lv_color_t *color_p)
{
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.writePixels((uint16_t *)color_p, w * h);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

// =====================
// WLAN verbinden
// =====================
// Scans for networks and connects using WiFiMulti

void wiFiSetup()
{

  WiFi.mode(WIFI_STA);

  // WiFi.scanNetworks will return the number of networks found
  int n = WiFi.scanNetworks();
  Serial.println("scan done");
  if (n == 0)
  {
    Serial.println("no networks found");
  }
  else
  {
    Serial.print(n);
    Serial.println(" networks found");
    for (int i = 0; i < n; ++i)
    {
      // Print SSID and RSSI for each network found
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(WiFi.SSID(i));
      Serial.print(" (");
      Serial.print(WiFi.RSSI(i));
      Serial.print(")");
      Serial.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? " " : "*");
      delay(10);
    }
  }
  // Connect to Wi-Fi using wifiMulti (connects to the SSID with strongest connection)
  Serial.println("Connecting Wifi...");
  if (wifiMulti.run() == WL_CONNECTED)
  {
    Serial.println("");
    Serial.println("WiFi connected:");
    Serial.println(WiFi.SSID().c_str());
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
  }
}

// Legacy connection function (unused)
// void wifi_connect()
// {
//   WiFi.begin(ssid, password);
//   Serial.print("WLAN verbinden");

//   while (WiFi.status() != WL_CONNECTED)
//   {
//     delay(500);
//     Serial.print(".");
//   }

//   Serial.println("\nWLAN verbunden");
// }

// =====================
// Zeit aktualisieren
// =====================
// Updates the time label on the LVGL GUI
void update_time()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    return;

  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
  lv_label_set_text(label_time, buf);
}

// =====================
// WLAN Status aktualisieren
// =====================
// Checks WiFi status and updates the GUI label with IP, SSID, and RSSI
void update_wifi_status()
{
  // if (WiFi.status() == WL_CONNECTED)
  if (wifiMulti.run(connectTimeoutMs) == WL_CONNECTED)
  {
    if (lastWLANStatus == 0)
    {
      char infoStr[255];
      sprintf(infoStr, "%s@%s (%ddBm)", WiFi.localIP().toString().c_str(), WiFi.SSID().c_str(), WiFi.RSSI());

      lv_label_set_text(label_wifi, infoStr);
      lastWLANStatus = 1;
    }
  }
  else
  {
    if (lastWLANStatus == 1)
    {
      lv_label_set_text(label_wifi, "NO WLAN");
      lastWLANStatus = 0;
    }
  }
}

// =====================
// Hintergrundtask für WLAN und NTP aktualisieren
// =====================
void updateTimeWifiStatusTask(void *pvParameters)
{

  Serial.print("Task running on core ");
  Serial.println(xPortGetCoreID());

  for (;;)
  {

    update_time();
    update_wifi_status();

    delay(1000);
  }
}

// Initializes the SD card interface
void initSD()
{
  if (!SD.begin(SD_CS))
  {
    Serial.println("Card Mount Failed");
    isSDAvailable = false;
    return;
  }

  uint8_t cardType = SD.cardType();

  if (cardType == CARD_NONE)
  {
    isSDAvailable = false;
    Serial.println("No SD card attached");
    return;
  }

  Serial.print("SD Card Type: ");
  if (cardType == CARD_MMC)
  {
    Serial.println("MMC");
  }
  else if (cardType == CARD_SD)
  {
    Serial.println("SDSC");
  }
  else if (cardType == CARD_SDHC)
  {
    Serial.println("SDHC");
  }
  else
  {
    Serial.println("UNKNOWN");
  }

  isSDAvailable = true;
}

// =====================

// Initializes the GUI: Backlight, SPI, TFT, LVGL, Timer, and UI elements
void initGUI()
{

  // Backlight
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // SPI
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);

  // Display
  tft.init(LCD_HEIGHT, LCD_WIDTH);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);

  // LVGL
  lv_init();
  lv_timer = timerBegin(0, 80, true); // 80 MHz / 80 = 1 MHz

  // Setup timer interrupt for LVGL ticking
  timerAttachInterrupt(lv_timer, &lv_tick_handler, true);
  timerAlarmWrite(lv_timer, 1000, true); // 1 ms
  timerAlarmEnable(lv_timer);

  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, LCD_WIDTH * 40);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = LCD_WIDTH;
  disp_drv.ver_res = LCD_HEIGHT;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  // UI
  label_time = lv_label_create(lv_scr_act());
  lv_label_set_text(label_time, "--:--:--");
  lv_obj_set_style_text_font(label_time, &lv_font_montserrat_48, 0);
  lv_obj_align(label_time, LV_ALIGN_CENTER, 0, -20);

  label_wifi = lv_label_create(lv_scr_act());
  lv_label_set_text(label_wifi, "📡 WLAN...");
  lv_obj_align(label_wifi, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// --- FUNKTIONEN: PERSISTENZ & LOGIK ---

// Reads the configuration version from the local JSON file
uint32_t getLocalVersion()
{
  if (!LittleFS.exists(CONFIG_FILE))
    return 0;
  File f = LittleFS.open(CONFIG_FILE, "r");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  return (err == DeserializationError::Ok) ? doc["version"] : 0;
}

// Loads WiFi credentials from the config file into WiFiMulti
void updateWiFiMulti()
{
  if (!LittleFS.exists(CONFIG_FILE))
    return;
  File f = LittleFS.open(CONFIG_FILE, "r");
  JsonDocument doc;
  deserializeJson(doc, f);
  f.close();
  JsonArray arr = doc["networks"].as<JsonArray>();
  for (JsonObject n : arr)
  {
    wifiMulti.addAP(n["ssid"].as<const char *>(), n["pass"].as<const char *>());
  }
}

// Saves the configuration to LittleFS and optionally broadcasts it to the Mesh
void saveFullConfig(JsonDocument &doc, bool propagate)
{
  File f = LittleFS.open(CONFIG_FILE, "w");
  serializeJson(doc, f);
  f.close();
  updateWiFiMulti();

  if (MESH_ENABLED && meshStarted && propagate)
  {
    doc["type"] = "SYNC_RES";
    String msg;
    serializeJson(doc, msg);
    mesh.sendBroadcast(msg);
    Serial.println("Mesh: Update propagiert!");
  }
}

// Adds a single network to the configuration (via Webserver/Manager)
void addNewNetwork(String ssid, String pass)
{
  JsonDocument doc;
  if (LittleFS.exists(CONFIG_FILE))
  {
    File f = LittleFS.open(CONFIG_FILE, "r");
    deserializeJson(doc, f);
    f.close();
  }

  uint32_t currentVer = doc["version"] | 0;
  doc["version"] = currentVer + 1;

  JsonArray arr = doc["networks"].as<JsonArray>();
  if (arr.isNull())
    arr = doc["networks"].to<JsonArray>();

  bool found = false;
  for (JsonObject n : arr)
  {
    if (n["ssid"] == ssid)
    {
      n["pass"] = pass;
      found = true;
      break;
    }
  }
  if (!found)
  {
    JsonObject n = arr.add<JsonObject>();
    n["ssid"] = ssid;
    n["pass"] = pass;
  }
  saveFullConfig(doc, true);
}

// --- MESH CALLBACKS ---

void blinkLED()
{
  digitalWrite(LED_PIN, HIGH);
  delay(500); // Kurz leuchten
  digitalWrite(LED_PIN, LOW);
}

// Callback when a message is received via Mesh
void receivedCallback(uint32_t from, String &msg)
{
  JsonDocument doc;
  if (deserializeJson(doc, msg))
    return;
  String type = doc["type"];

  // Handle custom command (e.g., "BLINK_CMD") to trigger LED blinking
  if (type == "BLINK_CMD")
  {
    Serial.println("Blink-Befehl erhalten!");
    blinkLED();
  }

  // Handle synchronization request
  if (type == "SYNC_REQ" && !isBatteryPowered)
  {
    if (LittleFS.exists(CONFIG_FILE))
    {
      File f = LittleFS.open(CONFIG_FILE, "r");
      JsonDocument res;
      deserializeJson(res, f);
      f.close();
      res["type"] = "SYNC_RES";
      String out;
      serializeJson(res, out);
      mesh.sendSingle(from, out);
    }
  }
  // Handle synchronization response (update local config if newer version received)
  else if (type == "SYNC_RES")
  {
    if (doc["version"].as<uint32_t>() > getLocalVersion())
    {
      saveFullConfig(doc, false);
      syncReceived = true;
      Serial.printf("Update von %u erhalten (V%d). Speichere...\n", from, doc["version"].as<uint32_t>());
    }
  }
}

// Funktion zum Senden des Befehls
void sendBlinkCommand()
{
  JsonDocument doc;
  doc["type"] = "BLINK_CMD";
  String msg;
  serializeJson(doc, msg);
  mesh.sendBroadcast(msg);
  blinkLED(); // Auch die eigene LED blinken lassen
}

// --- WEB SERVER ---

// --- MESH RSSI HELPER ---
// Erstellt eine Liste der Nachbarn mit Signalstärke
// Hilfsfunktion für die Balkenanzeige

String getRSSILevel(int rssi)
{
  if (rssi > -55)
    return "<span style='color:#34a853;'>▂▄▆█</span>"; // Exzellent
  if (rssi > -70)
    return "<span style='color:#fbbc04;'>▂▄▆</span><span style='color:#ccc;'>█</span>"; // Gut
  if (rssi > -85)
    return "<span style='color:#ea4335;'>▂▄</span><span style='color:#ccc;'>▆█</span>"; // Schwach
  return "<span style='color:#ea4335;'>▂</span><span style='color:#ccc;'>▄▆█</span>";   // Sehr schwach
}

String getMeshStatusHTML()
{
  String out = "<div class='mesh-list'><b>Mesh Status:</b><br>";
  out += "• Local ID: " + String(mesh.getNodeId()) + "<br>";

  String json = mesh.subConnectionJson();
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);

  if (!error && doc.is<JsonArray>())
  {
    JsonArray subNodes = doc.as<JsonArray>();

    if (subNodes.size() == 0)
    {
      out += "<i style='color:gray;'>Suche Nachbarn...</i><br>";
    }

    for (JsonObject node : subNodes)
    {
      uint32_t id = node["nodeId"];
      int rssi = node["rssi"] | -100; // Standardwert falls nicht vorhanden

      out += "• Node: " + String(id) + " " + getRSSILevel(rssi);
      out += " <small>(" + String(rssi) + " dBm)</small><br>";
    }
  }
  else
  {
    // Fallback: Wenn JSON noch leer ist, zeige einfache Liste
    std::list<uint32_t> nodeList = mesh.getNodeList();
    for (uint32_t node : nodeList)
    {
      if (node != mesh.getNodeId())
      {
        out += "• Node: " + String(node) + " <span style='color:gray;'>▂▄▆█</span><br>";
      }
    }
  }

  out += "</div>";
  return out;
}
// void handleRoot() {
//     String html = "<h1>Swarm Admin</h1><p>Version: " + String(getLocalVersion()) + "</p>";
//     html += "<a href='/scan'>Scan New WiFi</a>";
//     server.send(200, "text/html", html);
// }

// // Handles the root URL of the webserver, displaying status and controls
// void handleRoot()
// {
//   String html = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
//   html += "<style>";
//   html += "body { font-family: 'Segoe UI', sans-serif; background: #f0f2f5; color: #333; margin: 0; padding: 20px; }";
//   html += ".container { max-width: 600px; margin: auto; }";
//   html += ".card { background: white; padding: 20px; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); margin-bottom: 20px; }";
//   html += "h1 { color: #1a73e8; font-size: 24px; margin-top: 0; }";
//   html += ".stat-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 20px; }";
//   html += ".stat-item { background: #e8f0fe; padding: 15px; border-radius: 8px; text-align: center; }";
//   html += ".stat-label { font-size: 12px; color: #5f6368; text-transform: uppercase; font-weight: bold; }";
//   html += ".stat-value { font-size: 20px; color: #1a73e8; font-weight: bold; }";
//   html += ".btn { display: block; width: 100%; padding: 12px; text-align: center; background: #1a73e8; color: white; border-radius: 6px; text-decoration: none; font-weight: bold; margin-top: 10px; }";
//   html += ".btn-scan { background: #34a853; }";
//   html += ".btn-reset { background: #ea4335; margin-top: 30px; font-size: 12px; padding: 8px; opacity: 0.7; }";
//   html += "</style></head><body>";

//   html += "<div class='container'>";
//   html += "<div class='card'>";
//   html += "<h1>ESP32 Swarm Node</h1>";

//   // Status-Leiste
//   html += "<div class='stat-grid'>";
//   html += "<div class='stat-item'><div class='stat-label'>Config Version</div><div class='stat-value'>v" + String(getLocalVersion()) + "</div></div>";

//   // Mesh-Knoten zählen
//   int meshNodes = (MESH_ENABLED && meshStarted) ? mesh.getNodeList().size() + 1 : 1;
//   html += "<div class='stat-item'><div class='stat-label'>Mesh Knoten</div><div class='stat-value'>" + String(meshNodes) + "</div></div>";
//   html += "</div>";

//   html += "<p><b>IP:</b> " + WiFi.localIP().toString() + "<br>";
//   html += "<b>Uptime:</b> " + String(millis() / 60000) + " min</p>";

//   html += "<a href='/scan' class='btn btn-scan'>Neues WLAN scannen & verteilen</a>";
//   html += "<a href='/view' class='btn'>Gespeicherte Netze verwalten</a>";
//   html += "<a href='/reboot' class='btn btn-reset' onclick=\"return confirm('ESP neu starten?')\">Gerät Neustarten</a>";

//   html += "</div>";

//   html += "</div></body></html>";

//   server.send(200, "text/html", html);
// }

// void handleRoot() {
//     String html = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'><style>body{font-family:sans-serif; background:#f0f2f5; padding:20px;} .card{background:white; padding:20px; border-radius:10px; box-shadow:0 2px 5px rgba(0,0,0,0.1); max-width:400px; margin:auto;} .btn{display:block; padding:10px; background:#1a73e8; color:white; text-align:center; text-decoration:none; border-radius:5px; margin:10px 0;}</style></head><body>";
//     html += "<div class='card'><h1>Swarm Node</h1>";
//     html += "<p>Version: <b>v" + String(getLocalVersion()) + "</b></p>";
//     html += "<p>Mesh Knoten: <b>" + String(mesh.getNodeList().size() + 1) + "</b></p>";
//     html += "<a href='/scan' class='btn'>WLAN Scannen</a>";
//     html += "<a href='/view' class='btn'>Netze verwalten</a>";
//     html += "<a href='/reboot' style='color:red; font-size:0.8em;'>Neustart</a></div></body></html>";
//     server.send(200, "text/html", html);
// }

void handleRoot()
{
  String url = "http://" + WiFi.localIP().toString();
  if (WiFi.status() != WL_CONNECTED)
    url = "http://192.168.4.1";

  String html = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  // QR Code Script (Browser-Seite)
  html += "<script src='https://cdnjs.cloudflare.com/ajax/libs/qrcodejs/1.0.0/qrcode.min.js'></script>";

  html += "<style>";
  html += "body{font-family:sans-serif; background:#f4f7f9; text-align:center; padding:10px;}";
  html += ".card{background:white; padding:20px; border-radius:15px; box-shadow:0 4px 10px rgba(0,0,0,0.1); max-width:400px; margin:auto;}";
  html += ".btn{display:block; padding:12px; background:#1a73e8; color:white; text-decoration:none; border-radius:8px; margin:10px 0; font-weight:bold;}";
  html += ".mesh-list{text-align:left; font-size:0.85em; background:#eee; padding:10px; border-radius:8px; margin:15px 0; border-left:4px solid #1a73e8;}";
  html += "#qrcode{display:flex; justify-content:center; margin:20px;}";
  html += "</style></head><body>";

  html += "<div class='card'><h1>Swarm Admin</h1>";
  html += "<p>Config Version: <b>v" + String(getLocalVersion()) + "</b></p>";

  // QR Code Container
  html += "<div id='qrcode'></div>";

  // Mesh Status mit RSSI
  html += getMeshStatusHTML();

  html += "<a href='/scan' class='btn' style='background:#34a853;'>WLAN Scannen</a>";
  html += "<a href='/view' class='btn'>Netzwerke verwalten</a>";
  html += "<a href='/blink' class='btn' style='background:#fbbc04; color:black;'>Alle Knoten finden (Blink)</a>";
  html += "<p style='font-size:0.7em; color:gray;'>IP: " + url + "</p>";

  // JS für QR Code Generierung im Browser
  html += "<script>new QRCode(document.getElementById('qrcode'), {text:'" + url + "', width:140, height:140, colorDark:'#000000', colorLight:'#ffffff'});</script>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

void handleView()
{
  String html = "<html><body><h2>Gespeicherte Netze</h2><table border='1'><tr><th>SSID</th><th>Aktion</th></tr>";
  if (LittleFS.exists(CONFIG_FILE))
  {
    File f = LittleFS.open(CONFIG_FILE, "r");
    JsonDocument doc;
    deserializeJson(doc, f);
    f.close();
    JsonArray arr = doc["networks"].as<JsonArray>();
    for (int i = 0; i < arr.size(); i++)
    {
      html += "<tr><td>" + arr[i]["ssid"].as<String>() + "</td><td><a href='/delete?id=" + String(i) + "'>Löschen</a></td></tr>";
    }
  }
  html += "</table><br><a href='/'>Zurück</a></body></html>";
  server.send(200, "text/html", html);
}

void handleScan()
{
  int n = WiFi.scanNetworks();
  String html = "<html><body><h2>Verfügbare Netze</h2><table border='1'>";
  for (int i = 0; i < n; ++i)
  {
    html += "<tr><td>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)</td>";
    html += "<td><form action='/add' method='POST'><input type='hidden' name='s' value='" + WiFi.SSID(i) + "'><input type='password' name='p'><input type='submit' value='Hinzufügen'></form></td></tr>";
  }
  html += "</table><br><a href='/'>Zurück</a></body></html>";
  server.send(200, "text/html", html);
}

void handleDelete()
{
  if (server.hasArg("id"))
  {
    JsonDocument doc;
    File f = LittleFS.open(CONFIG_FILE, "r");
    deserializeJson(doc, f);
    f.close();
    doc["networks"].as<JsonArray>().remove(server.arg("id").toInt());
    doc["version"] = doc["version"].as<uint32_t>() + 1;
    saveFullConfig(doc, true);
  }
  server.sendHeader("Location", "/view");
  server.send(303);
}

void printQRCode()
{
  String url = "http://" + WiFi.localIP().toString() + '/';

  // QR-Code Struktur initialisieren
  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(3)];
  qrcode_initText(&qrcode, qrcodeData, 3, 0, url.c_str());

  Serial.println("\n--- Scan zum Öffnen des Web-Interface ---");

  // Um den QR-Code im Serial Monitor weiß auf schwarz darzustellen
  // nutzen wir zwei Zeilen für die Vertikale, um die Proportionen zu halten
  for (uint8_t y = 0; y < qrcode.size; y++)
  {
    Serial.print("  "); // Linker Rand
    for (uint8_t x = 0; x < qrcode.size; x++)
    {
      if (qrcode_getModule(&qrcode, x, y))
      {
        Serial.print("\u2588\u2588"); // Schwarzes Quadrat (Unicode Full Block)
      }
      else
      {
        Serial.print("  "); // Weißes Quadrat
      }
    }
    Serial.println();
  }
  Serial.println("URL: " + url);
  Serial.println("------------------------------------------\n");
}

// Sets up Mesh, Filesystem, WiFiManager, and Webserver routes
void setupMeshEtAL()
{

  LittleFS.begin(true);

  pinMode(TRIGGER_PIN, INPUT_PULLUP);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  if (!LittleFS.begin(true))
  {
    Serial.println("FS Error");
    return;
  }

  // 1. Initialisiere WiFi & Multi
  updateWiFiMulti();

  // 2. Prepare WiFiManager (Non-Blocking)
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(180);

  // 3. Prepare Mesh
  if (MESH_ENABLED)
  {
    mesh.setDebugMsgTypes(ERROR | STARTUP);
    mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
    mesh.onReceive(&receivedCallback);
    mesh.stationManual(MESH_PREFIX, MESH_PASSWORD);
    meshStarted = true;
  }

  // 4. Attempt Connection
  if (wifiMulti.run() != WL_CONNECTED)
  {
    Serial.println("WiFiMulti fehlgeschlagen. Starte Mesh-Sync...");

    // Try to get data via Mesh for 10 seconds
    unsigned long start = millis();
    while (!syncReceived && millis() - start < 10000)
    {
      if (millis() % 3000 == 0)
      {
        JsonDocument req;
        req["type"] = "SYNC_REQ";
        String r;
        serializeJson(req, r);
        mesh.sendBroadcast(r);
      }
      mesh.update();
      delay(1);
    }

    // If still no WLAN, open WiFiManager AP
    if (WiFi.status() != WL_CONNECTED && !isBatteryPowered)
    {
      wm.startConfigPortal(ESP32_SWARM_AP);
    }
  }

  server.on("/", handleRoot);

  server.on("/reboot", []()
            {
    server.send(200, "text/plain", "Rebooting...");
    delay(1000);
    ESP.restart(); });

  server.on("/blink", []()
            {
              sendBlinkCommand();
              server.sendHeader("Location", "/");
              server.send(303); // Zurück zur Hauptseite
            });

  server.on("/view", handleView);
  server.on("/scan", handleScan);
  server.on("/delete", handleDelete);
}

// =====================

void setup()
{
  Serial.begin(115200);

  // SD Card
  // initSD();

  initGUI();

  // WLAN + NTP
  setupMeshEtAL();
  // wiFiSetup();

  configTime(3600, 3600, strNTP); // MEZ (+1h)

  // setup async background tasks
  // temp wieder weg

  // xTaskCreate(
  //     updateTimeWifiStatusTask, /* Task function. */
  //     "WLAN_NTP",               /* name of task. */
  //     10000,                    /* Stack size of task */
  //     NULL,                     /* parameter of the task */
  //     1,                        /* priority of the task */
  //     &TaskWlan_ntp);           /* Task handle to keep track of created task */
}

void loop()
{
  lv_timer_handler(); // Handle LVGL tasks

  wm.process(); // WiFiManager background tasks
  if (meshStarted)
    mesh.update();
  if (webserverActive)
    server.handleClient();

  // WiFiManager Success Check: If connected in AP mode, save creds and switch to STA
  static bool apWasActive = false;
  if (WiFi.status() == WL_CONNECTED && WiFi.getMode() & WIFI_AP)
  {
    Serial.println("Verbunden! Schalte AP aus.");
    addNewNetwork(WiFi.SSID(), WiFi.psk());
    WiFi.mode(WIFI_STA); // AP deaktivieren
  }

  // Webserver Trigger (Button): Start admin server on button press
  if (digitalRead(TRIGGER_PIN) == LOW)
  {
    delay(50);
    if (!webserverActive)
    {
      server.begin();
      webserverActive = true;
      serverStartTime = millis();
      Serial.println("Webserver gestartet!");

      // RUFE HIER DEN QR-CODE AUF
      if (WiFi.status() == WL_CONNECTED)
      {
        printQRCode();
      }
      else
      {
        Serial.println("IP: 192.168.4.1 (Portal Modus)");
      }
    }
  }

  // Admin-Server Timeout: Stop server after 5 minutes of inactivity
  if (webserverActive && millis() - serverStartTime > 300000)
  {
    server.stop();
    webserverActive = false;
  }

  // WiFi Reconnect Logic: Check connection every 20 seconds
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 20000)
  {
    if (WiFi.status() != WL_CONNECTED)
      wifiMulti.run();
    lastCheck = millis();
  }

  // Battery Logic: Sleep when finished or after timeout
  if (isBatteryPowered && (WiFi.status() == WL_CONNECTED || millis() > 60000))
  {
    Serial.println("atterie-Modus:Going to sleep...");
    ESP.deepSleep(600e6); // 10 Min
  }

  // Periodic UI update (every 1 second)
  static uint32_t last = 0;
  if (millis() - last > 1000)
  {
    last = millis();
    update_time();
    update_wifi_status();
  }

  delay(5);
}