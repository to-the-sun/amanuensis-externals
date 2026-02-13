CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -I../max-sdk/source/max-sdk-base/c74support/msp-includes -shared -m64
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -L../max-sdk/source/max-sdk-base/c74support/msp-includes/x64 -lMaxAPI -lMaxAudio
COMMON_SOURCES = ../max-sdk/source/max-sdk-base/c74support/max-includes/common/commonsyms.c

buildspans.mxe64: buildspans.c ../shared/visualize.c $(COMMON_SOURCES)
	$(CC) $(CFLAGS) -o buildspans.mxe64 buildspans.c ../shared/visualize.c $(COMMON_SOURCES) $(LDFLAGS) -lws2_32

clean:
	rm -f buildspans.mxe64
