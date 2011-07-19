all: main.c parasite-blob.h
	gcc -g -Wall -c main.c
	gcc -o parasite main.o

parasite-blob.h: parasite.c parasite.lds
	gcc -Wall -fpic -c parasite.c
	ld -T parasite.lds -o parasite.bin parasite.o
	echo 'static char parasite_blob[] = {' > parasite-blob.h
	hexdump -v -e '8/1 "0x%02x, "' -e '"\n"' parasite.bin >> parasite-blob.h
	echo -e '};' >> parasite-blob.h

clean:
	rm -f parasite parasite-blob.h *.o *.bin
