# DevTITANS 07 - Equipe 04 - Projeto Final: Emissor de Infravermelho

Bem-vindo ao repositório do projeto final da capacitação em **Sistemas Embarcados (DevTITANS)**!  
Este projeto tem como objetivo integrar um hardware emissor de infravermelho (IR) com a arquitetura **AOSP**, utilizando uma implementação de **HAL (Hardware Abstraction Layer)** customizada.

O principal desafio é estabelecer uma comunicação eficiente entre um protótipo de hardware baseado em **ESP32** e a camada de framework do Android, por meio de um **driver de kernel**.

---

## Tabela de Conteúdos
- [Equipe](#equipe)
- [Visão Geral do Projeto](#visão-geral-do-projeto)
- [Arquitetura do Sistema](#arquitetura-do-sistema)
- [Divisão de Tarefas (Roles)](#divisão-de-tarefas-roles)
- [Pré-requisitos e Setup](#pré-requisitos-e-setup)
- [Instruções de Instalação e Uso](#instruções-de-instalação-e-uso)
- [Roadmap e Progresso](#roadmap-e-progresso)
- [Contato](#contato)

---

## Equipe

| Papel                                | Membro            |
|--------------------------------------|-------------------|
| Engenheiro de Hardware e Firmware    | George D'Paula    |
| Engenheiro de Kernel e SELinux       | Marcelo Araújo    |
| Engenheiro de HAL                    | Mateus de Jesus   |
| Engenheiro de Framework e Pesquisa   | Amon Menezes      |
| Engenheiro de Aplicação e Integração | Thiago Vítor      |

---

## Visão Geral do Projeto

Este projeto tem como objetivo aplicar conceitos de **sistemas embarcados** e da **arquitetura do Android**, integrando hardware e software em uma solução funcional de emissão de sinais infravermelhos. 
A meta é permitir que um aplicativo Android padrão envie comandos de infravermelho através de um hardware externo e customizado.

### Fluxo de dados:

1. Um aplicativo Android envia um comando de IR via **ConsumerIrManager**.  
2. O **Framework do Android** roteia o comando para a nossa **HAL customizada**.  
3. A **HAL** traduz o comando para um formato aceitável pelo **driver de kernel**.  
4. O **driver de kernel** se comunica via **serial USB** com o **hardware ESP32**.  
5. O **ESP32** recebe o comando e modula um **LED IR** para transmiti-lo.

---

## Arquitetura do Sistema
```bash
+----------------+      +-------------------+      +-------------+
|    Android     | <--> |        HAL        | <--> |   Kernel    |
|  Framework     |      |   (ConsumerIrHal) |      |   Driver    |
| (App, Service) |      |                   |      |   (sysfs)   |
+----------------+      +-------------------+      +-------------+
        |
        | Comunicação USB-Serial
        v
+-------------------------+
|        Hardware         |
|   (ESP32 + LED IR)      |
+-------------------------+
```

---

## Divisão de Tarefas

- **Engenheiro de Hardware e Firmware (HF):** Projetar, montar e programar o hardware emissor de IR com o ESP32.  
- **Engenheiro de Kernel e SELinux (DKS):** Implementar o driver de kernel que cria a ponte de comunicação no sysfs e gerenciar as permissões de acesso.  
- **Engenheiro de HAL (H):** Escrever a implementação da HAL em C++ que conecta o framework do Android ao driver de kernel.  
- **Engenheiro de Framework e Pesquisa (FP):** Pesquisar a arquitetura AOSP, garantir a correta integração da HAL no framework e liderar o alinhamento do projeto.  
- **Engenheiro de Aplicação e Integração (AI):** Desenvolver a aplicação de controle remoto e orquestrar a validação e integração de ponta a ponta.  

---

# Pré-requisitos e Setup

## Repositório
Clone o repositório:
```bash
git clone https://github.com/amon-mn/Hands-On-Ir.git
```

Ferramenta de Gestão: acompanhe o nosso progresso no **[Github Projects](https://github.com/users/amon-mn/projects/3)**.

---

## Ambientes de Desenvolvimento

### Hardware/Firmware
- **Arduino IDE**

### Kernel/HAL
- **Ambiente de build do AOSP (Android Open Source Project)**

### Aplicação
- **Android Studio**

---

## Instruções de Instalação e Uso
As instruções detalhadas de como compilar e instalar cada componente serão adicionadas aqui nas próximas sprints.  

➡️ **A Sprint 0 está focada no planejamento e na configuração.**


