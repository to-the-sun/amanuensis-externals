CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -I../max-sdk/source/max-sdk-base/c74support/msp-includes -shared -m64
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -L../max-sdk/source/max-sdk-base/c74support/msp-includes/x64 -lMaxAPI -lMaxAudio -lws2_32

COMMON_SOURCES = ../max-sdk/source/max-sdk-base/c74support/max-includes/common/commonsyms.c
SHARED_SOURCES = ../shared/visualize.c

threads~.mxe64: threads~.c $(SHARED_SOURCES) $(COMMON_SOURCES)
	$(CC) $(CFLAGS) -o threads~.mxe64 threads~.c $(SHARED_SOURCES) $(COMMON_SOURCES) $(LDFLAGS)

clean:
	rm -f threads~.mxe64
