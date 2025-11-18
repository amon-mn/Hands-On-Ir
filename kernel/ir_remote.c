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
#include <linux/mutex.h> 


// DEFINIÇÕES E VARIÁVEIS GLOBAIS

MODULE_AUTHOR("Equipe 04 - DevTITANS");
MODULE_DESCRIPTION("Driver de acesso ao Emissor/Receptor Infravermelho");
MODULE_LICENSE("GPL");

#define MAX_RECV_LINE 500
#define VENDOR_ID  0x10C4
#define PRODUCT_ID 0xEA60

// Protótipos
static int  usb_probe(struct usb_interface *ifce, const struct usb_device_id *id);
static void usb_disconnect(struct usb_interface *ifce);
static int  usb_send_cmd_ir(char *full_command);
static ssize_t attr_show_transmit(struct kobject *sys_obj, struct kobj_attribute *attr, char *buff);
static ssize_t attr_store_transmit(struct kobject *sys_obj, struct kobj_attribute *attr, const char *buff, size_t count);

// Novos protótipos para o receive
static ssize_t attr_show_receive(struct kobject *sys_obj, struct kobj_attribute *attr, char *buff);
static ssize_t attr_store_receive(struct kobject *sys_obj, struct kobj_attribute *attr, const char *buff, size_t count);

static void cleanup_ir(char *buff);

// Variáveis de estado
static char recv_line[MAX_RECV_LINE];
static struct usb_device *ir_device;
static uint usb_in, usb_out;
static char *usb_in_buffer, *usb_out_buffer;
static int usb_max_size;
bool ignore = true;

// Variável Global para Persistência Transmit
static char last_ir_command[MAX_RECV_LINE] = "Nenhum comando IR enviado ainda.";

// Variável Global para Cache do Receive
static char cached_recv_buffer[MAX_RECV_LINE] = "Nenhum dado lido ainda.\n";

// Mutex para proteger acesso simultâneo (Transmit vs Receive)
static struct mutex ir_lock;

// Definição dos Arquivos Sysfs

static struct kobj_attribute transmit_attribute = __ATTR(transmit, 0660, attr_show_transmit, attr_store_transmit);
static struct kobj_attribute receive_attribute  = __ATTR(receive,  0660, attr_show_receive, attr_store_receive);

static struct attribute      *attrs[]       = { 
    &transmit_attribute.attr, 
    &receive_attribute.attr,
    NULL 
};
static struct attribute_group attr_group    = { .attrs = attrs };
static struct kobject        *sys_obj;

static void cleanup_ir(char *buf) {
    kfree(buf);
}

// Função para configurar os parâmetros seriais do CP2102 via Control-Messages
static int ir_config_serial(struct usb_device *dev){
    int ret;
    u32 baudrate = 115200; // Defina o baud rate que seu ESP32 usa!

    printk(KERN_INFO "IR_REMOTE: Configurando a porta serial...\n");

    // 1. Habilita a interface UART do CP2102
    //    Comando específico do vendor Silicon Labs (CP210X_IFC_ENABLE)
    //    bmRequestType: 0x41 (Vendor, Host-to-Device, Interface)
    //    bRequest: 0x00 (CP210X_IFC_ENABLE)
    //    wValue: 0x0001 (UART Enable)
    ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
                          0x00, 0x41, 0x0001, 0, NULL, 0, 1000);
    if (ret){
        printk(KERN_ERR "IR_REMOTE: Erro ao habilitar a UART (código %d)\n", ret);
        return ret;
    }

    // 2. Define o baud rate
    //    Comando específico do vendor Silicon Labs (CP210X_SET_BAUDRATE)
    //    bRequest: 0x1E (CP210X_SET_BAUDRATE)
    ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
                          0x1E, 0x41, 0, 0, &baudrate, sizeof(baudrate), 1000);
    if (ret < 0){
        printk(KERN_ERR "IR_REMOTE: Erro ao configurar o baud rate (código %d)\n", ret);
        return ret;
    }

    printk(KERN_INFO "IR_REMOTE: Baud rate configurado para %d\n", baudrate);
    return 0;
}


// REGISTRO (PROBE/DISCONNECT)
static const struct usb_device_id id_table[] = { { USB_DEVICE(VENDOR_ID, PRODUCT_ID) }, {} };
MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver ir_driver = {
    .name        = "ir_remote",
    .probe       = usb_probe,
    .disconnect  = usb_disconnect,
    .id_table    = id_table,
};
module_usb_driver(ir_driver);

static int usb_probe(struct usb_interface *interface, const struct usb_device_id *id) {
    struct usb_endpoint_descriptor *usb_endpoint_in, *usb_endpoint_out;
    int ret;

    printk(KERN_INFO "IR_REMOTE: Dispositivo conectado ...\n");
    
    sys_obj = kobject_create_and_add("infrared", kernel_kobj);
    if (!sys_obj) return -ENOMEM;

    if (sysfs_create_group(sys_obj, &attr_group)) {
        kobject_put(sys_obj);
        return -ENOMEM;
    }

    ir_device = interface_to_usbdev(interface);
    if (usb_find_common_endpoints(interface->cur_altsetting, &usb_endpoint_in, &usb_endpoint_out, NULL, NULL)) {
        kobject_put(sys_obj);
        return -ENODEV;
    }

    usb_max_size = usb_endpoint_maxp(usb_endpoint_in);
    usb_in = usb_endpoint_in->bEndpointAddress;
    usb_out = usb_endpoint_out->bEndpointAddress;

    usb_in_buffer = kmalloc(MAX_RECV_LINE, GFP_KERNEL);
    usb_out_buffer = kmalloc(MAX_RECV_LINE, GFP_KERNEL);

    if (!usb_in_buffer || !usb_out_buffer) {
        if (sys_obj) kobject_put(sys_obj);
        kfree(usb_in_buffer);
        kfree(usb_out_buffer);
        return -ENOMEM;
    }

    memset(usb_in_buffer, 0, MAX_RECV_LINE);
    memset(usb_out_buffer, 0, MAX_RECV_LINE);

    mutex_init(&ir_lock);

    ret = ir_config_serial(ir_device);
    if (ret){
           kfree(usb_in_buffer);
           kfree(usb_out_buffer);
           return ret;
       }

    return 0;
}

static void usb_disconnect(struct usb_interface *interface) {
    printk(KERN_INFO "IR_REMOTE: Dispositivo desconectado.\n");
    if (sys_obj) kobject_put(sys_obj);
    kfree(usb_in_buffer);
    kfree(usb_out_buffer);
}


// ENVIO IR VIA USB 
// Envia o comando IR completo (string) via USB
static int usb_send_cmd_ir(char *full_command) {
    int ret, actual_size;
    int attempts = 10;              // menos tentativas para evitar travar
    int read_timeout_ms = 200;      // timeout mais curto (200ms)
    char final_command[MAX_RECV_LINE] = {0};
    char *expected_ok_prefix;
    char *start_ptr, *newline_ptr;
    
    // 1. Aloca o buffer de resposta
    char *full_response = kmalloc(MAX_RECV_LINE, GFP_KERNEL);
    if (!full_response) 
        return -ENOMEM; // Sai cedo, nada para limpar

    memset(full_response, 0, MAX_RECV_LINE);
    memset(recv_line, 0, MAX_RECV_LINE);

    // Monta o comando
    if (strncmp(full_command, "NEC ", 4) == 0) {
        snprintf(final_command, MAX_RECV_LINE, "NEC %s\n", full_command);
        expected_ok_prefix = "[OK] NEC";
    } else {
        snprintf(final_command, MAX_RECV_LINE, "TX %s\n", full_command);
        expected_ok_prefix = "[OK] TX";
    }

    strncpy(usb_out_buffer, final_command, MAX_RECV_LINE);
    printk(KERN_INFO "IR_REMOTE: Enviando comando: '%s'\n", usb_out_buffer);

    // Envia comando para o ESP32 via USB
    ret = usb_bulk_msg(ir_device, usb_sndbulkpipe(ir_device, usb_out),
                       usb_out_buffer, strlen(usb_out_buffer), &actual_size, 1000);
    if (ret) {
        printk(KERN_ERR "IR_REMOTE: Falha ao enviar comando! Código %d\n", ret);
        cleanup_ir(full_response);
        return ret;
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
            printk(KERN_ERR "IR_REMOTE: Erro de leitura USB (%d). Código: %d\n", attempts, ret);
            cleanup_ir(full_response); 
            return ret;
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

            printk(KERN_INFO "IR_REMOTE: Resposta recebida: '%s'\n", start_ptr);

            if (!strncmp(start_ptr, expected_ok_prefix, strlen(expected_ok_prefix))) {
                printk(KERN_INFO "IR_REMOTE: Comando executado com sucesso.\n");
                snprintf(last_ir_command, MAX_RECV_LINE, "%s", full_command);
                ret = 1;
                cleanup_ir(full_response);
                return ret;
            } else {
                printk(KERN_ERR "IR_REMOTE: Firmware retornou erro: %s\n", start_ptr);
                ret = -EIO;
                cleanup_ir(full_response);
                return ret;
            }
        }
    }

    printk(KERN_WARNING "IR_REMOTE: Nenhuma resposta recebida (timeout após várias tentativas).\n");
    cleanup_ir(full_response);
    return 0;
}


// Função Específica para buscar dados (Receive) com lógica de Acumulação
static int usb_request_last_recv(void) {
    int ret, actual_size;
    int attempts = 100; 
    char *line_start;
    char *line_end;
    
    // Buffer temporário para ler TUDO
    char *raw_buffer = kmalloc(MAX_RECV_LINE, GFP_KERNEL);
    if (!raw_buffer) return -ENOMEM;

    // 1. Envia o comando
    memset(usb_out_buffer, 0, MAX_RECV_LINE);
    snprintf(usb_out_buffer, MAX_RECV_LINE, "LAST_RECV\n"); 

    printk(KERN_INFO "IR_REMOTE: Enviando trigger LAST_RECV...\n");

    ret = usb_bulk_msg(ir_device, usb_sndbulkpipe(ir_device, usb_out),
                       usb_out_buffer, strlen(usb_out_buffer), &actual_size, 1000);
    if (ret) {
        kfree(raw_buffer);
        return ret;
    }

    // 2. Limpa buffers
    memset(raw_buffer, 0, MAX_RECV_LINE);
    memset(cached_recv_buffer, 0, MAX_RECV_LINE); 

    // 3. Loop de Leitura
    while (attempts-- > 0) {
        memset(usb_in_buffer, 0, MAX_RECV_LINE);
        
        ret = usb_bulk_msg(ir_device, usb_rcvbulkpipe(ir_device, usb_in),
                           usb_in_buffer, usb_max_size, &actual_size, 100);

        if (ret == -ETIMEDOUT || actual_size == 0) {
            msleep(10); 
            continue;
        } else if (ret) {
            kfree(raw_buffer);
            return ret;
        }

        usb_in_buffer[actual_size] = '\0';

        if (strlen(raw_buffer) + actual_size >= MAX_RECV_LINE) {
             printk(KERN_ERR "IR_REMOTE: Overflow no raw_buffer!\n");
             break; 
        }

        strncat(raw_buffer, usb_in_buffer, MAX_RECV_LINE - strlen(raw_buffer) - 1);
        
        // Varre o buffer linha por linha procurando uma que comece com "REC "
        line_start = raw_buffer;
        while (line_start && *line_start) {
            // Procura o fim desta linha
            line_end = strchr(line_start, '\n');
            
            // Se achou um \n, analisar essa linha
            if (line_end) {
                // Verifica se a linha começa com "REC " (4 caracteres)
                // Isso ignora "[DBG]" e "[OK] REC..."
                if (strncmp(line_start, "REC ", 4) == 0) {
                    // ACHAMOS! Corta a string no \n
                    *line_end = '\0'; 
                    // Copia para o buffer final
                    strncpy(cached_recv_buffer, line_start, MAX_RECV_LINE);
                    printk(KERN_INFO "IR_REMOTE: Resposta recebida: '%s'\n", cached_recv_buffer);
                    kfree(raw_buffer);
                    return 0; // Sucesso Total
                }
                // Se não for essa linha, avança para a próxima (pula o \n)
                line_start = line_end + 1;
            } else {
                // Se não tem \n, a linha está incompleta. 
                // Sai do loop interno e deixa o 'while(attempts)' ler mais dados do USB.
                break; 
            }
        }
    }

    printk(KERN_WARNING "IR_REMOTE: Timeout. Assinatura 'REC ' não encontrada.\n");
    // printk(KERN_INFO "IR_REMOTE: Buffer bruto: %s\n", raw_buffer); // Descomente se precisar debugar
    kfree(raw_buffer);
    return -ETIMEDOUT;
}


// INTERFACE SYSFS (LEITURA/ESCRITA)

// --- TRANSMIT (Show) ---
static ssize_t attr_show_transmit(struct kobject *sys_obj, struct kobj_attribute *attr, char *buff) {
    return sprintf(buff, "Último TX enviado: %s\n", last_ir_command);
}

// Executado quando o arquivo /sys/kernel/infrared/transmit é escrito
static ssize_t attr_store_transmit(struct kobject *sys_obj, struct kobj_attribute *attr, const char *buff, size_t count) {
    int ret;
    // 1. TRATAMENTO DO BUFFER E VALIDAÇÃO DE PROTOCOLO ('\n')
    char command[MAX_RECV_LINE];
    size_t data_len = count; // data_len inicial é o tamanho total
    char full_ir_command[MAX_RECV_LINE];
    char *command_payload;

    // O ÚLTIMO CARACTERE DEVE SER '\n' 
    if (data_len == 0 || buff[data_len - 1] != '\n') {
        printk(KERN_ERR "IR_REMOTE: Erro de protocolo! A HAL DEVE encerrar o comando com '\\n'.\n");
        return -EINVAL; // Retorna Erro de Argumento Inválido
    }
    // Se a validação passou, removemos o '\n' para não enviá-lo para o ESP32
    data_len--; // Desconsidera o '\n'

    // Garantia de que a string cabe no buffer local
    if (data_len >= MAX_RECV_LINE - 4) { // -4 para "TX " e '\0'
        printk(KERN_ERR "IR_REMOTE: Comando IR muito longo. Max: %d\n", MAX_RECV_LINE - 4);
        return -EINVAL;
    }

    // 1. Copia o conteúdo da HAL (buff) para a variável local (command)
    strncpy(command, buff, data_len);
    command[data_len] = '\0';
    printk(KERN_INFO "IR_REMOTE: Recebido da HAL: '%s'", command); // Removi o '\n' do log

    if (strncmp(command, "NEC ", 4) == 0) {
        // Encontramos o prefixo NEC!
        char *hex_data = command + 4; // Aponta para o dado hexadecimal
        // Verifica se o dado HEX tem o tamanho esperado (8 chars)
        if (strlen(hex_data) == 8) {
            // O ESP32 espera: NEC <HEX8>\n
            snprintf(full_ir_command, MAX_RECV_LINE, "NEC %s", hex_data);
            printk(KERN_INFO "IR_REMOTE: Protocolo NEC detectado. Comando final: '%s'", full_ir_command);
        } else {
            printk(KERN_ERR "IR_REMOTE: Protocolo NEC invalido. Esperado: NEC <HEX8> (8 digitos).\n");
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

    // 2. Envia o comando IR para o ESP32
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

    
    mutex_lock(&ir_lock); 
    ret = usb_send_cmd_ir(command_payload); // Chamada da função com o comando
    mutex_unlock(&ir_lock); 

    // 3. RETORNO E PERSISTÊNCIA:
    if (ret > 0) { // Se o retorno for sucesso (ret == 1)
        // Persiste o comando para que attr_show possa exibi-lo
        snprintf(last_ir_command, MAX_RECV_LINE, "%s", command);
        return count; // Retorna o 'count' original (incluindo o '\n' que foi aceito)
    } else {
        printk(KERN_ALERT "IR_REMOTE: Falha na transmissao. Retorno: %d\n", ret);
        return -EIO; // Retorna erro de I/O para o userspace
    }
}

// --- RECEIVE (Show) ---
static ssize_t attr_show_receive(struct kobject *sys_obj, struct kobj_attribute *attr, char *buff) {
    // Retorna o último comando recebido
    return sprintf(buff, "%s", cached_recv_buffer);
}

// Executado quando o arquivo /sys/kernel/infrared/receive é escrito (TRIGGER PARA ATUALIZAR LEITURA)
static ssize_t attr_store_receive(struct kobject *sys_obj, struct kobj_attribute *attr, const char *buff, size_t count) {
    int ret;
    // 1. TRATAMENTO DO BUFFER E VALIDAÇÃO DE PROTOCOLO ('\n')
    char command[MAX_RECV_LINE];
    size_t data_len = count; // data_len inicial é o tamanho total

    // O ÚLTIMO CARACTERE DEVE SER '\n'
    if (data_len == 0 || buff[data_len - 1] != '\n') {
        printk(KERN_ERR "IR_REMOTE: Erro de protocolo (Receive)! A HAL DEVE encerrar o comando com '\\n'.\n");
        return -EINVAL; // Retorna Erro de Argumento Inválido
    }

    // Se a validação passou, removemos o '\n'
    data_len--; // Desconsidera o '\n'

    // Garantia de que a string cabe no buffer local
    if (data_len >= MAX_RECV_LINE - 1) {
        printk(KERN_ERR "IR_REMOTE: Comando Trigger muito longo. Max: %d\n", MAX_RECV_LINE - 1);
        return -EINVAL;
    }

    // 1. Copia o conteúdo da HAL (buff) para a variável local (command)
    strncpy(command, buff, data_len);
    command[data_len] = '\0';
    printk(KERN_INFO "IR_REMOTE: Recebido da HAL (Receive Trigger): '%s'", command);

    if (strncmp(command, "LAST_RECV", 9) != 0) {
        printk(KERN_ERR "IR_REMOTE: Comando inválido para receive. Esperado: 'LAST_RECV'. Recebido: '%s'\n", command);
        return -EINVAL;
    }

    // 2. Busca o último sinal recebido pelo Firmware
  
    mutex_lock(&ir_lock); 
    ret = usb_request_last_recv();
    mutex_unlock(&ir_lock); 

    // 3. RETORNO:
    if (ret == 0) { // Se o retorno for sucesso (0)
        // O buffer 'cached_recv_buffer' foi atualizado com sucesso.
        return count; // Retorna o 'count' original indicando sucesso na escrita
    } else {
        printk(KERN_ALERT "IR_REMOTE: Falha na busca de dados (USB). Retorno: %d\n", ret);
        return -EIO; // Retorna erro de I/O para o userspace
    }

}
