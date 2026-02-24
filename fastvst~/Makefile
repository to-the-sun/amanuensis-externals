CC = x86_64-w64-mingw32-gcc
CFLAGS = -DWIN_VERSION -DC74_X64 -I../max-sdk/source/max-sdk-base/c74support/max-includes -I../max-sdk/source/max-sdk-base/c74support/msp-includes -shared -m64
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -L../max-sdk/source/max-sdk-base/c74support/msp-includes/x64 -lMaxAPI -lMaxAudio
COMMON_SOURCES = ../max-sdk/source/max-sdk-base/c74support/max-includes/common/commonsyms.c

fastvst~.mxe64: fastvst~.c $(COMMON_SOURCES)
	$(CC) $(CFLAGS) -o "fastvst~.mxe64" "fastvst~.c" $(COMMON_SOURCES) $(LDFLAGS)

clean:
	rm -f "fastvst~.mxe64"
