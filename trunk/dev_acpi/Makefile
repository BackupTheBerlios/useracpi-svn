ifneq ($(KERNELRELEASE),)

obj-m	:= dev_acpi.o

else

MDIR	:= /lib/modules/$(shell uname -r)
KDIR	:= $(MDIR)/build
KSRC	:= $(MDIR)/source
PWD	:= $(shell pwd)
INC	:= -I $(KSRC)/include -I $(KDIR)/include
CFLAGS	:= -O3 -Wall $(INC)

default: dev_acpi acpitree acpiundock acpivideo eventwatch acpitree-debug execute

clean:
ifneq ($(findstring 2.6,$(KDIR)),)
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean
else
	rm -f dev_acpi.o
endif
	rm -f acpitree acpiundock acpivideo eventwatch acpitree-debug execute

dev_acpi: dev_acpi.c
ifneq ($(findstring 2.6,$(KDIR)),)
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules
else
	$(CC) -D__KERNEL__ -DMODULE -I $(KDIR)/include -O2 -c dev_acpi.c
endif

install: default
ifneq ($(findstring 2.6,$(KDIR)),)
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules_install
	@echo "#################### 2.6 Kernel ################################"
	@echo "# udev should create the /dev/acpi device file when the module"
	@echo "# is loaded.  Configure udev to use the appropriate file perms."
else
	mkdir -p $(MDIR)/extra
	cp dev_acpi.o $(MDIR)/extra
	@echo "#################### 2.4 Kernel ################################"
	@echo "# The /dev/acpi device file will need to be manually created."
	@echo "# Look in the install script for an example of how to do this."


endif
	@depmod -e

endif

