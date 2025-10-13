#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>  
#include <linux/string.h>
#include <linux/kernel.h> 

// =========================================================
// DEFINIÇÕES E VARIÁVEIS GLOBAIS
// =========================================================

MODULE_AUTHOR("Equipe 04 - DevTITANS");
MODULE_DESCRIPTION("Driver de acesso ao Emissor Infravermelho");
MODULE_LICENSE("GPL");

#define MAX_RECV_LINE 256 
#define VENDOR_ID  0x10C4  /* VendorID do CP2102 */
#define PRODUCT_ID 0xEA60  /* ProductID do CP2102 */

// Protótipos das funções do driver
static int  usb_probe(struct usb_interface *ifce, const struct usb_device_id *id); 
static void usb_disconnect(struct usb_interface *ifce);                            
static int  usb_send_cmd_ir(char *full_command); // Função que envia o comando via USB
static ssize_t attr_show(struct kobject *sys_obj, struct kobj_attribute *attr, char *buff); // Leitura do sysfs
static ssize_t attr_store(struct kobject *sys_obj, struct kobj_attribute *attr, const char *buff, size_t count); // Escrita no sysfs

// Variáveis de estado
static char recv_line[MAX_RECV_LINE];
static struct usb_device *ir_device;        
static uint usb_in, usb_out;                       
static char *usb_in_buffer, *usb_out_buffer;       
static int usb_max_size;                           
bool ignore = true;

// / Variáveis para criar o arquivo no /sys/kernel/infrared/transmit
static struct kobj_attribute  transmit_attribute = __ATTR(transmit, S_IRUGO | S_IWUSR, attr_show, attr_store);
static struct attribute      *attrs[]       = { &transmit_attribute.attr, NULL };
static struct attribute_group attr_group    = { .attrs = attrs };
static struct kobject        *sys_obj;


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
    ignore = sysfs_create_group(sys_obj, &attr_group);

    // Detecta portas e aloca buffers
    ir_device = interface_to_usbdev(interface); 
    ignore =  usb_find_common_endpoints(interface->cur_altsetting, &usb_endpoint_in, &usb_endpoint_out, NULL, NULL);
    usb_max_size = usb_endpoint_maxp(usb_endpoint_in);
    usb_in = usb_endpoint_in->bEndpointAddress;
    usb_out = usb_endpoint_out->bEndpointAddress;
    // Aumentado para 256
    usb_in_buffer = kmalloc(MAX_RECV_LINE, GFP_KERNEL); 
    usb_out_buffer = kmalloc(MAX_RECV_LINE, GFP_KERNEL); 

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
// ENVIO IR VIA USB
// =========================================================

// Envia o comando IR completo (string) via USB
static int usb_send_cmd_ir(char *full_command) {
    int recv_size = 0;                     
    int ret, actual_size, i;
    int retries = 50;                        // Aumentado retries para dar tempo de resposta do firmware
    char resp_expected[] = "[OK] TX";        // Resposta esperada: [OK] TX
    
    
    // Constrói o comando no formato que o firmware espera: TX <dados>\n
    // Assumimos que 'full_command' já está no formato '<freq> <us,...>'
    
    if (strlen(full_command) + 4 > MAX_RECV_LINE) {
        printk(KERN_ERR "IR_EMITTER: Comando IR muito longo.\n");
        return -1;
    }

    sprintf(usb_out_buffer, "TX %s\n", full_command); 
    printk(KERN_INFO "IR_EMITTER: Enviando IR: %s", usb_out_buffer); // Já possui o \n

    // Envia o comando (usb_out_buffer) para a USB
    ret = usb_bulk_msg(ir_device, usb_sndbulkpipe(ir_device, usb_out), usb_out_buffer, strlen(usb_out_buffer), &actual_size, 1000*HZ);
    if (ret) {
        printk(KERN_ERR "IR_EMITTER: Erro de codigo %d ao enviar comando! USB_SEND_CMD_IR falhou.\n", ret);
        return -1;
    }

    // Espera pela resposta correta do dispositivo
    while (retries > 0) {
        // Lê dados da USB
        ret = usb_bulk_msg(ir_device, usb_rcvbulkpipe(ir_device, usb_in), usb_in_buffer, min(usb_max_size, MAX_RECV_LINE), &actual_size, HZ*1000);
        
        if (ret && ret != -ETIMEDOUT) { // Se for diferente de timeout e não 0 (sucesso)
            printk(KERN_ERR "IR_EMITTER: Erro ao ler dados da USB (tentativa %d). Codigo: %d\n", ret, retries--);
            continue;
        } else if (ret == -ETIMEDOUT) {
             retries--;
             continue;
        }

        // Para cada caractere recebido ...
        for (i=0; i<actual_size; i++) {

            if (usb_in_buffer[i] == '\n') {  // Temos uma linha completa
                recv_line[recv_size] = '\0';
                printk(KERN_INFO "IR_EMITTER: Recebido linha: '%s'\n", recv_line);

                // VERIFICA SE O INÍCIO DA LINHA É IGUAL À RESPOSTA ESPERADA ([OK] TX)
                if (!strncmp(recv_line, resp_expected, strlen(resp_expected))) {
                    
                    // A linha é uma resposta de sucesso!
                    printk(KERN_INFO "IR_EMITTER: Confirmacao de TX recebida. Sucesso.\n");
                    
                    return 1; // Retorna SUCESSO
                }
                // Adicional: Verificamos se é uma mensagem de ERRO do firmware
                else if (!strncmp(recv_line, "[ERR]", 5)) {
                    printk(KERN_ERR "IR_EMITTER: Recebido ERRO do firmware: %s\n", recv_line);
                    return -1; // Retorna Falha
                }
                else { 
                    printk(KERN_INFO "IR_EMITTER: Linha nao eh resposta TX. Tentiva %d. Proxima linha...\n", retries--);
                    recv_size = 0; // Limpa a linha lida
                }
            }
            else { 
                // AQUI ESTÁ A LÓGICA DE PREVENÇÃO DE OVERFLOW
                if (recv_size >= MAX_RECV_LINE - 1) { // -1 para o \0
                    recv_size = 0; // Limpa o buffer
                    printk(KERN_ERR "IR_EMITTER: Overflow na linha recebida (limite atingido, buffer limpo).\n");
                    // Não é necessário sair do loop 'for' aqui, a próxima iteração irá recomeçar a preencher.
                } else {
                     // É um caractere normal, adiciona ao buffer
                    recv_line[recv_size] = usb_in_buffer[i];
                    recv_size++;
                }
            }
        }
    }
    return -1; // Não recebeu a resposta esperada após todas as tentativas
}


// =========================================================
// INTERFACE SYSFS (LEITURA/ESCRITA)
// =========================================================

static ssize_t attr_show(struct kobject *sys_obj, struct kobj_attribute *attr, char *buff) {
    // Implementação básica de leitura (apenas debug)
    return sprintf(buff, "Pronto para TX. Atributo: %s\n", attr->attr.name);
}

// Executado quando o arquivo /sys/kernel/infrared/transmit é escrito (A HAL ESCREVE AQUI!)
static ssize_t attr_store(struct kobject *sys_obj, struct kobj_attribute *attr, const char *buff, size_t count) {
    int ret;

    // A HAL escreverá a string IR no formato que definimos na Sprint 1 (ex: "38000 9000,4500,...")
    // O 'buff' contém a string.

    // 1. O buff vem com '\n' e '\0' no final; removemos o último caractere '\n'
    char command[MAX_RECV_LINE];
    size_t data_len = (count < MAX_RECV_LINE) ? count : MAX_RECV_LINE - 1;

    // 1. TRATAMENTO DO BUFFER: Copiar o conteúdo da HAL (buff) para uma variável local (command)
    if (data_len > 0 && buff[data_len - 1] == '\n') data_len--;
    if (data_len == 0) return -EINVAL; 
    
    strncpy(command, buff, data_len);
    command[data_len] = '\0'; 

    printk(KERN_INFO "IR_EMITTER: Recebido da HAL: '%s'\n", command);

    // 2. CHAMADA AO USB: Envia o comando IR para o ESP32
    ret = usb_send_cmd_ir(command);

    if (ret < 0) {
        printk(KERN_ALERT "IR_EMITTER: Falha na transmissao. Retorno: %d\n", ret);
        return -EIO; 
    }

    // 3. RETORNO: Confirmação de bytes processados
    return count; 
}
