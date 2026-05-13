obj-m += modetc.o
KERNEL_DIR ?= /usr/lib/modules/$(shell uname -r)/build
INSTALL_MOD_DIR ?= /usr/lib/modules/$(shell uname -r)/misc

all:
	make -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	make -C $(KERNEL_DIR) M=$(PWD) clean

install:
	install -Dm555 -t $(INSTALL_MOD_DIR) modetc.ko
