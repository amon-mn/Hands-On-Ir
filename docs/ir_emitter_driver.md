# Driver de Kernel `ir_emitter` (DevTITANS)

Este é um driver de kernel Linux (`ir_emitter.c`) para o dispositivo **Emissor Infravermelho DevTITANS**.  
O driver se registra para o dispositivo USB (**Vendor ID:** `0x10C4`, **Product ID:** `0xEA60`) e atua como a ponte entre o sistema operacional (especificamente o **AOSP**) e o firmware do dispositivo.

O driver expõe uma interface de controle principal via **sysfs** no caminho:  
`/sys/kernel/infrared/transmit`

---

## 🧩 Alinhamento com AOSP (ConsumerIrManager)

O driver atua como a camada de abstração de hardware (**HAL**) entre a **API ConsumerIrManager** do Android e o firmware do ESP32.  
A interface sysfs é projetada para espelhar exatamente a API do Android:

```java
// API do Android
transmit(int carrierFrequencyHz, int[] patternMicros);
```
> Frequência em Hz e padrão em microssegundos, começando em **ON** e alternando **ON/OFF**.

O driver mapeia as escritas do sysfs para os comandos do firmware que usam as mesmas unidades (**Hz** e **µs**) e mesma semântica **ON/OFF**.

`NEC <HEX8>` é um atalho de alto nível que o driver repassa ao firmware.

---

## 🔧 Protocolo de Comandos (sysfs)

A comunicação com o driver é feita escrevendo **strings de comando** no nó sysfs.  
O driver então **valida**, **formata** (se necessário) e envia o comando ao firmware via **USB**.

### 🟢 NEC `<HEX8>`
Envia um frame NEC (32 bits, MSB-first).

**Exemplo de Escrita:**
```bash
NEC 10C8E11E
```

### 🟡 Padrão Bruto (Numérico) `<freqHz> <us,us,...>`
Transmite um padrão bruto na portadora informada.  
O driver espera uma string que comece com a frequência (um número).  
Ele automaticamente prefixará `TX` ao comando antes de enviá-lo ao firmware.

**Exemplo de Escrita:**
```bash
38000 9000,4500,560,560,560,560
```

### 🔵 TX `<freqHz> <us,us,...>`
Alternativamente, a HAL pode enviar o comando `TX` completo.  
O driver também o aceitará.

**Exemplo de Escrita:**
```bash
TX 38000 9000,4500,560,560,560,560
```

---

## 🧪 Tutorial de Teste via sysfs

Para testar o driver e o firmware manualmente a partir do terminal, você pode usar **echo** e **tee** para escrever no nó sysfs.

### 1️⃣ Teste de Envio (NEC)
Este comando envia um código NEC. O driver espera uma resposta `[OK] NEC` do firmware.

```bash
echo "NEC 10C8E11E" | sudo tee /sys/kernel/infrared/transmit
```

### 2️⃣ Teste de Envio (Padrão Bruto/TX)
Este comando envia um padrão bruto de 38kHz.  
O driver adicionará o prefixo `TX` e esperará uma resposta `[OK] TX` do firmware.

```bash
echo "38000 9000,4500,560,1690,560" | sudo tee /sys/kernel/infrared/transmit
```

### 3️⃣ Teste de Leitura (Verificar Último Comando)
Você pode ler o nó sysfs para ver o último comando que foi enviado com sucesso (conforme armazenado na variável `last_ir_command` do driver).

```bash
cat /sys/kernel/infrared/transmit
```
**Saída Esperada (após o Teste 1):**
```
Último TX enviado: NEC 10C8E11E
```

---

## ⚙️ Validações e Implementação do Driver

### `usb_probe`
- Identifica o dispositivo (`10C4:0xEA60`)
- Chama `ir_config_serial` para configurar o baud rate (115200)
- Cria o grupo sysfs (`/sys/kernel/infrared/transmit`)

### `attr_store` (Escrita no sysfs)
- **Validação de Nova Linha:** o comando escrito pela HAL **deve** terminar com `\n`. O driver o remove antes de processar.  
- **Validação de Protocolo:** verifica se o comando é `NEC <HEX8>` (com 8 dígitos) ou se é um padrão numérico (começa com 0–9).  
- **Retorno:** retorna o número de bytes escritos em sucesso, ou `-EINVAL` / `-EIO` em caso de falha.

### `attr_show` (Leitura do sysfs)
- Retorna o conteúdo da variável `last_ir_command`, que armazena o último comando (sem o prefixo `TX`) que recebeu um `[OK]` do firmware.

### `usb_send_cmd_ir`
- Adiciona o prefixo de protocolo (`TX` ou `NEC`) e o terminador `\n`  
- Envia os dados via `usb_bulk_msg`  
- Aguarda (com polling e timeout) por uma resposta `[OK]` ou `[ERR]` do firmware

### `usb_disconnect`
- Libera os buffers (`kfree`)  
- Remove o nó sysfs (`kobject_put`)

---

🧠 **Autor:** Equipe DevTITANS  
📂 **Arquivo:** `ir_emitter.c`  
🧰 **Camada:** Kernel / HAL / USB Communication
