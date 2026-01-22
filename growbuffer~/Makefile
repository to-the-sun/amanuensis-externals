CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -I../max-sdk/source/max-sdk-base/c74support/msp-includes -shared -m64
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -L../max-sdk/source/max-sdk-base/c74support/msp-includes/x64 -lMaxAPI -lMaxAudio

growbuffer~.mxe64: growbuffer~.c ../max-sdk/source/max-sdk-base/c74support/max-includes/common/commonsyms.c
	$(CC) $(CFLAGS) -o growbuffer~.mxe64 growbuffer~.c ../max-sdk/source/max-sdk-base/c74support/max-includes/common/commonsyms.c $(LDFLAGS)

clean:
	rm -f growbuffer~.mxe64
