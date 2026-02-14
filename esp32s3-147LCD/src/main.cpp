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

#include "SwarmConfigManager.h"

extern "C"
{
#include "lvgl.h"
}

// Hardware timer for LVGL ticking
hw_timer_t *lv_timer = NULL;

// =====================
// SwarmConfigManager 
// see *.h for defines
// =====================

// Konstruktor: (isBatteryPowered, MeshName, MeshPassword)
SwarmConfigManager swarm(false, MESH_PREFIX, MESH_PASSWORD);

uint8_t lastWLANStatus = 0; // Cache for WLAN status: 0 = not connected, 1 = connected

// WiFi connect timeout per AP. Increase when connecting takes longer.
const uint32_t connectTimeoutMs = 10000;

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

// // =====================
// // WLAN verbinden
// // =====================
// // Scans for networks and connects using WiFiMulti

// void wiFiSetup()
// {

//   WiFi.mode(WIFI_STA);

//   // WiFi.scanNetworks will return the number of networks found
//   int n = WiFi.scanNetworks();
//   Serial.println("scan done");
//   if (n == 0)
//   {
//     Serial.println("no networks found");
//   }
//   else
//   {
//     Serial.print(n);
//     Serial.println(" networks found");
//     for (int i = 0; i < n; ++i)
//     {
//       // Print SSID and RSSI for each network found
//       Serial.print(i + 1);
//       Serial.print(": ");
//       Serial.print(WiFi.SSID(i));
//       Serial.print(" (");
//       Serial.print(WiFi.RSSI(i));
//       Serial.print(")");
//       Serial.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? " " : "*");
//       delay(10);
//     }
//   }
//   // Connect to Wi-Fi using wifiMulti (connects to the SSID with strongest connection)
//   Serial.println("Connecting Wifi...");
//   if (wifiMulti.run() == WL_CONNECTED)
//   {
//     Serial.println("");
//     Serial.println("WiFi connected:");
//     Serial.println(WiFi.SSID().c_str());
//     Serial.println("IP address: ");
//     Serial.println(WiFi.localIP());
//   }
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
  if (swarm.getWifiMulti().run(connectTimeoutMs) == WL_CONNECTED)
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






// =====================

void setup()
{
  Serial.begin(115200);

  // SD Card
  // initSD();

  initGUI();
  
  swarm.setup();
  

  // WLAN + NTP
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

  swarm.loop(); // Handle Mesh, WiFiManager, WebServer, etc.

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