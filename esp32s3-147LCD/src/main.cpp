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
extern "C"
{
#include "lvgl.h"
}

hw_timer_t *lv_timer = NULL;

// =====================
// SD Card
// =====================
#define SD_CS 4                // Digital I/O used
boolean isSDAvailable = false; // Flag to indicate if SD card is available

// =====================
// MESH
// =====================

#define MESH_ENABLED true
#define MESH_PREFIX "ESP32_SWARM_NET"
#define MESH_PASSWORD "meshpassword123"
#define MESH_PORT 5555

// =====================
// CONFIG
// =====================
const char *CONFIG_FILE = "/networks.json";
const int TRIGGER_PIN = 0; // temp Webserver trigger

// --- GERÄTE-PROFIL ---
bool isBatteryPowered = false; // TRUE für Sensoren (Deep Sleep), FALSE für Router/Anker
bool meshStarted = false;
bool syncReceived = false;

// --- OBJEKTE ---
WiFiMulti wifiMulti;
WebServer server(80);
painlessMesh mesh;
Scheduler userScheduler;

uint8_t lastWLANStatus = 0; // 0 = not connected, 1 = connected

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

void IRAM_ATTR lv_tick_handler()
{
  lv_tick_inc(1);
}

// =====================
// Display Flush
// =====================
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

void wiFiSetup()
{

  WiFi.mode(WIFI_STA);

  // Add list of wifi networks
  // TODO: should be stored in non-volatile memory and added in setup() instead of hardcoded here

  wifiMulti.addAP("WAP3-B5", "jps123jps");
  wifiMulti.addAP("WAP-B5", "jps123jps");
  wifiMulti.addAP("SMEK01", "00000000");

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

uint32_t getLocalVersion()
{
  if (!LittleFS.exists(CONFIG_FILE))
    return 0;
  File f = LittleFS.open(CONFIG_FILE, "r");
  JsonDocument doc;
  deserializeJson(doc, f);
  f.close();
  return doc["version"] | 0;
}

void saveFullConfig(JsonDocument &doc, bool propagate)
{
  File f = LittleFS.open(CONFIG_FILE, "w");
  serializeJson(doc, f);
  f.close();

  // WiFiMulti aktualisieren
  JsonArray arr = doc["networks"].as<JsonArray>();
  for (JsonObject net : arr)
  {
    wifiMulti.addAP(net["ssid"], net["pass"]);
  }

  if (MESH_ENABLED && propagate)
  {
    String msg;
    doc["type"] = "SYNC_RES";
    serializeJson(doc, msg);
    mesh.sendBroadcast(msg);
    Serial.println("Mesh: Neue Config verteilt!");
  }
}

// Einzelnes Netz hinzufügen (über Webserver/Manager)
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
  JsonArray arr = doc["networks"].to<JsonArray>(); // Reset/Update Array

  // Einfachheitshalber: Bestehendes suchen oder neu
  bool found = false;
  for (JsonObject n : arr)
  {
    if (n["ssid"] == ssid)
    {
      n["pass"] = pass;
      found = true;
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

void receivedCallback(uint32_t from, String &msg)
{
  JsonDocument doc;
  if (deserializeJson(doc, msg))
    return;
  String type = doc["type"];

  if (type == "SYNC_REQ" && !isBatteryPowered)
  {
    // Always-On Gerät antwortet auf Anfrage
    File f = LittleFS.open(CONFIG_FILE, "r");
    JsonDocument res;
    deserializeJson(res, f);
    f.close();
    res["type"] = "SYNC_RES";
    String out;
    serializeJson(res, out);
    mesh.sendSingle(from, out);
  }
  else if (type == "SYNC_RES")
  {
    uint32_t remoteVer = doc["version"];
    if (remoteVer > getLocalVersion())
    {
      Serial.printf("Update von %u erhalten (V%d). Speichere...\n", from, remoteVer);
      saveFullConfig(doc, false);
      syncReceived = true;
    }
  }
}

// --- WEB SERVER ---

// void handleRoot() {
//     String html = "<h1>Swarm Admin</h1><p>Version: " + String(getLocalVersion()) + "</p>";
//     html += "<a href='/scan'>Scan New WiFi</a>";
//     server.send(200, "text/html", html);
// }

void handleRoot()
{
  String html = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: 'Segoe UI', sans-serif; background: #f0f2f5; color: #333; margin: 0; padding: 20px; }";
  html += ".container { max-width: 600px; margin: auto; }";
  html += ".card { background: white; padding: 20px; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); margin-bottom: 20px; }";
  html += "h1 { color: #1a73e8; font-size: 24px; margin-top: 0; }";
  html += ".stat-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 20px; }";
  html += ".stat-item { background: #e8f0fe; padding: 15px; border-radius: 8px; text-align: center; }";
  html += ".stat-label { font-size: 12px; color: #5f6368; text-transform: uppercase; font-weight: bold; }";
  html += ".stat-value { font-size: 20px; color: #1a73e8; font-weight: bold; }";
  html += ".btn { display: block; width: 100%; padding: 12px; text-align: center; background: #1a73e8; color: white; border-radius: 6px; text-decoration: none; font-weight: bold; margin-top: 10px; }";
  html += ".btn-scan { background: #34a853; }";
  html += ".btn-reset { background: #ea4335; margin-top: 30px; font-size: 12px; padding: 8px; opacity: 0.7; }";
  html += "</style></head><body>";

  html += "<div class='container'>";
  html += "<div class='card'>";
  html += "<h1>ESP32 Swarm Node</h1>";

  // Status-Leiste
  html += "<div class='stat-grid'>";
  html += "<div class='stat-item'><div class='stat-label'>Config Version</div><div class='stat-value'>v" + String(getLocalVersion()) + "</div></div>";

  // Mesh-Knoten zählen
  int meshNodes = (MESH_ENABLED && meshStarted) ? mesh.getNodeList().size() + 1 : 1;
  html += "<div class='stat-item'><div class='stat-label'>Mesh Knoten</div><div class='stat-value'>" + String(meshNodes) + "</div></div>";
  html += "</div>";

  html += "<p><b>IP:</b> " + WiFi.localIP().toString() + "<br>";
  html += "<b>Uptime:</b> " + String(millis() / 60000) + " min</p>";

  html += "<a href='/scan' class='btn btn-scan'>Neues WLAN scannen & verteilen</a>";
  html += "<a href='/view' class='btn'>Gespeicherte Netze verwalten</a>";
  html += "<a href='/reboot' class='btn btn-reset' onclick=\"return confirm('ESP neu starten?')\">Gerät Neustarten</a>";
  html += "</div>";

  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

// --- HAUPTABLAUF ---

void setupMeshEtAL()
{

  LittleFS.begin(true);

  pinMode(TRIGGER_PIN, INPUT_PULLUP);

  // 1. Lokale Netze in WiFiMulti laden
  uint32_t ver = getLocalVersion();
  if (ver > 0)
  {
    File f = LittleFS.open(CONFIG_FILE, "r");
    JsonDocument doc;
    deserializeJson(doc, f);
    f.close();
    for (JsonObject n : doc["networks"].as<JsonArray>())
      wifiMulti.addAP(n["ssid"], n["pass"]);
  }

  // 2. Erster Connect-Versuch
  if (wifiMulti.run() != WL_CONNECTED)
  {
    if (MESH_ENABLED)
    {
      Serial.println("WiFi Fail. Suche Mesh Sync...");
      mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
      mesh.onReceive(&receivedCallback);
      meshStarted = true;

      // Sync Request senden
      unsigned long start = millis();
      while (!syncReceived && millis() - start < 15000)
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
    }
  }

  // 3. Fallback auf WiFiManager (nur für Always-On)
  if (WiFi.status() != WL_CONNECTED && !isBatteryPowered)
  {
    WiFiManager wm;
    if (wm.autoConnect("ESP32_SWARM_INITIAL"))
    {
      addNewNetwork(WiFi.SSID(), WiFi.psk());
    }
  }

  // Always-On Mesh permanent starten
  if (MESH_ENABLED && !meshStarted && !isBatteryPowered)
  {
    mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
    mesh.onReceive(&receivedCallback);
    mesh.stationManual(MESH_PREFIX, MESH_PASSWORD);
    meshStarted = true;
  }

  server.on("/", handleRoot);

  server.on("/reboot", []()
            {
    server.send(200, "text/plain", "Rebooting...");
    delay(1000);
    ESP.restart(); });

  // Hier weitere Routen (Scan, Delete etc. wie oben) einfügen
}

// =====================

void setup()
{
  Serial.begin(115200);

  // SD Card
  initSD();

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
  lv_timer_handler();

  if (meshStarted)
    mesh.update();
  server.handleClient(); // Webserver bedienen

  // Button für Webserver
  if (digitalRead(TRIGGER_PIN) == LOW)
  {
    server.begin();
    Serial.println("Webserver gestartet!");
    delay(1000);
  }

  // Batterie-Logik: Schlafen wenn fertig
  if (isBatteryPowered && (WiFi.status() == WL_CONNECTED || millis() > 60000))
  {
    Serial.println("Going to sleep...");
    ESP.deepSleep(600e6); // 10 Min
  }

  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 30000)
  {
    if (WiFi.status() != WL_CONNECTED)
      wifiMulti.run();
    lastWiFiCheck = millis();
  }

  static uint32_t last = 0;
  if (millis() - last > 1000)
  {
    last = millis();
    update_time();
    update_wifi_status();
  }

  delay(5);
}