#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

class OledUI {
public:
  // Construtor: fixa 128x32 (igual ao seu hardware)
  OledUI();
  
  // Inicializa. Retorna true se ok. Endereço padrão 0x3C.
  bool begin(uint8_t i2c_addr = 0x3C);
  
  // Mostra três linhas + contador (canto inferior direito).
  void show3(
    const String& l1,
    const String& l2 = "",
    const String& l3 = "",
    uint16_t packetCount = 0
  );
  
  bool isReady() const { return _ready; }

private:
  static constexpr int SCREEN_W = 128;
  static constexpr int SCREEN_H = 32;

  Adafruit_SSD1306 _display;
  bool _ready = false;
};
