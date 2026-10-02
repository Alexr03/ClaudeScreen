// LovyanGFX setup for the ESP32-2432S028 ("Cheap Yellow Display").
//
//   LCD   : HSPI  SCLK 14  MOSI 13  MISO 12  CS 15  DC 2  BL 21
//   Touch : VSPI  SCLK 25  MOSI 32  MISO 39  CS 33  IRQ 36  (XPT2046)
//   RGB   : R 4  G 16  B 17  (active low)
//
// Two panel controllers ship on boards sold under this name: ILI9341 on the
// original, ST7789 on the "CYD2USB" revision. Both are configured here and
// main.cpp picks one at boot (detected, or overridden over serial).
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

static constexpr int PIN_LED_R = 4;
static constexpr int PIN_LED_G = 16;
static constexpr int PIN_LED_B = 17;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _ili;
  lgfx::Panel_ST7789 _st;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_XPT2046 _touch;

  void configPanel(lgfx::Panel_Device& p, bool invert, bool bgr) {
    auto cfg = p.config();
    cfg.pin_cs = 15;
    cfg.pin_rst = -1;
    cfg.pin_busy = -1;
    cfg.panel_width = 240;
    cfg.panel_height = 320;
    cfg.offset_x = 0;
    cfg.offset_y = 0;
    cfg.offset_rotation = 0;
    cfg.dummy_read_pixel = 8;
    cfg.dummy_read_bits = 1;
    cfg.readable = true;
    cfg.invert = invert;
    cfg.rgb_order = bgr;
    cfg.dlen_16bit = false;
    cfg.bus_shared = false;
    p.config(cfg);
    p.setBus(&_bus);
    p.setLight(&_light);
    p.setTouch(&_touch);
  }

 public:
  bool isST7789 = false;

  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = HSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc = 2;
      _bus.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 21;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
    }
    {
      auto cfg = _touch.config();
      cfg.x_min = 300;
      cfg.x_max = 3900;
      cfg.y_min = 200;
      cfg.y_max = 3800;
      cfg.pin_int = 36;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.spi_host = VSPI_HOST;
      cfg.freq = 1000000;
      cfg.pin_sclk = 25;
      cfg.pin_mosi = 32;
      cfg.pin_miso = 39;
      cfg.pin_cs = 33;
      _touch.config(cfg);
    }
    select(false, false, false);
  }

  // Must be called before init().
  void select(bool st7789, bool invert, bool bgr) {
    isST7789 = st7789;
    if (st7789) {
      configPanel(_st, invert, bgr);
      setPanel(&_st);
    } else {
      configPanel(_ili, invert, bgr);
      setPanel(&_ili);
    }
  }
};
