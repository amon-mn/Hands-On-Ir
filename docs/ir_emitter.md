# 🧠 Documentação do Driver de Kernel: IR_EMITTER

**Módulo do Kernel:** `ir_emitter.ko`

---

## 1. Visão Geral e Propósito

Este driver atua como a ponte de comunicação de mais baixo nível entre o **Userspace (camada HAL)** e o **Hardware (ESP32)**.  
Ele é o coração da comunicação serial para transmissão IR.

**Objetivos principais:**

- Criar uma interface no **sysfs** (sistema de arquivos do Kernel) para que a HAL possa escrever comandos.  
- Traduzir a string de comando recebida para o formato de comunicação serial (**USB-Serial**) e enviá-la ao ESP32.  
- Monitorar a linha serial para receber a confirmação de sucesso (`[OK] TX...`) do firmware.

---

## 2. Interface de Comunicação (sysfs)

O driver cria um objeto de kernel na hierarquia do sistema de arquivos para receber comandos:

| Parâmetro | Caminho Completo (Write) | Propósito |
|------------|---------------------------|------------|
| `transmit` | `/sys/kernel/infrared/transmit` | Recebe a string de comando IR da HAL para transmissão. |
| `transmit` | `/sys/kernel/infrared/transmit` | Leitura: apenas para debug. Retorna o último comando salvo com sucesso. |

---

### 2.1. Contrato de Dados (HAL ↔ Kernel)

A HAL deve formatar os dados de pulso e frequência recebidos do Framework do Android em uma única string ASCII e escrevê-la no arquivo `transmit`.

**Formato de Entrada Esperado (Escrita no sysfs):**
```text
<frequenciaHz> <pulso1,pulso2,pulso3,...>\n
```

**REGRA DE PROTOCOLO:**  
O driver exige que o último caractere seja a **quebra de linha (`\n`)** para garantir o envio do pacote completo.

**Exemplo de String:**
```text
38000 9000,4500,560,560,560,560\n
```

---

### 2.2. Protocolo de Saída (Kernel ↔ Hardware)

Ao receber a string do sysfs, o driver a prefixa com o comando `TX` e a envia via **USB-Serial**.

**Formato Enviado para o ESP32 (Via USB-Serial):**
```text
TX <frequenciaHz> <pulso1,pulso2,pulso3,...>\n
```

---

## 3. Comandos e Respostas

A comunicação serial é **síncrona**: o driver envia o comando e aguarda a confirmação retornada pelo firmware.

| Fluxo | Mensagem Esperada pelo Kernel | Ação do Kernel |
|--------|-------------------------------|----------------|
| **SUCESSO** | `[OK] TX f=38000 Hz, n=6\n` | `attr_store` retorna o número de bytes processados (`count`) e salva o comando em cache. |
| **FALHA** | Nenhum dado, apenas Timeout | `attr_store` retorna erro de I/O `(-EIO)` após o limite de tentativas. (Ou o firmware responde com `[ERR]...`) |

---

## 4. Estrutura e Localização

O driver foi construído a partir do modelo **SmartLamp**, mantendo a robustez da comunicação **USB-Serial (CP2102)**.

**Localização:** `kernel/drivers/ir_emitter.c`

**Dependência:**  
- **Chip Serial USB:** CP2102  
- **Vendor ID:** `0x10C4`  
- **Product ID:** `0xEA60`

---

## 5. Tutorial de Teste Manual (Driver ↔ Hardware)

Este tutorial permite testar a funcionalidade de **Transmissão (TX)** do Kernel para o Hardware, validando o protocolo da **Sprint 1**.

### 5.1. Pré-requisitos

- Módulo `ir_emitter.ko` compilado e pronto.  
- ESP32 conectado e com o firmware **IR ASCII Console** carregado.  
- Permissões de **sudo** no terminal.  
- Terminal de monitoramento do Kernel (`dmesg -w` ou outro).

---

### 5.2. Etapas do Teste

```bash
# 1. Inserir o Módulo
sudo rmmod cp210x   # Remova o driver anterior
sudo insmod kernel/drivers/ir_emitter.ko   # Insira o novo módulo
# Verifique no dmesg: IR_EMITTER: Dispositivo conectado ...

# 2. Monitorar o Log do Kernel
sudo dmesg -w
# Esperado: O --- TESTE DIRETO DE USB (ATOMIC) INICIADO ---

# 3. Comando de Teste (com \n obrigatório)
echo -e "38000 9000,4500,560,560,560,560\n" | sudo tee /sys/kernel/infrared/transmit

# 4. Confirmação de Sucesso
cat /sys/kernel/infrared/transmit
# Esperado: Último TX enviado: 38000 9000,4500,560,560,560,560

# 5. Confirmação de Hardware
# Olhe o LED IR com a câmera do celular — deve piscar (handshake TX/RX completo)
```

---

### 5.3. Exemplo de Falha de Protocolo

Teste se a HAL **esquecer o `\n`** no final:

```bash
echo "38000 9000,4500,560,560,560,560" | sudo tee /sys/kernel/infrared/transmit
```

**Resultado Esperado:**

- **Terminal:**  
  `tee: /sys/kernel/infrared/transmit: Invalid argument`
- **dmesg:**  
  `IR_EMITTER: Erro de protocolo! A HAL DEVE encerrar o comando com '\n'.`

---

🧩 **Fim da Documentação**  
Versão: *Sprint 1 - IR Emitter Driver*
