HEADERS = osmo-fl2k.h
INCLUDES=-I/usr/include/libusb-1.0/ -I/usr/local/Cellar/libusb/1.0.22/include/libusb-1.0/
LDFLAGS=-lusb-1.0 -pthread -lm

default: vgaplay

vgaplay.o: vgaplay.c $(HEADERS)
	gcc -c vgaplay.c -o vgaplay.o $(INCLUDES)

libosmo-fl2k.o: libosmo-fl2k.c $(HEADERS)
	gcc -c libosmo-fl2k.c -o libosmo-fl2k.o  $(INCLUDES)

vgaplay: vgaplay.o libosmo-fl2k.o
	gcc -ggdb vgaplay.o libosmo-fl2k.o -o vgaplay $(LDFLAGS)

clean:
	-rm -f vgaplay.o
	-rm -f libosmo-fl2k.o
	-rm -f vgaplay
	-rm -f modulatedFile.o

modulatedFile.o: modulatedFile.c
	gcc modulatedFile.c -c

modulatedFile: modulatedFile.o
	gcc modulatedFile.o -o modulatedFile -lm
