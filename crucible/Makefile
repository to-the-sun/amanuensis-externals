CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -shared -m64
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -lMaxAPI

crucible.mxe64: crucible.c
	$(CC) $(CFLAGS) -o crucible.mxe64 crucible.c $(LDFLAGS)

clean:
	rm -f crucible.mxe64
