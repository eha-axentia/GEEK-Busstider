#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "wifi_credentials.h"

// Waveshare ESP32-C6-GEEK: ST7789 240x135, SPI
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789  _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.spi_host   = SPI2_HOST;
            cfg.spi_mode   = 3;
            cfg.freq_write = 40000000;
            cfg.freq_read  = 16000000;
            cfg.pin_sclk   = 1;
            cfg.pin_mosi   = 2;
            cfg.pin_miso   = -1;
            cfg.pin_dc     = 3;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs      = 5;
            cfg.pin_rst     = 4;
            cfg.pin_busy    = -1;
            cfg.memory_width  = 240;
            cfg.memory_height = 320;
            cfg.panel_width   = 135;
            cfg.panel_height  = 240;
            cfg.offset_x      = 52;
            cfg.offset_y      = 40;
            cfg.offset_rotation = 0;
            cfg.invert      = true;
            cfg.rgb_order   = false;
            cfg.dlen_16bit  = false;
            cfg.bus_shared  = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl      = 6;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 0;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};

static LGFX        lcd;
static WebServer   server(80);
static Preferences prefs;
static bool        serverActive = false;

static const char AP_SSID[] = "ESP32-C6-GEEK";

void startWebServer();

void handleRoot() {
    int n = WiFi.scanNetworks();

    String html =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>WiFi Setup</title><style>"
        "body{font-family:sans-serif;max-width:380px;margin:40px auto;padding:0 16px}"
        "h2{margin-bottom:24px}"
        "label{display:block;margin-bottom:4px;font-size:14px;color:#555}"
        "input{display:block;width:100%;padding:8px;margin-bottom:16px;"
        "border:1px solid #ccc;border-radius:4px;box-sizing:border-box;font-size:16px}"
        "button{width:100%;padding:10px;background:#0070f3;color:#fff;"
        "border:none;border-radius:4px;font-size:16px;cursor:pointer}"
        "</style></head><body><h2>WiFi Setup</h2>"
        "<form method=\"POST\" action=\"/save\">"
        "<label>SSID</label>"
        "<input type=\"text\" name=\"ssid\" list=\"networks\" autocomplete=\"off\" required>"
        "<datalist id=\"networks\">";

    for (int i = 0; i < n; i++) {
        html += "<option value=\"" + WiFi.SSID(i) + "\">";
    }
    WiFi.scanDelete();

    html +=
        "</datalist>"
        "<label>Password</label>"
        "<input type=\"password\" name=\"password\" autocomplete=\"off\">"
        "<button type=\"submit\">Save &amp; Connect</button>"
        "</form></body></html>";

    server.send(200, "text/html", html);
}

void handleSave() {
    if (!server.hasArg("ssid") || server.arg("ssid").isEmpty()) {
        server.send(400, "text/plain", "SSID is required.");
        return;
    }

    prefs.begin("wifi", false);
    prefs.putString("ssid",     server.arg("ssid"));
    prefs.putString("password", server.arg("password"));
    prefs.end();

    server.send(200, "text/html",
        "<!DOCTYPE html><html><body>"
        "<p>Saved! Restarting in 2 seconds...</p>"
        "</body></html>");
    delay(2000);
    ESP.restart();
}

bool connectWifi() {
    prefs.begin("wifi", true);
    String ssid     = prefs.getString("ssid",     WIFI_SSID);
    String password = prefs.getString("password", WIFI_PASSWORD);
    prefs.end();

    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.setTextSize(1);
    lcd.setCursor(10, 55);
    lcd.print("WiFi: connecting...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    Serial.print("Connecting to WiFi");

    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        lcd.setCursor(10, 55);
        lcd.setTextColor(TFT_GREEN, TFT_BLACK);
        lcd.printf("WiFi: %-20s", ssid.c_str());
        lcd.setCursor(10, 68);
        lcd.print("IP: ");
        lcd.print(WiFi.localIP().toString().c_str());
        Serial.println("Connected! IP: " + WiFi.localIP().toString());
        startWebServer();
        return true;
    }

    lcd.setCursor(10, 55);
    lcd.setTextColor(TFT_RED, TFT_BLACK);
    lcd.print("WiFi: FAILED        ");
    Serial.println("WiFi connection failed!");
    return false;
}

void startWebServer() {
    server.on("/",     HTTP_GET,  handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.begin();
    serverActive = true;
}

void startConfigAP() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID);

    lcd.setTextColor(TFT_ORANGE, TFT_BLACK);
    lcd.setTextSize(1);
    lcd.setCursor(10, 55);
    lcd.printf("AP: %-21s", AP_SSID);
    lcd.setCursor(10, 68);
    lcd.print("-> 192.168.4.1      ");

    Serial.printf("AP started: %s\n", AP_SSID);
    Serial.println("Open http://192.168.4.1 to configure WiFi.");

    startWebServer();
}

void setup() {
    Serial.begin(115200);

    lcd.init();
    lcd.setBrightness(255);
    lcd.setRotation(1);      // landscape, USB-C at left
    lcd.fillScreen(TFT_BLACK);

    lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    lcd.setTextSize(2);
    lcd.setCursor(10, 5);
    lcd.println("ESP32-C6-GEEK");

    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setTextSize(1);
    lcd.setCursor(10, 35);
    lcd.println("PlatformIO ready");

    Serial.println("Display initialised.");

    if (!connectWifi()) {
        startConfigAP();
    }
}

void loop() {
    if (serverActive) {
        server.handleClient();
    }
}
