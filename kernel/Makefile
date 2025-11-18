# Define os arquivos que compõem o módulo.
# No seu caso, o arquivo é smartlamp.c, então o objeto é smartlamp.o.
obj-m += ir_remote.o

# Obtém a versão do kernel em execução no seu sistema.
# Ex: 5.15.0-86-generic
KVERSION := $(shell uname -r)

# Obtém o caminho completo para a pasta atual do seu módulo.
PWD := $(CURDIR)

# Alvo 'all': compila o módulo.
# O '-C' muda o diretório para a pasta de build do kernel.
# O 'M=$(PWD)' informa onde o make deve procurar os arquivos do módulo.
all:
	$(MAKE) -C /lib/modules/$(KVERSION)/build M=$(PWD) modules

# Alvo 'clean': remove os arquivos compilados (módulo, objetos, etc.).
clean:
	$(MAKE) -C /lib/modules/$(KVERSION)/build M=$(PWD) clean