PACKAGE = libchacha.so
CC = gcc
CFLAGS = -Wall -O3
CFLAGS += -g
CFLAGS += -Wno-maybe-uninitialized
CFLAGS += -fPIC
#LIB = /usr/local/lib/chacha
LIB = /usr/local/lib
INC = /usr/local/include/chacha
LDFLAGS = 

all: chacha-ref 

install: all 
	sudo mkdir -p ${LIB}
	sudo cp ${PACKAGE} ${LIB}
	sudo mkdir -p ${INC}
	sudo cp *.h ${INC} 
	sudo ldconfig 

chacha.o : chacha.c
	${CC} ${CFLAGS} -c chacha.c

chacha-ref : chacha.o
	$(CC) $(CFLAGS) -shared -o $(PACKAGE) $< $(LDFLAGS)	

clean:
	rm -rf *.o ${PACKAGE}
