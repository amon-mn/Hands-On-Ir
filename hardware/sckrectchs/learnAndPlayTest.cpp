// ESP32 + IRremoteESP8266 2.8.6
// Aprender e reproduzir qualquer comando de AR (RAW), sem saber a marca.

#include <Arduino.h>
#include <Preferences.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

#define RECV_PIN 15
#define SEND_PIN 4
#define BAUD     115200

// ==== captura robusta p/ AC ====
const uint16_t CAPTURE_BUF = 4000;   // frames longos
const uint8_t  RECV_TIMEOUT_MS = 120; // delimita fim do frame (~120 ms)
const uint16_t MIN_UNKNOWN = 4;       // ignora ruído curto

IRrecv  irrecv(RECV_PIN, CAPTURE_BUF, RECV_TIMEOUT_MS, true);
IRsend  irsend(SEND_PIN);
decode_results results;
Preferences prefs;

// parâmetros de reprodução (configuráveis via serial)
uint16_t carrierKHz = 38;   // 36, 38 ou 40
uint8_t  repeats    = 2;    // repetições além da 1ª (total = 1 + repeats)
uint16_t gapMs      = 90;   // intervalo entre repetições
bool     autoRepeat = false;
unsigned long lastAuto = 0;
const unsigned long autoEveryMs = 3000;

// buffer RAM para reprodução
static uint16_t pulses[5000];
uint16_t pulsesLen = 0;

// converte rawbuf (ticks de 50us) -> micros e guarda em pulses[]
uint16_t buildMicrosFromRawbuf(const decode_results &res, uint16_t *dst, uint16_t dst_max) {
  const uint16_t tick_us = kRawTick; // 50us
  uint16_t out = 0;
  // pular rawbuf[0] (gap inicial) e começar em rawbuf[1] (mark)
  for (uint16_t i = 1; i < res.rawlen && out < dst_max; i++) {
    uint32_t us = (uint32_t)res.rawbuf[i] * tick_us;
    if (us > 65535) us = 65535;
    dst[out++] = (uint16_t)us;
  }
  if (out & 1) out--; // garantir par
  return out;
}

// salva no NVS
void saveToNVS(const uint16_t *data, uint16_t len) {
  prefs.begin("irlearn", false);
  prefs.putUShort("len", len);
  prefs.putBytes("data", data, len * sizeof(uint16_t));
  prefs.end();
}

// carrega do NVS
bool loadFromNVS() {
  prefs.begin("irlearn", true);
  uint16_t len = prefs.getUShort("len", 0);
  if (len == 0 || len > (sizeof(pulses) / sizeof(pulses[0]))) {
    prefs.end();
    return false;
  }
  size_t got = prefs.getBytes("data", pulses, len * sizeof(uint16_t));
  prefs.end();
  if (got != len * sizeof(uint16_t)) return false;
  pulsesLen = len;
  return true;
}

void eraseNVS() {
  prefs.begin("irlearn", false);
  prefs.clear();
  prefs.end();
  pulsesLen = 0;
}

// reproduz o que está em pulses[]
void playStored() {
  if (pulsesLen < 2) {
    Serial.println(F("[ERRO] Nenhum comando salvo."));
    return;
  }
  for (uint8_t i = 0; i < 1 + repeats; i++) {
    irsend.sendRaw(pulses, pulsesLen, carrierKHz);
    Serial.print(F("[TX] len=")); Serial.print(pulsesLen);
    Serial.print(F(" @ ")); Serial.print(carrierKHz); Serial.println(F(" kHz"));
    if (i < repeats) delay(gapMs);
  }
}

void learnOnce() {
  Serial.println(F("LEARN: Aperte o botão UMA vez e solte..."));
  unsigned long start = millis();
  while (millis() - start < 5000) { // janela de 5s p/ receber
    if (irrecv.decode(&results)) {
      Serial.println(resultToHumanReadableBasic(&results));
      Serial.println(resultToSourceCode(&results)); // debug
      pulsesLen = buildMicrosFromRawbuf(results, pulses, (uint16_t)(sizeof(pulses)/sizeof(pulses[0])));
      irrecv.resume();
      if (pulsesLen >= 2) {
        saveToNVS(pulses, pulsesLen);
        Serial.print(F("[OK] Capturado e salvo. len=")); Serial.println(pulsesLen);
        return;
      } else {
        Serial.println(F("[ERRO] Captura muito curta. Tente de novo."));
        return;
      }
    }
    delay(1);
  }
  Serial.println(F("[ERRO] Timeout esperando sinal."));
}

void printHelp() {
  Serial.println(F("\nComandos:"));
  Serial.println(F("  LEARN          -> captura 1 frame e salva"));
  Serial.println(F("  PLAY           -> reproduz o salvo"));
  Serial.println(F("  AUTO ON|OFF    -> liga/desliga reprodução a cada 3 s"));
  Serial.println(F("  SETKHZ 36|38|40"));
  Serial.println(F("  SETREP N       -> repetições extra (0..5)"));
  Serial.println(F("  SETGAP ms      -> intervalo entre repetições (ex.: 90/120)"));
  Serial.println(F("  ERASE          -> apaga salvo\n"));
}

void setup() {
  Serial.begin(BAUD);
  delay(50);
  irrecv.setUnknownThreshold(MIN_UNKNOWN);
  irrecv.setTolerance(35);
  irrecv.enableIRIn();
  irsend.begin();

  Serial.println(F("IR Learn&Play (RAW) pronto."));
  if (loadFromNVS()) {
    Serial.print(F("[INFO] Comando salvo. len=")); Serial.println(pulsesLen);
  } else {
    Serial.println(F("[INFO] Sem comando salvo."));
  }
  printHelp();
}

void loop() {
  // AUTO repeat
  if (autoRepeat && millis() - lastAuto >= autoEveryMs) {
    lastAuto = millis();
    playStored();
  }

  // comandos pela serial (linha por linha)
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length()) {
        String s = line; line = ""; s.trim();
        if (s.equalsIgnoreCase("LEARN")) {
          learnOnce();
        } else if (s.equalsIgnoreCase("PLAY")) {
          playStored();
        } else if (s.equalsIgnoreCase("AUTO ON")) {
          autoRepeat = true; lastAuto = 0; Serial.println(F("[OK] AUTO ON (3 s)"));
        } else if (s.equalsIgnoreCase("AUTO OFF")) {
          autoRepeat = false; Serial.println(F("[OK] AUTO OFF"));
        } else if (s.startsWith("SETKHZ")) {
          int v = s.substring(6).toInt();
          if (v == 36 || v == 38 || v == 40) { carrierKHz = v; Serial.print(F("[OK] KHZ=")); Serial.println(v); }
          else Serial.println(F("[ERRO] Use 36, 38 ou 40"));
        } else if (s.startsWith("SETREP")) {
          int v = s.substring(6).toInt();
          if (v >= 0 && v <= 5) { repeats = v; Serial.print(F("[OK] REP=")); Serial.println(v); }
          else Serial.println(F("[ERRO] 0..5"));
        } else if (s.startsWith("SETGAP")) {
          int v = s.substring(6).toInt();
          if (v >= 20 && v <= 300) { gapMs = v; Serial.print(F("[OK] GAPms=")); Serial.println(v); }
          else Serial.println(F("[ERRO] 20..300 ms"));
        } else if (s.equalsIgnoreCase("ERASE")) {
          eraseNVS(); Serial.println(F("[OK] apagado."));
        } else if (s.equalsIgnoreCase("HELP")) {
          printHelp();
        } else {
          Serial.println(F("[?] Comando desconhecido. Digite HELP."));
        }
      }
    } else {
      line += c;
      if (line.length() > 200) line = "";
    }
  }
}
