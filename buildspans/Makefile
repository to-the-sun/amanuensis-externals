CC = x86_64-w64-mingw32-gcc
CFLAGS = -I/app/max-sdk/source/max-sdk-base/c74support/max-includes -I/app/max-sdk/source/max-sdk-base/c74support/msp-includes -shared -m64
LDFLAGS = -L/app/max-sdk/source/max-sdk-base/c74support/max-includes/x64 -L/app/max-sdk/source/max-sdk-base/c74support/msp-includes/x64 -lMaxAPI -lMaxAudio
COMMON_SOURCES = /app/max-sdk/source/max-sdk-base/c74support/max-includes/common/commonsyms.c

buildspans.mxe64: buildspans.c ../crucible/crucible.c ../shared/visualize.c ../shared/logging.c ../shared/async_worker.c $(COMMON_SOURCES)
	$(CC) $(CFLAGS) -DNO_EXT_MAIN -o buildspans.mxe64 buildspans.c ../crucible/crucible.c ../shared/visualize.c ../shared/logging.c ../shared/async_worker.c $(COMMON_SOURCES) $(LDFLAGS) -lws2_32

clean:
	rm -f buildspans.mxe64
