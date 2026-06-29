CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -I../max-sdk/source/max-sdk-base/c74support/msp-includes -shared -m64 -O3
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -L../max-sdk/source/max-sdk-base/c74support/msp-includes/x64 -lMaxAPI -lMaxAudio
# Use local max-sdk paths if present
IF_LOCAL_SDK = $(wildcard ../max-sdk/source/max-sdk-base/c74support/max-includes/common/commonsyms.c)
ifeq ($(IF_LOCAL_SDK),)
	SDK_PATH = ./max-sdk/source/max-sdk-base/c74support
	CFLAGS := -I$(SDK_PATH)/max-includes -I$(SDK_PATH)/msp-includes -shared -m64 -O3
	LDFLAGS := -L$(SDK_PATH)/max-includes/x64 -L$(SDK_PATH)/msp-includes/x64 -lMaxAPI -lMaxAudio
	COMMON_SOURCES = $(SDK_PATH)/max-includes/common/commonsyms.c
else
	COMMON_SOURCES = ../max-sdk/source/max-sdk-base/c74support/max-includes/common/commonsyms.c
endif
SHARED_SOURCES = ../shared/async_worker.c

all: analyze~.mxe64

analyze~.mxe64: analyze~.c cumulative_transience.c $(COMMON_SOURCES) $(SHARED_SOURCES)
	$(CC) $(CFLAGS) -o analyze~.mxe64 analyze~.c cumulative_transience.c $(COMMON_SOURCES) $(SHARED_SOURCES) $(LDFLAGS)

clean:
	rm -f analyze~.mxe64
