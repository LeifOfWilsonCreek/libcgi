all: libcgi.a

libcgi.a: cgi.o
	rm -f libcgi.a
	ar cq libcgi.a cgi.o
	ranlib libcgi.a

cgi.o: cgi.h cgi.c
#	$(CC) -O2 -fpic -Wall -c cgi.c
	$(CC) -O2 -r -Wall -c cgi.c

example.cgi: libcgi.a example.cgi.c
	$(CC) -O2 -Wall -o example.cgi example.cgi.c libcgi.a

install: libcgi.a cgi.h
	install -m 0644 -o root -g wheel cgi.h /usr/include
	install -m 0644 -o root -g wheel libcgi.a /usr/lib

clean:
	rm -f libcgi.a cgi.o example.cgi *.core core
