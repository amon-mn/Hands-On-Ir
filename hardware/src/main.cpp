#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <IRremote.hpp>
#include <ctype.h>      // isspace, isxdigit
#include <string.h>     // strtok, strlen

// ====== Hardware & Display ======
#define IR_SEND_PIN     4
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   32
#define OLED_ADDR       0x3C

#define UART            Serial
#define BAUD            115200

// ====== Limites de segurança ======
static const uint32_t MAX_XMIT_TIME_US   = 2000000UL;  // 2 s
static const uint16_t MAX_PATTERN_COUNT  = 256;

// ====== Estado / buffers ======
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
static char asciiBuf[256];
static uint16_t asciiLen = 0;
static uint16_t packetCount = 0;
static uint32_t lastFreqHz = 38000;

// ====== Helpers ======
static inline void show3(const String& l1, const String& l2 = "", const String& l3 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);  display.println(l1);   // 0..31 é o range válido
  display.setCursor(0, 12); display.println(l2);   // 12 px
  display.setCursor(0, 24); display.println(l3);   // 24 px (última linha)
  display.setCursor(100, 24);                      // cabe no 128×32
  display.print("#"); display.print(packetCount);
  display.display();
}

static inline void trim(char* s) {
  int n = strlen(s);
  while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' || isspace((unsigned char)s[n-1]))) s[--n] = 0;
  int i = 0; while (isspace((unsigned char)s[i])) i++;
  if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
}

static inline bool isHexStr(const char* s, int expectLen) {
  if ((int)strlen(s) != expectLen) return false;
  for (const char* p = s; *p; ++p) if (!isxdigit((unsigned char)*p)) return false;
  return true;
}

static void help() {
  UART.println(F("IR ASCII cmds:"));
  UART.println(F("  NEC <HEX8>                  e.g. NEC 20DF10EF"));
  UART.println(F("  TX <freqHz> <us,...>        e.g. TX 38000 9000,4500,560,560,560,560"));
  UART.println(F("  RAW <b b b>                 e.g. RAW 10 20 30 40  (each * 50us)"));
}

// ====== Execução dos comandos ======
static void doNEC(const char* hex8) {
  if (!isHexStr(hex8, 8)) { UART.println(F("[ERR] use: NEC 20DF10EF")); return; }
  uint8_t raw[4];
  for (int i = 0; i < 4; i++) {
    char tmp[3] = { hex8[2*i], hex8[2*i+1], 0 };
    raw[i] = (uint8_t) strtoul(tmp, nullptr, 16);
  }
  uint32_t code = (uint32_t(raw[0])<<24)|(uint32_t(raw[1])<<16)|(uint32_t(raw[2])<<8)|raw[3];
  IrSender.sendNECMSB(code, 32);   // API nova (MSB)
  packetCount++;
  show3("NEC", String(hex8), "enviado");
  UART.printf("[OK] NEC 0x%s\n", hex8);
}

static void doTX(char* freqStr, char* listStr) {
  if (!freqStr || !listStr) { UART.println(F("[ERR] use: TX <freqHz> <us,us,...>")); return; }
  uint32_t freqHz = strtoul(freqStr, nullptr, 10);
  if (freqHz == 0) { UART.println(F("[ERR] freqHz invalida")); return; }

  static uint16_t raw[MAX_PATTERN_COUNT];
  uint16_t count = 0;
  uint32_t totalUs = 0;

  // Parse "9000,4500,560,560,..."
  for (char* tok = strtok(listStr, ","); tok && count < MAX_PATTERN_COUNT; tok = strtok(nullptr, ",")) {
    while (*tok && isspace((unsigned char)*tok)) tok++;
    uint32_t us = strtoul(tok, nullptr, 10);
    if (us == 0) { UART.println(F("[ERR] duracao <= 0")); return; }
    raw[count++] = (uint16_t) us;
    totalUs += us;
  }
  if (count == 0) { UART.println(F("[ERR] pattern vazio")); return; }
  if (totalUs > MAX_XMIT_TIME_US) { UART.println(F("[ERR] pattern muito longo")); return; }

  uint8_t kHz = (uint8_t)((freqHz + 500) / 1000);
  if (kHz == 0) kHz = 1; if (kHz > 255) kHz = 255;

  IrSender.enableIROut(kHz);
  IrSender.sendRaw(raw, count, kHz);

  lastFreqHz = freqHz;
  packetCount++;

  char fbuf[28]; snprintf(fbuf, sizeof(fbuf), "f=%lu Hz", (unsigned long)freqHz);
  char cbuf[28]; snprintf(cbuf, sizeof(cbuf), "n=%u slices", count);
  show3("TRANSMIT", fbuf, cbuf);
  UART.printf("[OK] TX f=%lu Hz, n=%u\n", (unsigned long)freqHz, count);
}

static void doRAW(int argc, char** argv) {
  // RAW 10 20 30 40  (cada valor vira 50us)
  if (argc <= 1) { UART.println(F("[ERR] use: RAW <b b b>")); return; }
  static uint16_t raw[MAX_PATTERN_COUNT];
  uint16_t n = 0; uint32_t totalUs = 0;

  for (int i = 1; i < argc && n < MAX_PATTERN_COUNT; i++) {
    // aceita decimal ou hex (0x.. ou sem 0x)
    uint32_t v = 0;
    if (strncasecmp(argv[i], "0x", 2) == 0) v = strtoul(argv[i] + 2, nullptr, 16);
    else v = strtoul(argv[i], nullptr, 0);
    if (v > 255) v = 255;
    raw[n++] = (uint16_t)(v * 50);
    totalUs += raw[n-1];
  }

  if (n == 0) { UART.println(F("[ERR] RAW vazio")); return; }
  if (totalUs > MAX_XMIT_TIME_US) { UART.println(F("[ERR] pattern muito longo")); return; }

  uint8_t kHz = (uint8_t)((lastFreqHz + 500) / 1000);
  if (kHz == 0) kHz = 38;
  IrSender.sendRaw(raw, n, kHz);

  packetCount++;
  show3("RAW(antigo)", String("n=") + n, "enviado");
  UART.printf("[OK] RAW n=%u\n", n);
}

// ====== Parser de linha ASCII ======
static void handleAsciiLine(char* line) {
  trim(line);
  if (!*line) return;

  // tokenização simples
  char* argv[40] = {0};
  int argc = 0;
  for (char* p = strtok(line, " "); p && argc < 40; p = strtok(nullptr, " ")) {
    argv[argc++] = p;
  }
  if (argc == 0) return;

  if (strcasecmp(argv[0], "NEC") == 0) {
    if (argc < 2) { UART.println(F("[ERR] use: NEC <HEX8>")); return; }
    doNEC(argv[1]);
    return;
  }

  if (strcasecmp(argv[0], "TX") == 0 || strcasecmp(argv[0], "TRANSMIT") == 0) {
    if (argc < 3) { UART.println(F("[ERR] use: TX <freqHz> <us,us,...>")); return; }
    // argv[2] pode conter vírgulas; reconstitui o resto da linha original após o freq
    char* listStart = strstr(line, argv[2]); // já está tokenizado; “line” foi modificado, mas argv[2] persiste
    doTX(argv[1], listStart);
    return;
  }

  if (strcasecmp(argv[0], "RAW") == 0) {
    doRAW(argc, argv);
    return;
  }

  if (strcasecmp(argv[0], "HELP") == 0 || strcasecmp(argv[0], "?") == 0) {
    help();
    return;
  }

  UART.println(F("[ERR] comandos: NEC <hex8>, TX <freq> <us,...>, RAW <b b b>, HELP"));
}

// ====== Setup/Loop ======
void setup() {
  UART.begin(BAUD);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    UART.println(F("[WARN] SSD1306 nao inicializou. Seguindo sem display."));
  } else {
    show3("IR ASCII v1.0", "Aguardando cmd", "");
  }
  IrSender.begin(IR_SEND_PIN, ENABLE_LED_FEEDBACK, USE_DEFAULT_FEEDBACK_LED_PIN);
  UART.println(F("[IR] pronto. Digite HELP."));
}

void loop() {
  while (UART.available()) {
    int c = UART.read();
    if (c == '\r') continue;
    if (c == '\n') {
      asciiBuf[(asciiLen < sizeof(asciiBuf)-1) ? asciiLen : sizeof(asciiBuf)-1] = 0;
      handleAsciiLine(asciiBuf);
      asciiLen = 0;
    } else if (asciiLen < sizeof(asciiBuf) - 1) {
      asciiBuf[asciiLen++] = (char)c;
    }
  }
}
