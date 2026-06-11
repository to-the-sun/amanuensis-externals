CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -I../max-sdk/source/max-sdk-base/c74support/msp-includes -shared -m64 -O3
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -L../max-sdk/source/max-sdk-base/c74support/msp-includes/x64 -lMaxAPI -lMaxAudio
COMMON_SOURCES = ../max-sdk/source/max-sdk-base/c74support/max-includes/common/commonsyms.c

# Find all sound design directories
SOUND_DIRS = $(wildcard design/sounds/*)
# Convert them to DLL paths in modules/
MODULES = $(patsubst design/sounds/%, modules/sound_%.dll, $(SOUND_DIRS))

all: sounds~.mxe64 $(MODULES)

sounds~.mxe64: sounds~.c $(COMMON_SOURCES)
	$(CC) $(CFLAGS) -o sounds~.mxe64 sounds~.c $(COMMON_SOURCES) $(LDFLAGS)

# Rule to build each module DLL
modules/sound_%.dll: design/sounds/%/sound_design.c
	@mkdir -p modules
	$(CC) -shared -m64 -O3 -I$(dir $<) -o $@ $< -lm

clean:
	rm -f sounds~.mxe64
	rm -rf modules
