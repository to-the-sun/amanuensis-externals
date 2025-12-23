CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -shared -m64
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -lMaxAPI -lws2_32

buildspans.mxe64: buildspans.c
	$(CC) $(CFLAGS) -o buildspans.mxe64 buildspans.c ../shared/visualize.o $(LDFLAGS)

clean:
	rm -f buildspans.mxe64
