CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -shared -m64
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -lMaxAPI

COMMON_SOURCES = ../max-sdk/source/max-sdk-base/c74support/max-includes/common/commonsyms.c

stemversion.mxe64: stemversion.c $(COMMON_SOURCES)
	$(CC) $(CFLAGS) -o stemversion.mxe64 stemversion.c $(COMMON_SOURCES) $(LDFLAGS)

clean:
	rm -f stemversion.mxe64
