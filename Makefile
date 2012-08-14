CC? = gcc

OPTIMIZATION? = -O3
DEBUG?        = -g -ggdb

CFLAGS  += -Ideps/hiredis -Ideps/libev-4.11 -Ideps/buffer -Ideps/picohttpparser $(OPTIMIZATION) $(DEBUG)

OBJS = src/redis-http.o deps/buffer/buffer.o deps/picohttpparser/picohttpparser.o
OBJS += deps/hiredis/libhiredis.a deps/libev-4.11/.libs/libev.a

redis-http: $(OBJS)
	$(CC) $(LDFLAGS) -o redis-http $^

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

deps/hiredis/libhiredis.a:
	make -C deps/hiredis static

deps/libev-4.11/.libs/libev.a:
	cd deps/libev-4.11 && ./configure --disable-shared && make

clean:
	rm -f redis-http
	rm -f src/*.o
	rm -f deps/buffer/*.o
	rm -f deps/picohttpparser/*.o
	make -C deps/hiredis clean
	make -C deps/libev-4.11 clean
