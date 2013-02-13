KDIR	:= /lib/modules/$(shell uname -r)/build

# change a line according to your needs, 
# if you want to build for another kernel
#KDIR	:= /lib/modules/2.6.12-usermode-r1-bs5/build
#KDIR	:= /lib/modules/2.6.13-15.7-smp/build

PWD	:= $(shell pwd)


obj-m += dnbd.o
dnbd-objs := queue.o cache.o net.o main.o 

all:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	-$(RM) *.o dnbd.ko *~
