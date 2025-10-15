#include "OledUI.hpp"
#include <Wire.h>

OledUI::OledUI()
: _display(SCREEN_W, SCREEN_H, &Wire, -1) {}

bool OledUI::begin(uint8_t i2c_addr) {
  _ready = _display.begin(SSD1306_SWITCHCAPVCC, i2c_addr);
  if (_ready) {
    _display.clearDisplay();
    _display.setTextSize(1);
    _display.setTextColor(SSD1306_WHITE);
    _display.display();
  }
  return _ready;
}

void OledUI::show3(const String& l1, const String& l2, const String& l3, uint16_t packetCount) {
  if (!_ready) return;

  _display.clearDisplay();
  _display.setTextSize(1);
  _display.setTextColor(SSD1306_WHITE);

  // Linhas (0, 12, 24) como no seu layout
  _display.setCursor(0, 0);  _display.println(l1);
  _display.setCursor(0, 12); _display.println(l2);
  _display.setCursor(0, 24); _display.println(l3);

  // Contador no canto inferior direito (pos fixo 100,24 como no original)
  _display.setCursor(100, 24);
  _display.print("#");
  _display.print(packetCount);

  _display.display();
}
