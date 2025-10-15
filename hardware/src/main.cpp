#include <Arduino.h>
#include <Wire.h>
#include <IRremote.hpp>
#include <ctype.h>
#include <string.h>

// FreeRTOS (ESP32)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "OledUI.hpp"

// ======================= Hardware =======================
#define IR_SEND_PIN   4
#define IR_RECV_PIN   15
#define UART          Serial
#define BAUD          115200
#define OLED_ADDR     0x3C

// =================== Limites de segurança =================
static const uint32_t MAX_XMIT_TIME_US   = 3000000UL;  // 3 s (AC pode ser longo)
static const uint16_t MAX_PATTERN_COUNT  = 1024;       // suporta padrões grandes

// =================== Estado / buffers ====================
static char asciiBuf[4096];                 // linha do terminal (grande)
static uint16_t asciiLen = 0;
static volatile uint16_t packetCount = 0;
static volatile uint32_t lastFreqHz = 38000;

// =================== Display & sync ======================
static OledUI oled;
static SemaphoreHandle_t dispMutex;
static String lastTxShort = "-";
static String lastRxShort = "-";

// ================ Helpers gerais =========================
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
  UART.println(F("  NEC <HEX8>"));
  UART.println(F("  TX  <freqHz> <us,us,...>"));
  UART.println(F("  TXM <freqHz>  (entao envie linhas com <us,us,...> e finalize com END)"));
  UART.println(F("  RAW <b b b>   (cada b vira 50us)"));
}

// Atualiza display com "TX: ..." e "RX: ..."
static void showStatus(const String& txLine, const String& rxLine) {
  if (!oled.isReady()) return;
  String l1 = "IR ASCII v1.1";
  String l2 = "TX: " + txLine;
  String l3 = "RX: " + rxLine;
  if (dispMutex && xSemaphoreTake(dispMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    oled.show3(l1, l2, l3, packetCount);
    xSemaphoreGive(dispMutex);
  }
}

// ================= Execução de comandos (TX) ==============
static void doNEC(const char* hex8) {
  if (!isHexStr(hex8, 8)) { UART.println(F("[ERR] use: NEC 20DF10EF")); return; }
  uint8_t raw[4];
  for (int i = 0; i < 4; i++) {
    char tmp[3] = { hex8[2*i], hex8[2*i+1], 0 };
    raw[i] = (uint8_t) strtoul(tmp, nullptr, 16);
  }
  uint32_t code = (uint32_t(raw[0])<<24)|(uint32_t(raw[1])<<16)|(uint32_t(raw[2])<<8)|raw[3];

  IrSender.sendNECMSB(code, 32);
  packetCount++;

  lastTxShort = String("NEC ") + hex8;
  UART.printf("[OK] NEC 0x%s\n", hex8);
  showStatus(lastTxShort, lastRxShort);
}

static bool sendRawUs(uint32_t freqHz, const uint16_t* raw, uint16_t count) {
  if (count == 0) { UART.println(F("[ERR] pattern vazio")); return false; }

  uint32_t totalUs = 0;
  for (uint16_t i = 0; i < count; ++i) totalUs += raw[i];
  if (totalUs > MAX_XMIT_TIME_US) {
    UART.printf("[ERR] pattern muito longo: %lu us > limite %lu us\n",
                (unsigned long)totalUs, (unsigned long)MAX_XMIT_TIME_US);
    return false;
  }

  uint8_t kHz = (uint8_t)((freqHz + 500) / 1000);
  if (kHz == 0) kHz = 1; if (kHz > 255) kHz = 255;

  IrSender.enableIROut(kHz);
  IrSender.sendRaw(raw, count, kHz);
  lastFreqHz = freqHz;
  packetCount++;

  char shortF[20]; snprintf(shortF, sizeof(shortF), "%luHz", (unsigned long)freqHz);
  char shortN[20]; snprintf(shortN, sizeof(shortN), "n=%u", count);
  lastTxShort = String(shortF) + " " + shortN;

  UART.printf("[OK] TX f=%lu Hz, n=%u\n", (unsigned long)freqHz, count);
  showStatus(lastTxShort, lastRxShort);
  return true;
}

static void doTX(char* freqStr, char* listStr) {
  if (!freqStr || !listStr) { UART.println(F("[ERR] use: TX <freqHz> <us,us,...>")); return; }
  uint32_t freqHz = strtoul(freqStr, nullptr, 10);
  if (freqHz == 0) { UART.println(F("[ERR] freqHz invalida")); return; }

  static uint16_t raw[MAX_PATTERN_COUNT];
  uint16_t count = 0;

  for (char* tok = strtok(listStr, ","); tok && count < MAX_PATTERN_COUNT; tok = strtok(nullptr, ",")) {
    while (*tok && isspace((unsigned char)*tok)) tok++;
    uint32_t us = strtoul(tok, nullptr, 10);
    if (us == 0) { UART.println(F("[ERR] duracao <= 0")); return; }
    raw[count++] = (uint16_t) us;
  }
  if (count >= MAX_PATTERN_COUNT && strtok(nullptr, ",")) {
    UART.println(F("[WARN] parte do pattern foi truncada (MAX_PATTERN_COUNT)"));
  }

  (void)sendRawUs(freqHz, raw, count);
}

static void doRAW(int argc, char** argv) {
  if (argc <= 1) { UART.println(F("[ERR] use: RAW <b b b>")); return; }
  static uint16_t raw[MAX_PATTERN_COUNT];
  uint16_t n = 0;

  for (int i = 1; i < argc && n < MAX_PATTERN_COUNT; i++) {
    uint32_t v = 0;
    if (strncasecmp(argv[i], "0x", 2) == 0) v = strtoul(argv[i] + 2, nullptr, 16);
    else v = strtoul(argv[i], nullptr, 0);
    if (v > 255) v = 255;
    raw[n++] = (uint16_t)(v * 50);
  }
  if (n == 0) { UART.println(F("[ERR] RAW vazio")); return; }

  uint8_t kHz = (uint8_t)((lastFreqHz + 500) / 1000);
  if (kHz == 0) kHz = 38;
  sendRawUs(kHz * 1000UL, raw, n);
}

// ========== TXM (multi-linha): estado de montagem =========
struct TXMState {
  bool assembling = false;
  uint32_t freqHz = 38000;
  uint16_t count = 0;
  uint16_t raw[MAX_PATTERN_COUNT];
} static txm;

static void txmReset() { txm.assembling = false; txm.freqHz = 38000; txm.count = 0; }

static void txmStart(uint32_t freqHz) {
  txmReset();
  txm.assembling = true;
  txm.freqHz = freqHz ? freqHz : 38000;
  UART.printf("[TXM] iniciada a %lu Hz. Envie linhas com numeros separados por virgula. Termine com END.\n",
              (unsigned long)txm.freqHz);
}

static void txmAddLine(char* line) {
  // aceita "123,456, 789 , 100" ...
  for (char* tok = strtok(line, ","); tok && txm.count < MAX_PATTERN_COUNT; tok = strtok(nullptr, ",")) {
    while (*tok && isspace((unsigned char)*tok)) tok++;
    if (!*tok) continue;
    uint32_t us = strtoul(tok, nullptr, 10);
    if (us == 0) { UART.println(F("[TXM] ignorado valor <= 0")); continue; }
    txm.raw[txm.count++] = (uint16_t)us;
  }
  if (txm.count >= MAX_PATTERN_COUNT) UART.println(F("[TXM] atingiu MAX_PATTERN_COUNT; truncado."));
}

static void txmFinish() {
  if (!txm.assembling) { UART.println(F("[TXM] nao estava ativo.")); return; }
  UART.printf("[TXM] finalizando. total=%u slices\n", txm.count);
  sendRawUs(txm.freqHz, txm.raw, txm.count);
  txmReset();
}

// ================= Parser ASCII ==========================
static void handleAsciiLine(char* line) {
  trim(line);
  if (!*line) return;

  // Se estamos em modo TXM, interpretamos primeiro
  if (txm.assembling) {
    if (strcasecmp(line, "END") == 0) { txmFinish(); return; }
    // linha de conteúdo
    txmAddLine(line);
    return;
  }

  // tokenização padrão
  char* argv[40] = {0};
  int argc = 0;
  for (char* p = strtok(line, " "); p && argc < 40; p = strtok(nullptr, " ")) argv[argc++] = p;
  if (argc == 0) return;

  if (strcasecmp(argv[0], "NEC") == 0) {
    if (argc < 2) { UART.println(F("[ERR] use: NEC <HEX8>")); return; }
    doNEC(argv[1]); return;
  }

  if (strcasecmp(argv[0], "TXM") == 0) {
    if (argc < 2) { UART.println(F("[ERR] use: TXM <freqHz>")); return; }
    uint32_t f = strtoul(argv[1], nullptr, 10);
    if (!f) { UART.println(F("[ERR] freqHz invalida")); return; }
    txmStart(f);
    return;
  }

  if (strcasecmp(argv[0], "TX") == 0 || strcasecmp(argv[0], "TRANSMIT") == 0) {
    if (argc < 3) { UART.println(F("[ERR] use: TX <freqHz> <us,us,...>")); return; }
    char* listStart = strstr(line, argv[2]); // linha já tokenizada
    doTX(argv[1], listStart); return;
  }

  if (strcasecmp(argv[0], "RAW") == 0) { doRAW(argc, argv); return; }
  if (strcasecmp(argv[0], "HELP") == 0 || strcasecmp(argv[0], "?") == 0) { help(); return; }

  UART.println(F("[ERR] comandos: NEC <hex8>, TX <freq> <us,...>, TXM <freq> ... END, RAW <b b b>, HELP"));
}

// ========= Conversão do decode p/ TXM =====================
#ifndef MICROS_PER_TICK
#define MICROS_PER_TICK 50  // IRremote usa 50us/tick no buffer cru
#endif

// Mapeia protocolo → frequência típica (fallback para 38 kHz).
static uint32_t defaultFreqForProtocol(decode_type_t p) {
  switch (p) {
    case RC5:
    case RC6:        return 36000;
    case SONY:       return 40000;
    case PANASONIC:
    case KASEIKYO:   return 37000;
    default:         return 38000;
  }
}

// Imprime como bloco TXM:
// TXM <freq>
// a,b,c,... (quebrando em linhas de ~80 colunas)
// END
static void printTXMFromLastDecode() {
  const IRData* d = &IrReceiver.decodedIRData;
  if (!d || !d->rawDataPtr || d->rawDataPtr->rawlen < 2) {
    UART.println(F("[WARN] decode vazio"));
    return;
  }

  uint32_t freq = defaultFreqForProtocol(d->protocol);
  if (!freq) freq = lastFreqHz;

  UART.printf("TXM %lu\n", (unsigned long)freq);

  // Monta e imprime em linhas
  // Índice 0 costuma ser lead; começamos no 1
  // Reservamos ~80 colunas por linha para legibilidade
  const uint_fast8_t rawlen = d->rawDataPtr->rawlen;
  char line[96];
  size_t pos = 0;

  for (uint_fast8_t i = 1; i < rawlen; i++) {
    uint32_t us = (uint32_t)d->rawDataPtr->rawbuf[i] * MICROS_PER_TICK;
    char num[16];
    snprintf(num, sizeof(num), "%lu", (unsigned long)us);

    size_t need = strlen(num) + 1; // com possível vírgula
    if (pos + need >= sizeof(line)) {
      // imprime linha acumulada (sem trailing vírgula)
      if (pos > 0 && line[pos-1] == ',') line[pos-1] = 0;
      UART.println(line);
      pos = 0;
    }

    // adiciona número + vírgula
    size_t nlen = strlen(num);
    memcpy(line + pos, num, nlen); pos += nlen;
    if (i + 1 < rawlen) { line[pos++] = ','; }
    line[pos] = 0;
  }

  if (pos > 0) {
    if (line[pos-1] == ',') line[pos-1] = 0;
    UART.println(line);
  }

  UART.println("END");
}

// ======================= Tasks (2 núcleos) =================
static void TaskSerialAndTX(void* pv) {
  for (;;) {
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
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static void TaskIRRecv(void* pv) {
  for (;;) {
    if (IrReceiver.decode()) {
      // imprime sempre em formato TXM
      printTXMFromLastDecode();

      // Atualiza RX curto para display (apenas cabeçalho)
      lastRxShort = "TXM recebido";
      showStatus(lastTxShort, lastRxShort);

      IrReceiver.resume(); // próximo pacote
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ======================= Setup / Loop =====================
void setup() {
  UART.begin(BAUD);

  if (!oled.begin(OLED_ADDR)) {
    UART.println(F("[WARN] SSD1306 nao inicializou. Seguindo sem display."));
  }
  dispMutex = xSemaphoreCreateMutex();
  showStatus(lastTxShort, lastRxShort);

  IrSender.begin(IR_SEND_PIN, ENABLE_LED_FEEDBACK, USE_DEFAULT_FEEDBACK_LED_PIN);
  IrReceiver.begin(IR_RECV_PIN, ENABLE_LED_FEEDBACK, USE_DEFAULT_FEEDBACK_LED_PIN);

  UART.println(F("[IR] pronto. Digite HELP."));
  UART.printf("[INFO] Sender pin=%d, Receiver pin=%d\n", IR_SEND_PIN, IR_RECV_PIN);

  xTaskCreatePinnedToCore(TaskSerialAndTX, "serial_tx", 4096, nullptr, 1, nullptr, 1); // core 1
  xTaskCreatePinnedToCore(TaskIRRecv,      "ir_recv",   4096, nullptr, 2, nullptr, 0); // core 0
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(100));
}
