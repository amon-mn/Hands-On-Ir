# IR ASCII Console (ESP32) — Compatível com o modelo do `ConsumerIrManager` (AOSP)

Este firmware implementa um **console ASCII via UART (115200)** para transmitir IR com **ESP32 + IRremote**, exibindo status em **OLED SSD1306 128×32**. O design dos comandos foi pensado para alinhar **1:1** com o contrato da API Android **`ConsumerIrManager.transmit(int carrierFrequency, int[] patternMicros)`**.

---

## Visão geral

- **UART**: `115200-8N1` (digite comandos no monitor serial).
- **Pino IR TX**: `GPIO 4` (`IR_SEND_PIN`).
- **Display**: SSD1306 (`0x3C`) com `show3()` para título/subtítulo/rodapé + contador de pacotes.
- **Limites de segurança**: máx. **256 fatias** e **2 s** (somatório de µs).

---

## Alinhamento com AOSP (`ConsumerIrManager`)

No Android, transmite-se com:
```java
transmit(int carrierFrequencyHz, int[] patternMicros);
```
- **Frequência em Hz** e **padrão em microssegundos**, começando em **ON** e alternando **ON/OFF**.
- Este firmware espelha exatamente isso:
  - `TX <freqHz> <us,us,...>` ↔ `transmit(freqHz, new int[]{...})`
  - **Mesmas unidades** (Hz e µs) e **mesma semântica ON/OFF**.
- `NEC <HEX8>` é um **atalho de alto nível** (gera o trem NEC internamente), tal como pré-sintetizar o `int[]` e chamar `transmit()`.
- `RAW <b ...>` compacta o padrão em **bytes × 50 µs**, continuando compatível conceitualmente (ainda um `int[]` em µs, usando a última frequência).

---

## Protocolo de comandos (ASCII)

### `NEC <HEX8>`
Envia um frame **NEC** (32 bits, MSB-first).
- Ex.: `NEC 20DF10EF`

### `TX <freqHz> <us,us,...>`
Transmite padrão bruto na portadora informada.  
Unidades **idênticas** ao Android (**Hz** e **µs**, começando em ON).
- Ex.: `TX 38000 9000,4500,560,560,560,560`

### `RAW <b b b ...>`
Cada byte vira **`byte * 50 µs`**; usa `lastFreqHz` como portadora.
- Aceita decimal/hex (`10 20 0x1E ...`)
- Ex.: `RAW 10 20 30` → `[500, 1000, 1500] µs` (em `lastFreqHz`)

### `HELP`
Mostra ajuda dos comandos.

---

## Validações e segurança

- **Comprimento total** do padrão limitado a **2 s**.
- **Número de fatias** limitado a **256**.
- Rejeita duração **≤ 0** e strings malformadas.
- OLED exibe **título, parâmetros e `#packetCount`** a cada envio.

---

## Exemplos de mapeamento Android ⇄ Firmware

- **Android → Firmware**  
  `transmit(38000, new int[]{9000,4500,560,560,560,560})`  
  ⇨ `TX 38000 9000,4500,560,560,560,560`

- **Firmware (RAW) → Android**  
  `RAW 10 20 30` ⇨ padrão `[500,1000,1500] µs` em `lastFreqHz`  
  ⇨ `transmit(lastFreqHz, new int[]{500,1000,1500})`

---

## Boot & uso

1. Conecte ao monitor serial (115200) e aguarde:  
   - `"[IR] pronto. Digite HELP."`
2. Opcional: confirme OLED com splash `IR ASCII v1.0`.
3. Envie um dos comandos (`HELP`, `NEC ...`, `TX ...`, `RAW ...`).

---

## Notas de implementação

- `IrSender.begin(IR_SEND_PIN, ...)` inicializa a camada de envio IR.  
- `sendNECMSB(code, 32)` para NEC; `sendRaw(raw, n, kHz)` para padrões brutos.  
- `lastFreqHz` é atualizado no `TX` e reutilizado pelo `RAW`.

--- 

**Licença / créditos**: Utilize e adapte conforme necessário.
