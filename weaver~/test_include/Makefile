CC = gcc
CFLAGS = -Wall -O2 -I. -I../../shared -pthread
LDFLAGS = -pthread -lm

SOURCES = weaver_test.c max_mock.c ../weaver~.c ../../shared/logging.c ../../shared/crossfade.c ../../shared/visualize.c

test_runner: $(SOURCES)
	$(CC) $(CFLAGS) -o test_runner $(SOURCES) $(LDFLAGS)

run: test_runner
	./test_runner ../../sampletranscript.json

clean:
	rm -f test_runner weaver_verbose_log.txt
