CC? = gcc

OPTIMIZATION? = -O3
DEBUG?        = -g -ggdb

CFLAGS  += -Ideps/hiredis -Ideps/buffer -Ideps/picohttpparser $(OPTIMIZATION) $(DEBUG)
LDFLAGS += -lev

OBJS = src/redis-http.o deps/buffer/buffer.o deps/picohttpparser/picohttpparser.o

redis-http: deps/hiredis/libhiredis.a $(OBJS)
	$(CC) $(LDFLAGS) -o redis-http $^

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

deps/hiredis/libhiredis.a:
	make -C deps/hiredis

clean:
	rm -f redis-http
	rm -f src/*.o
	rm -f deps/buffer/*.o
	rm -f deps/picohttpparser/*.o
	make -C deps/hiredis clean
