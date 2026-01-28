obj-m += modetc.o
KERNEL_DIR ?= /lib/modules/$(shell uname -a)/build
INSTALL_MOD_DIR ?= /lib/modules/$(shell uname -a)/misc

all:
	make -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	make -C $(KERNEL_DIR) M=$(PWD) clean

install:
	install -Dm555 -t $(INSTALL_MOD_DIR) modetc.ko
