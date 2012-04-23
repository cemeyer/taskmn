LIB=		taskmn
CFLAGS=		-g -O2 -Wall
NO_MAN=		1

SRCS=		asm.S context.c fd.c net.c rendez.c task.c
BINS=		asm.o context.o fd.o net.o rendez.o task.o

INCS=		taskmn.h

INCLUDEDIR=	/usr/include

lib$(LIB).a: $(BINS)
	ar rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -I/usr/include -I. $(SRCS) -lrt -lpthread -c $<

clean:
	rm -f *.o libtaskmn.a
