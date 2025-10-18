#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/err.h>
#include <linux/minmax.h>

// =========================================================
// DEFINIÇÕES E VARIÁVEIS GLOBAIS
// =========================================================

MODULE_AUTHOR("Equipe 04 - DevTITANS");
MODULE_DESCRIPTION("Driver de acesso ao Emissor Infravermelho");
MODULE_LICENSE("GPL");

#define MAX_RECV_LINE 512
#define VENDOR_ID  0x10C4  /* VendorID do CP2102 */
#define PRODUCT_ID 0xEA60  /* ProductID do CP2102 */

// Protótipos das funções do driver
static int  usb_probe(struct usb_interface *ifce, const struct usb_device_id *id);
static void usb_disconnect(struct usb_interface *ifce);
static int  usb_send_cmd_ir(char *full_command);
static ssize_t attr_show(struct kobject *sys_obj, struct kobj_attribute *attr, char *buff);
static ssize_t attr_store(struct kobject *sys_obj, struct kobj_attribute *attr, const char *buff, size_t count);
static void run_test_tx(void);
static void run_test_nec(void);
static void run_all_tests(void);

// Variáveis de estado
static char recv_line[MAX_RECV_LINE];
static struct usb_device *ir_device;
static uint usb_in, usb_out;
static char *usb_in_buffer, *usb_out_buffer;
static int usb_max_size;
bool ignore = true;

// Variável Global para Persistência: Armazena o último comando IR enviado com sucesso
static char last_ir_command[MAX_RECV_LINE] = "Nenhum comando IR enviado ainda.";

// Variáveis para criar o arquivo no /sys/kernel/infrared/transmit
static struct kobj_attribute  transmit_attribute = __ATTR(transmit, S_IRUGO | S_IWUSR, attr_show, attr_store);
static struct attribute      *attrs[]       = { &transmit_attribute.attr, NULL };
static struct attribute_group attr_group    = { .attrs = attrs };
static struct kobject        *sys_obj;

// String de teste MUITO mais curta (apenas Header + 4 bits de dados)
static char test_command_tx[] = "38000 9000,4500,560,560,560,1690,560,560,560,560,560,1690,560,40180";

// NOVO: String de teste NEC (exemplo de NEC 32 bits)
static char test_command_nec[] = "NEC 10C8E11E";

// Função para configurar os parâmetros seriais do CP2102 via Control-Messages
static int ir_config_serial(struct usb_device *dev)
{
    int ret;
    u32 baudrate = 115200; // Defina o baud rate que seu ESP32 usa!

    printk(KERN_INFO "IR_EMITTER: Configurando a porta serial...\n");

    // 1. Habilita a interface UART do CP2102
    //    Comando específico do vendor Silicon Labs (CP210X_IFC_ENABLE)
    //    bmRequestType: 0x41 (Vendor, Host-to-Device, Interface)
    //    bRequest: 0x00 (CP210X_IFC_ENABLE)
    //    wValue: 0x0001 (UART Enable)
    ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
                          0x00, 0x41, 0x0001, 0, NULL, 0, 1000);
    if (ret)
    {
        printk(KERN_ERR "IR_EMITTER: Erro ao habilitar a UART (código %d)\n", ret);
        return ret;
    }

    // 2. Define o baud rate
    //    Comando específico do vendor Silicon Labs (CP210X_SET_BAUDRATE)
    //    bRequest: 0x1E (CP210X_SET_BAUDRATE)
    ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
                          0x1E, 0x41, 0, 0, &baudrate, sizeof(baudrate), 1000);
    if (ret < 0)
    {
        printk(KERN_ERR "IR_EMITTER: Erro ao configurar o baud rate (código %d)\n", ret);
        return ret;
    }

    printk(KERN_INFO "IR_EMITTER: Baud rate configurado para %d\n", baudrate);
    return 0;
}

// =========================================================
// FUNÇÕES DE TESTE DIRETO NO PROBE (DEBUG)
// =========================================================

// Função de teste para o protocolo RAW/TX (Original)
static void run_test_tx(void) {
    int ret;
    printk(KERN_INFO "--- TESTE DIRETO USB (PROTOCOLO TX) INICIADO ---\n");

    // Chama a função de envio com a string de teste RAW
    ret = usb_send_cmd_ir(test_command_tx);

    if (ret > 0) {
        printk(KERN_INFO "--- TESTE TX USB SUCESSO! (Retorno: %d) ---\n", ret);
    } else {
        printk(KERN_ERR "--- TESTE TX USB FALHA! (Retorno: %d) ---\n", ret);
    }
    printk(KERN_INFO "--- TESTE TX USB FINALIZADO ---\n");
}

static void run_test_nec(void) {
    int ret;
    printk(KERN_INFO "--- TESTE DIRETO USB (PROTOCOLO NEC) INICIADO ---\n");

    // Chama a função de envio com a string de teste NEC
    ret = usb_send_cmd_ir(test_command_nec);

    if (ret > 0) {
        printk(KERN_INFO "--- TESTE NEC USB SUCESSO! (Retorno: %d) ---\n", ret);
    } else {
        printk(KERN_ERR "--- TESTE NEC USB FALHA! (Retorno: %d) ---\n", ret);
    }
    printk(KERN_INFO "--- TESTE NEC USB FINALIZADO ---\n");
}

// Função unificada para rodar todos os testes
static void run_all_tests(void) {
    run_test_tx();
    // Adicione um pequeno delay entre os testes para o ESP32 se recuperar
    msleep(200);
    run_test_nec();
}

// =========================================================
// REGISTRO (PROBE/DISCONNECT)
// =========================================================

static const struct usb_device_id id_table[] = { { USB_DEVICE(VENDOR_ID, PRODUCT_ID) }, {} };
MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver ir_driver = {
    .name        = "ir_emitter",
    .probe       = usb_probe,
    .disconnect  = usb_disconnect,
    .id_table    = id_table,
};
module_usb_driver(ir_driver);


static int usb_probe(struct usb_interface *interface, const struct usb_device_id *id) {
    struct usb_endpoint_descriptor *usb_endpoint_in, *usb_endpoint_out;
    int ret;

    printk(KERN_INFO "IR_EMITTER: Dispositivo conectado ...\n");
    // Cria arquivos do /sys/kernel/infrared/*
    sys_obj = kobject_create_and_add("infrared", kernel_kobj);
    if (!sys_obj) {
        printk(KERN_ERR "IR_EMITTER: Falha ao criar kobject 'infrared'.\n");
        return -ENOMEM;
    }

    if (sysfs_create_group(sys_obj, &attr_group)) {
        printk(KERN_ERR "IR_EMITTER: Falha ao criar grupo sysfs.\n");
        kobject_put(sys_obj);
        return -ENOMEM;
    }

    // Detecta portas e aloca buffers
    ir_device = interface_to_usbdev(interface);
    if (usb_find_common_endpoints(interface->cur_altsetting, &usb_endpoint_in, &usb_endpoint_out, NULL, NULL)) {
        printk(KERN_ERR "IR_EMITTER: Falha ao encontrar endpoints USB.\n");
        kobject_put(sys_obj);
        return -ENODEV;
    }

    usb_max_size = usb_endpoint_maxp(usb_endpoint_in);
    usb_in = usb_endpoint_in->bEndpointAddress;
    usb_out = usb_endpoint_out->bEndpointAddress;

    // Aloca buffers de entrada e saída
    usb_in_buffer = kmalloc(MAX_RECV_LINE, GFP_KERNEL);
    usb_out_buffer = kmalloc(MAX_RECV_LINE, GFP_KERNEL);

    if (!usb_in_buffer || !usb_out_buffer) {
        printk(KERN_ERR "IR_EMITTER: Falha ao alocar buffers.\n");
        if (sys_obj) kobject_put(sys_obj);
        kfree(usb_in_buffer);
        kfree(usb_out_buffer);
        return -ENOMEM;
    }

    // Inicializa buffers após a alocação (NOVO)
    memset(usb_in_buffer, 0, MAX_RECV_LINE);
    memset(usb_out_buffer, 0, MAX_RECV_LINE);

    ret = ir_config_serial(ir_device);
    if (ret){
           printk(KERN_ERR "IR_EMITTER: Falha na configuração da serial\n");
           kfree(usb_in_buffer);
           kfree(usb_out_buffer);
           return ret;
       }

    // CHAMA TODOS OS TESTES DIRETOS APÓS A INICIALIZAÇÃO
    run_test_nec();

    return 0;
}


static void usb_disconnect(struct usb_interface *interface) {
    printk(KERN_INFO "IR_EMITTER: Dispositivo desconectado.\n");

    // Lógica: Remoção da interface sysfs e liberação dos buffers
    if (sys_obj) kobject_put(sys_obj);
    kfree(usb_in_buffer);
    kfree(usb_out_buffer);
}

// =========================================================
// ENVIO IR VIA USB (LÓGICA DE PROTOCOLO E ESTADO)
// =========================================================

// Envia o comando IR completo (string) via USB
static int usb_send_cmd_ir(char *full_command) {
    int ret, actual_size;
    int attempts = 10;              // menos tentativas para evitar travar
    int read_timeout_ms = 200;      // timeout mais curto (200ms)

    char final_command[MAX_RECV_LINE] = {0};
    char *expected_ok_prefix;
    char *start_ptr, *newline_ptr;
    char *full_response = kmalloc(MAX_RECV_LINE, GFP_KERNEL);
    if (!full_response) return -ENOMEM;

    memset(full_response, 0, MAX_RECV_LINE);
    memset(recv_line, 0, MAX_RECV_LINE);

    // Monta o comando
    if (strncmp(full_command, "NEC ", 4) == 0) {
        snprintf(final_command, MAX_RECV_LINE, "%s\n", full_command);
        expected_ok_prefix = "[OK] NEC";
    } else {
        snprintf(final_command, MAX_RECV_LINE, "TX %s\n", full_command);
        expected_ok_prefix = "[OK] TX";
    }

    strncpy(usb_out_buffer, final_command, MAX_RECV_LINE);

    printk(KERN_INFO "IR_EMITTER: Enviando comando: '%s'\n", usb_out_buffer);

    // Envia comando para o ESP32 via USB
    ret = usb_bulk_msg(ir_device, usb_sndbulkpipe(ir_device, usb_out),
                       usb_out_buffer, strlen(usb_out_buffer), &actual_size, 1000);
    if (ret) {
        printk(KERN_ERR "IR_EMITTER: Falha ao enviar comando! Código %d\n", ret);
        goto cleanup;
    }

    // Pequena pausa para o ESP32 processar
    msleep(50);

    printk(KERN_INFO "DEBUG: Iniciando leitura USB (%d tentativas, timeout=%dms)...", attempts, read_timeout_ms);

    // Loop de leitura com tempo reduzido
    while (attempts-- > 0) {
        ret = usb_bulk_msg(ir_device, usb_rcvbulkpipe(ir_device, usb_in),
                           usb_in_buffer, usb_max_size, &actual_size, read_timeout_ms);

        if (ret == -ETIMEDOUT || actual_size == 0) {
            msleep(10); // evita travar CPU
            continue;
        } else if (ret) {
            printk(KERN_ERR "IR_EMITTER: Erro de leitura USB (%d). Código: %d\n", attempts, ret);
            goto cleanup;
        }

        usb_in_buffer[actual_size] = '\0';
        strncat(full_response, usb_in_buffer, MAX_RECV_LINE - strlen(full_response) - 1);

        printk(KERN_INFO "DEBUG: Recebido [%d bytes]: '%s'\n", actual_size, usb_in_buffer);

        // Procura por [OK] ou [ERR]
        start_ptr = strstr(full_response, expected_ok_prefix);
        if (!start_ptr)
            start_ptr = strstr(full_response, "[ERR]");

        if (start_ptr) {
            newline_ptr = strchr(start_ptr, '\n');
            if (newline_ptr)
                *newline_ptr = '\0';

            printk(KERN_INFO "IR_EMITTER: Resposta recebida: '%s'\n", start_ptr);

            if (!strncmp(start_ptr, expected_ok_prefix, strlen(expected_ok_prefix))) {
                printk(KERN_INFO "IR_EMITTER: Comando executado com sucesso.\n");
                snprintf(last_ir_command, MAX_RECV_LINE, "%s", full_command);
                ret = 1;
                goto cleanup;
            } else {
                printk(KERN_ERR "IR_EMITTER: Firmware retornou erro: %s\n", start_ptr);
                ret = -EIO;
                goto cleanup;
            }
        }
    }

    printk(KERN_WARNING "IR_EMITTER: Nenhuma resposta recebida (timeout após várias tentativas).\n");
    ret = 0; // considera como sucesso silencioso, apenas sem confirmação

cleanup:
    kfree(full_response);
    return ret;
}



// =========================================================
// INTERFACE SYSFS (LEITURA/ESCRITA)
// =========================================================

static ssize_t attr_show(struct kobject *sys_obj, struct kobj_attribute *attr, char *buff) {
    // Retorna o último comando enviado (registro persistente)
    return sprintf(buff, "Último TX enviado: %s\n", last_ir_command);
}

// Executado quando o arquivo /sys/kernel/infrared/transmit é escrito (A HAL ESCREVE AQUI!)
static ssize_t attr_store(struct kobject *sys_obj, struct kobj_attribute *attr, const char *buff, size_t count) {
    int ret;
    // 1. TRATAMENTO DO BUFFER E VALIDAÇÃO DE PROTOCOLO ('\n')
    char command[MAX_RECV_LINE];
    size_t data_len = count; // data_len inicial é o tamanho total
    char full_ir_command[MAX_RECV_LINE];
    char *command_payload;

    // PASSO DE VALIDAÇÃO CRÍTICA: O ÚLTIMO CARACTERE DEVE SER '\n' (LINHA NOVA)
    if (data_len == 0 || buff[data_len - 1] != '\n') {
        printk(KERN_ERR "IR_EMITTER: Erro de protocolo! A HAL DEVE encerrar o comando com '\\n'.\n");
        return -EINVAL; // Retorna Erro de Argumento Inválido
    }

    // Se a validação passou, removemos o '\n' para não enviá-lo para o ESP32
    data_len--; // Desconsidera o '\n'

    // Garantia de que a string cabe no buffer local
    if (data_len >= MAX_RECV_LINE - 4) { // -4 para "TX " e '\0'
        printk(KERN_ERR "IR_EMITTER: Comando IR muito longo. Max: %d\n", MAX_RECV_LINE - 4);
        return -EINVAL;
    }

    // 1c. COPIA: Copia o conteúdo da HAL (buff) para a variável local (command)
    strncpy(command, buff, data_len);
    command[data_len] = '\0';
    printk(KERN_INFO "IR_EMITTER: Recebido da HAL: '%s'", command); // Removi o '\n' do log

    // ----------------------------------------------------
    // PASSO NOVO: VALIDAÇÃO DO PROTOCOLO NEC
    // ----------------------------------------------------
    if (strncmp(command, "NEC ", 4) == 0) {
        // Encontramos o prefixo NEC!
        char *hex_data = command + 4; // Aponta para o dado hexadecimal
        // Verifica se o dado HEX tem o tamanho esperado (8 chars)
        if (strlen(hex_data) == 8) {
            // O ESP32 espera: NEC <HEX8>\n
            snprintf(full_ir_command, MAX_RECV_LINE, "NEC %s", hex_data);
            printk(KERN_INFO "IR_EMITTER: Protocolo NEC detectado. Comando final: '%s'", full_ir_command);
        } else {
            printk(KERN_ERR "IR_EMITTER: Protocolo NEC invalido. Esperado: NEC <HEX8> (8 digitos).\n");
            return -EINVAL;
        }
    } else if (strncmp(command, "TX ", 3) == 0 || (command[0] >= '0' && command[0] <= '9')) {
        // Assume que é um comando RAW (TX <dados> ou <dados>) se não for NEC
        // O firmware original espera "TX <dados>", então formatamos para isso se for apenas raw data
        if (strncmp(command, "TX ", 3) == 0) {
            snprintf(full_ir_command, MAX_RECV_LINE, "%s", command); // Já está formatado
        } else {
            snprintf(full_ir_command, MAX_RECV_LINE, "TX %s", command); // Formata raw data
        }
    } else {
         // Comando RAW detectado, mas a função usb_send_cmd_ir adiciona "TX " internamente.
         // Para simplificar e manter a lógica original, vamos passar apenas o dado
         // e deixar que usb_send_cmd_ir o formate com "TX <dados>".
         snprintf(full_ir_command, MAX_RECV_LINE, "%s", command);
    }

    // 2. CHAMADA AO USB: Envia o comando IR para o ESP32
    // Passamos o comando sem o 'TX' ou 'NEC' pois a função usb_send_cmd_ir vai adicioná-lo
    // Mas note: O código original de 'usb_send_cmd_ir' *sempre* adiciona "TX " e '\n'.
    // Precisamos ajustar isso na 'usb_send_cmd_ir' ou passar a string completa.
    // Vamos passar APENAS o payload para a função usb_send_cmd_ir:

    command_payload = full_ir_command;

    // Se for NEC, o payload é "NEC <HEX8>"
    // Se for RAW/TX, o payload é o dado cru (ex: "38000 9000,...")
    if (strncmp(full_ir_command, "NEC ", 4) == 0) {
        // Passa a string completa "NEC <HEX8>"
    } else {
        // Passa apenas o payload RAW (o original do usuário)
        command_payload = command;
    }

    ret = usb_send_cmd_ir(command_payload); // Chamada da função com o comando

    // 3. RETORNO E PERSISTÊNCIA:
    if (ret > 0) { // Se o retorno for sucesso (ret == 1)
        // Persiste o comando para que attr_show possa exibi-lo
        snprintf(last_ir_command, MAX_RECV_LINE, "%s", command);
        return count; // Retorna o 'count' original (incluindo o '\n' que foi aceito)
    } else {
        printk(KERN_ALERT "IR_EMITTER: Falha na transmissao. Retorno: %d\n", ret);
        return -EIO; // Retorna erro de I/O para o userspace
    }
}
