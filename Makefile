obj-m := intel_pll.o
altr_pll-y := intel_pll.o \

#ccflags-y := -DDEBUG -g -Og

SRC := $(shell pwd)

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC)

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules_install

clean:
	-rm -f *.o *~ core .depend .*.cmd *.ko *.mod.c
	-rm -f Module.markers Module.symvers modules.order
	-rm -rf .tmp_versions Modules.symvers

.PHONY:
deploy: all
	scp *.ko root@$(BOARD_IP):/home/root/
