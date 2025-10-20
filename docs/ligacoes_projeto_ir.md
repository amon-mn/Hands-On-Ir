# Ligações — Projeto IR v1.0

> Guia de conexões usadas pelo projeto IR (OLED I2C + emissor IR com NPN e fonte 5 V externa).

## Visão rápida
- **MCU:** ESP32 DevKit (UART a 115200 bps)
- **Display:** OLED SSD1306 128×32 **I2C** (`0x3C`)
- **IR TX:** LED IR guiado por **2N2222 (NPN)**
- **Alimentação:** ESP32 via USB; **LED IR** alimentado por **5 V externa** (GND comum).

## Conexões

### 1) OLED SSD1306 (I2C)
- **VCC** → **3V3** do ESP32  
- **GND** → **GND** do ESP32  
- **SCL** → **GPIO22** (I2C SCL padrão)  
- **SDA** → **GPIO21** (I2C SDA padrão)  
> O construtor usa `-1` para RST → **sem pino de reset dedicado**.

### 2) Emissor IR com NPN (2N2222) e fonte 5 V externa
- **Fonte 5 V externa (+5V)** → **Resistor de LED** (≈ **100–150 Ω**\*) → **Ânodo do LED IR**  
- **Cátodo do LED IR** → **Coletor do 2N2222**  
- **Emissor do 2N2222** → **GND comum**  
- **GPIO4** (IR_SEND_PIN do código) → **Resistor de base 330–560 Ω** → **Base do 2N2222**  
- *(Opcional)* **Pull‑down** base **100 kΩ** → **GND** (evita disparo flutuante)

**Atenção ao GND comum:** una os **GNDs** do **ESP32**, da **fonte 5 V** e do circuito do **NPN**.

\* *Resistor do LED:* ajuste conforme o LED IR (corrente típica 20–100 mA). Para correntes mais altas, considere MOSFET lógico (ex.: AO3400) ou transistor com dissipação adequada.

### 3) Alimentação e isolamento
- **ESP32**: alimente via **USB** do PC.  
- **LED IR**: consome da **fonte 5 V externa**.  
- **Não** conecte o **+5 V externo** ao **5V do ESP32**; compartilhe **apenas o GND**.

## Notas úteis
- A biblioteca define portadora típica em **38 kHz** (ajustada pelo código com `enableIROut/sendRaw`).  
- Teste **polaridade do LED IR** (anodo no +5V através do resistor). A câmera do celular geralmente enxerga o IR.  
- Cabos curtos e bons **GNDs** reduzem ruído no I2C e no IR TX.
