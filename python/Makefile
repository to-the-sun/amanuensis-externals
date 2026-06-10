CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -I../max-sdk/source/max-sdk-base/c74support/msp-includes -shared -m64 -DWIN_VERSION -DC74_X64
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -L../max-sdk/source/max-sdk-base/c74support/msp-includes/x64 -lMaxAPI -lws2_32
COMMON_SOURCES = ../max-sdk/source/max-sdk-base/c74support/max-includes/common/commonsyms.c

python.mxe64: python.c $(COMMON_SOURCES)
	$(CC) $(CFLAGS) -o python.mxe64 python.c $(COMMON_SOURCES) $(LDFLAGS)

clean:
	rm -f python.mxe64
