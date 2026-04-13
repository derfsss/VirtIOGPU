CC     = ppc-amigaos-gcc
CFLAGS = -O2 -Wall -I../include
LDFLAGS = -lauto

all: test_gpu

test_gpu: test_gpu.c
	$(CC) $(CFLAGS) -o test_gpu test_gpu.c $(LDFLAGS)

clean:
	rm -f test_gpu
