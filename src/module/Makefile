KDIR = /lib/modules/$(shell uname -r)/build
obj-m += ums.o

all:
	make CC="ccache gcc" -C $(KDIR) M=$(PWD) modules

clean:
	make CC="ccache gcc" -C $(KDIR) M=$(PWD) clean