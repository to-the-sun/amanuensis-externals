CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -shared -m64
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -lMaxAPI

buildspans.mxe64: buildspans.c ../shared/visualize.c
	$(CC) $(CFLAGS) -o buildspans.mxe64 buildspans.c ../shared/visualize.c $(LDFLAGS) -lws2_32

clean:
	rm -f buildspans.mxe64
