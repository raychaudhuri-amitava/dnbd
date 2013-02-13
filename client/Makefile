CLIENT_BIN = dnbd-client
CLIENT_SRC = client.c

BINS = $(CLIENT_BIN)

CFLAGS = -Wall -D_GNU_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -O2

$(CLIENT_BIN): 
	$(CC) $(CFLAGS) -o $@ $(CLIENT_SRC)

all: $(BINS)

.PHONY:
clean:
	-$(RM) *.o $(BINS) *~
