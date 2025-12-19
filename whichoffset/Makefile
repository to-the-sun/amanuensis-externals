CC = x86_64-w64-mingw32-gcc
CFLAGS = -I../max-sdk/source/max-sdk-base/c74support/max-includes -shared -m64
LDFLAGS = -L../max-sdk/source/max-sdk-base/c74support/max-includes/x64 -lMaxAPI -lws2_32
OBJS = whichoffset.o ../shared/visualize.o

whichoffset.mxe64: $(OBJS)
	$(CC) $(CFLAGS) -o ../externals/whichoffset.mxe64 $(OBJS) $(LDFLAGS)

whichoffset.o: whichoffset.c
	$(CC) $(CFLAGS) -c whichoffset.c -o whichoffset.o

../shared/visualize.o: ../shared/visualize.c
	$(CC) $(CFLAGS) -c ../shared/visualize.c -o ../shared/visualize.o

clean:
	rm -f ../externals/whichoffset.mxe64 *.o ../shared/*.o
