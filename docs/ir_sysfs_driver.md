# Documentação do Driver de Kernel: **IR_EMITTER**

**Módulo do Kernel:** `ir_sysfs_driver.ko`  

---

## 1. Visão Geral e Propósito

Este driver atua como a ponte de comunicação de mais baixo nível entre o **Userspace (camada HAL)** e o **Hardware (ESP32)**.

### Objetivos principais:
- Criar uma **interface no sysfs** (sistema de arquivos do Kernel) para que a HAL possa escrever comandos.  
- Traduzir a **string de comando** recebida para o formato de comunicação serial (**USB-Serial**) e enviá-la ao **ESP32**.  
- Monitorar a linha serial para receber a **confirmação de sucesso** (`[OK] TX...`) do firmware.

---

## 2. Interface de Comunicação (sysfs)

O driver cria um **objeto de kernel** na hierarquia do sistema de arquivos para receber comandos:

| Parâmetro  | Caminho Completo (Write)  | Propósito  |
|------------|---------------------------|------------|
| `transmit` | `/sys/kernel/infrared/transmit` | Recebe a string de comando IR da HAL para transmissão. |
| `transmit` | `/sys/kernel/infrared/transmit` | **Leitura:** apenas para debug. Retorna uma mensagem de status. |

---

### 2.1. Contrato de Dados (**HAL ↔ Kernel**)

A HAL deve formatar os dados de **pulso e frequência** recebidos do Framework do Android em uma **única string ASCII** e escrevê-la no arquivo `transmit`.

#### Formato de Entrada Esperado (Escrita no sysfs)
```text
<frequenciaHz> <pulso1,pulso2,pulso3,...>
```

#### Exemplo de String:
```text
38000 9000,4500,560,560,560,560
```

---

### 2.2. Protocolo de Saída (**Kernel ↔ Hardware**)

Ao receber a string do sysfs, o driver a **prefixa** com o comando `TX` e adiciona uma **nova linha (`\n`)**, enviando-a ao **ESP32**.

#### Formato Enviado para o ESP32 (Via USB-Serial)
```text
TX <frequenciaHz> <pulso1,pulso2,pulso3,...>\n
```

---

## 3. Comandos e Respostas

A comunicação serial é **síncrona**: o driver envia o comando e **aguarda o status** retornado pelo firmware.

| Fluxo | Mensagem Esperada pelo Kernel | Ação do Kernel |
|-------|-------------------------------|----------------|
| **SUCESSO** | `[OK] TX f=38000 Hz, n=6\n` | O `attr_store` retorna o número de bytes processados (`count`). |
| **FALHA** | `[ERR] pattern muito longo\n` | O `attr_store` retorna um erro de I/O (`-EIO`) para a HAL. |

---

## 4. Estrutura e Localização

O driver foi construído a partir do modelo **SmartLamp**, mantendo a robustez da comunicação **USB-Serial (CP2102)**.

- **Arquivo Source:** `kernel/drivers/ir_sysfs_driver.c`  
- **Dependência:** Chip Serial USB **CP2102**  
  - Vendor ID: `0x10C4`  
  - Product ID: `0xEA60`
