SERVER_BIN = dnbd-server
SERVER_SRC = filer.c net.c query.c server.c

BINS = $(SERVER_BIN)

CFLAGS = -Wall -D_GNU_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -O2
LDFLAGS = -lpthread

$(SERVER_BIN): 
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRC) $(LDFLAGS)

all: $(BINS)

.PHONY:
clean:
	-$(RM) *.o $(BINS) *~
