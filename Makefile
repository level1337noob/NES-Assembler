TOOLCHAIN_PREFIX ?= x86_64-linux-gnu-

CC := g++
CFLAGS += -Wall -fno-asynchronous-unwind-tables -std=c++11 \
	-fno-asm -finline-functions -fuse-cxa-atexit -pipe \
	-O0 -fbuiltin -march=native -fPIC -I. \
	-mabi=sysv -fpermissive -fasm

OBJS += main.o


prog: all
	$(CC) $(CFLAGS) -o $@ $(OBJS)
	rm $(OBJS)
all: $(OBJS)
$(OBJS): %.o : %.cpp
	$(CC) $(CFLAGS) -c $< -o $*.o
