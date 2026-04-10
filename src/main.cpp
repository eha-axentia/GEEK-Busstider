#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

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

static LGFX lcd;

void setup() {
    Serial.begin(115200);

    lcd.init();
    lcd.setBrightness(255);
    lcd.setRotation(1);      // landscape, USB-C at left
    lcd.fillScreen(TFT_BLACK);

    lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    lcd.setTextSize(2);
    lcd.setCursor(10, 50);
    lcd.println("ESP32-C6-GEEK");

    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setTextSize(1);
    lcd.setCursor(10, 80);
    lcd.println("PlatformIO ready");

    Serial.println("Display initialised.");
}

void loop() {
}
