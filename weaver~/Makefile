CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -I../max-sdk/source/max-sdk-base/c74support/msp-includes -shared -m64
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -L../max-sdk/source/max-sdk-base/c74support/msp-includes/x64 -lMaxAPI -lMaxAudio
COMMON_SOURCES = ../max-sdk/source/max-sdk-base/c74support/max-includes/common/commonsyms.c

weaver~.mxe64: weaver~.c ../shared/visualize.c ../shared/logging.c $(COMMON_SOURCES)
	$(CC) $(CFLAGS) -o weaver~.mxe64 weaver~.c ../shared/visualize.c ../shared/logging.c $(COMMON_SOURCES) $(LDFLAGS) -lws2_32

clean:
	rm -f weaver~.mxe64
