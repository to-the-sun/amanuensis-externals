CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -shared -m64
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -lMaxAPI -lws2_32
COMMON_SOURCES = ../max-sdk/source/max-sdk-base/c74support/max-includes/common/commonsyms.c
SHARED_SOURCES = ../shared/visualize.c

whichoffset.mxe64: whichoffset.c $(SHARED_SOURCES) $(COMMON_SOURCES)
	$(CC) $(CFLAGS) -o whichoffset.mxe64 whichoffset.c $(SHARED_SOURCES) $(COMMON_SOURCES) $(LDFLAGS)

clean:
	rm -f whichoffset.mxe64
