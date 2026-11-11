KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
INSTALL_MOD_PATH ?= /lib/modules/$(shell uname -r)
INSTALL_MOD_DIR ?= misc

obj-m += modetc.o

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD)

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) $@

modules_install:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) $@

install: modules_install
