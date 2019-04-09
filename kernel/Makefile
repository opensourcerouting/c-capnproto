KDIR := /lib/modules/$(shell uname -r)/build
MODULE_NAME = "address"
# Temporary hack to make it work. Needs to be define as an environment variable
FULL_CAPN_DIR=$(PWD)/lib
CAPN_DIR=lib
EXTRA_CFLAGS:=-I$(FULL_CAPN_DIR)

obj-m += $(MODULE_NAME).o
$(MODULE_NAME)-objs := src/main.o $(CAPN_DIR)/capn-malloc.o $(CAPN_DIR)/capn-stream.o $(CAPN_DIR)/capn.o src/addressbook.capnp.o

all: $(MODULE_NAME).ko 

$(MODULE_NAME).ko:
	$(MAKE) -C $(KDIR) M=$(PWD) SUBDIRS=$(PWD) $(EXTRA_CFLAGS) modules

.PHONY: clean
clean: clean_kernel

.PHONY: clean_kernel
clean_kernel:
	rm -rf *.o *.ko *.mod.* *.cmd .module* modules* Module* .*.cmd .tmp*
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

