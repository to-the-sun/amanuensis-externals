CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -I../max-sdk/source/max-sdk-base/c74support/msp-includes -shared -m64 -DWIN_VERSION -DC74_X64
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -L../max-sdk/source/max-sdk-base/c74support/msp-includes/x64 -lMaxAPI -lMaxAudio -lws2_32
COMMON_SOURCES = ../max-sdk/source/max-sdk-base/c74support/max-includes/common/commonsyms.c

arrangement~.mxe64: arrangement~.c ../shared/logging.c ../shared/crossfade.c $(COMMON_SOURCES)
	$(CC) $(CFLAGS) -o arrangement~.mxe64 arrangement~.c ../shared/logging.c ../shared/crossfade.c $(COMMON_SOURCES) $(LDFLAGS)

clean:
	rm -f arrangement~.mxe64
