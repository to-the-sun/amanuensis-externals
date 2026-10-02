CC = gcc
CFLAGS = -O3 -I. -Wall -Wextra
LDFLAGS = -lm

TARGET = ct_analyze_headless
SOURCES = ct_analyze_headless.c ct_exporter.c cumulative_transience.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET) $(TARGET).exe

.PHONY: all clean
