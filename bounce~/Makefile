CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -I../max-sdk/source/max-sdk-base/c74support/msp-includes -shared -m64
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -L../max-sdk/source/max-sdk-base/c74support/msp-includes/x64 -lMaxAPI -lMaxAudio
COMMON_SOURCES = ../max-sdk/source/max-sdk-base/c74support/max-includes/common/commonsyms.c

bounce~.mxe64: bounce~.c ../shared/logging.c ../shared/crossfade.c $(COMMON_SOURCES)
	$(CC) $(CFLAGS) -o bounce~.mxe64 bounce~.c ../shared/logging.c ../shared/crossfade.c $(COMMON_SOURCES) $(LDFLAGS)

clean:
	rm -f bounce~.mxe64
