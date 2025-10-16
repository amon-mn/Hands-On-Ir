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
static void run_direct_test(void); 

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
static char test_command[] = "38000 9000,4500,560,560,560,1690,560,560,560,560,560,1690,560,40180";


// =========================================================
// FUNÇÃO DE TESTE DIRETO NO PROBE (DEBUG)
// =========================================================
static void run_direct_test(void) { 
    int ret;
    printk(KERN_INFO "--- TESTE DIRETO DE USB (ATOMIC) INICIADO (30 TENTATIVAS) ---\n");

    // Chama diretamente a função de envio com a string de teste
    ret = usb_send_cmd_ir(test_command);

    if (ret > 0) {
        printk(KERN_INFO "--- TESTE DIRETO USB SUCESSO! (Retorno: %d) ---\n", ret);
    } else {
        printk(KERN_ERR "--- TESTE DIRETO USB FALHA! (Retorno: %d) ---\n", ret);
    }
    printk(KERN_INFO "--- TESTE DIRETO DE USB FINALIZADO ---\n");
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

    // PASSO NOVO: CHAMA O TESTE DIRETO APÓS A INICIALIZAÇÃO
    run_direct_test(); 

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
    int attempts = 30;                        
    char resp_expected[] = "[OK] TX"; 
    
    // NOVO: Buffer para acumular a resposta completa do ESP32
    char full_response[MAX_RECV_LINE] = {0}; // Inicializado com 0
    char *start_ptr, *newline_ptr;

    // NOVO: Limpa buffers globais antes de iniciar a transação (Segurança)
    memset(recv_line, 0, MAX_RECV_LINE); 
    
    // 1. Constrói o comando no formato que o firmware espera: TX <dados>\n
    sprintf(usb_out_buffer, "TX %s\n", full_command); 
    
    // LOG DE DEBUG: Mostra o comando exato que está sendo enviado para a USB
    printk(KERN_INFO "IR_EMITTER: Enviando comando USB-Serial: '%s'", usb_out_buffer); 

    // 2. Envia o comando para a USB
    ret = usb_bulk_msg(ir_device, usb_sndbulkpipe(ir_device, usb_out), 
                       usb_out_buffer, strlen(usb_out_buffer), &actual_size, 1000*HZ);
    if (ret) {
        printk(KERN_ERR "IR_EMITTER: Erro de codigo %d ao enviar comando! Falha no TX USB.\n", ret);
        return -1;
    }

    // 3. Espera pela resposta correta do dispositivo (Lógica Robusta de Leitura)
    printk(KERN_INFO "DEBUG: Iniciando loop de leitura por %d tentativas (Timeout de 100ms/tentativa)", attempts);

    while (attempts > 0) {
        // Log de iteração (a cada 100ms)
        printk(KERN_INFO "DEBUG: Tentativa restante: %d", attempts);

        // Tenta ler dados da USB - Timeout de 100ms (HZ/10)
        ret = usb_bulk_msg(ir_device, usb_rcvbulkpipe(ir_device, usb_in), 
                           usb_in_buffer, usb_max_size, &actual_size, HZ/10); 
        
        // --- Tratamento de Erros e Timeouts ---
        if (ret == -ETIMEDOUT || actual_size == 0) { 
             attempts--;
             continue;
        } else if (ret) { 
            // Erro de I/O na leitura, mas não timeout
            printk(KERN_ERR "IR_EMITTER: Erro ao ler dados da USB (tentativa %d). Codigo: %d\n", attempts, ret);
            return -1;
        }
        
        // --- Acumulação de Dados ---
        usb_in_buffer[actual_size] = '\0';
        // strncat é seguro e acumula o novo pedaço no buffer completo
        strncat(full_response, usb_in_buffer, sizeof(full_response) - strlen(full_response) - 1);
        
        // LOG DE DEBUG: Mostra o que foi lido e o que foi acumulado
        printk(KERN_INFO "DEBUG: Lidos: %d. Conteudo BRUTO: '%s'. Acumulado: '%s'", actual_size, usb_in_buffer, full_response);

        // --- Parsing e Validação (Ponteiros) ---
        // Procura pelo prefixo de sucesso/erro no buffer acumulado
        start_ptr = strstr(full_response, "[OK] TX");
        if (!start_ptr) {
            start_ptr = strstr(full_response, "[ERR]");
        }

        if (start_ptr) {
            // Se encontrarmos o prefixo, procuramos o final da linha
            newline_ptr = strchr(start_ptr, '\n');
            
            if (newline_ptr) {
                // Se encontramos o '\n', a mensagem está completa!
                *newline_ptr = '\0'; // Corta a string no '\n'
                
                printk(KERN_INFO "IR_EMITTER: Resposta completa recebida: '%s'", start_ptr);

                if (!strncmp(start_ptr, resp_expected, 7)) { 
                    printk(KERN_INFO "IR_EMITTER: Confirmacao de TX recebida. SUCESSO.\n");
                    return 1; // Retorna SUCESSO
                } else {
                    printk(KERN_ERR "IR_EMITTER: Recebido ERRO do firmware: %s\n", start_ptr);
                    return -1; // Retorna Falha
                }
            }
        }
        
        // Se a mensagem for parcial, continuamos tentando
        attempts--;
    }
    
    // FIM DO LOOP: Falha no Timeout
    printk(KERN_ERR "IR_EMITTER: Falha ao ler resposta após %d tentativas (Timeout).", 30);
    return -1; 
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

    // LOG DE DEBUG: Mostra o comando exato que veio da HAL (leitura do arquivo)
    printk(KERN_INFO "IR_EMITTER: Recebido da HAL: '%s'\n", command);

    // 2. CHAMADA AO USB: Envia o comando IR para o ESP32
    ret = usb_send_cmd_ir(command);

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
