HEADERS = osmo-fl2k.h
INCLUDES=-I/usr/include/libusb-1.0/
LDFLAGS=-lusb-1.0 -pthread -lm

default: vgaplay

vgaplay.o: vgaplay.c $(HEADERS)
	gcc -c vgaplay.c -o vgaplay.o $(INCLUDES)
	
libosmo-fl2k.o: libosmo-fl2k.c $(HEADERS)
	gcc -c libosmo-fl2k.c -o libosmo-fl2k.o  $(INCLUDES)

vgaplay: vgaplay.o libosmo-fl2k.o
	gcc vgaplay.o libosmo-fl2k.o -o vgaplay $(LDFLAGS)

clean:
	-rm -f vgaplay.o
	-rm -f vgaplay
