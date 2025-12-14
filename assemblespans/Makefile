CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -shared -m64
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -lMaxAPI

assemblespans.mxe64: assemblespans.c
	$(CC) $(CFLAGS) -o assemblespans.mxe64 assemblespans.c $(LDFLAGS)

clean:
	rm -f assemblespans.mxe64
