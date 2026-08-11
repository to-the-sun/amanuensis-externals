CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -I../max-sdk/source/max-sdk-base/c74support/msp-includes -shared -m64 -O3
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -L../max-sdk/source/max-sdk-base/c74support/msp-includes/x64 -lMaxAPI -lMaxAudio
COMMON_SOURCES = ../max-sdk/source/max-sdk-base/c74support/max-includes/common/commonsyms.c

# Find all sound design directories
SOUND_DIRS = $(wildcard design/sounds/*)

# Helper function to format directory index to 5-digit padded string
pad = $(shell printf "%05d" $(1))

# Convert them to DLL paths in modules/ with 5-digit padding
MODULES = $(foreach dir,$(SOUND_DIRS),modules/sound_$(call pad,$(notdir $(dir))).dll)

all: sounds~.mxe64 $(MODULES)

sounds~.mxe64: sounds~.c $(COMMON_SOURCES)
	$(CC) $(CFLAGS) -o sounds~.mxe64 sounds~.c $(COMMON_SOURCES) $(LDFLAGS)

# Generate build rules for each module dynamically
define MODULE_rule
modules/sound_$(call pad,$(1)).dll: design/sounds/$(1)/sound_design.c
	@mkdir -p modules
	$$(CC) -shared -m64 -O3 -Idesign/sounds/$(1)/ -o $$@ $$< -lm
endef

$(foreach dir,$(SOUND_DIRS),$(eval $(call MODULE_rule,$(notdir $(dir)))))

clean:
	rm -f sounds~.mxe64
	rm -rf modules
