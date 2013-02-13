SUBDIRS = kernel client server
SRCDIR = .
TO_TAR = ./{Makefile,README,COPYING,CREDITS,{kernel,client,server}/{*.c,*.h,Makefile},common/*.h}

all-recursive:
	for dir in $(SUBDIRS); do cd $$dir; $(MAKE) all; cd ..; done
	
all-clean:
	for dir in $(SUBDIRS); do cd $$dir; $(MAKE) clean; cd ..; done
	
all: all-recursive

clean: all-clean

tar:
	cd $(SRCDIR)
	dirname=`basename $(PWD)`; cd ..; \
	tar czfv $$dirname.tar.gz $$dirname/$(TO_TAR)
