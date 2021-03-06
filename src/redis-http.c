#include <stdio.h>

#include <ev.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <stddef.h>
#include <string.h>
#include <sys/uio.h>
#include <signal.h>

#include "hiredis.h"
#include "async.h"
#include "adapters/libev.h"
#include "sds.h"

#include "ngx-queue.h"
#include "buffer.h"
#include "picohttpparser.h"

/* default options */
static uint16_t http_port;
static sds http_address;
static sds http_socket;
static uint16_t redis_port;
static sds redis_address;
static sds redis_socket;

typedef struct http_server_s http_server_t;
typedef struct http_conn_s http_conn_t;

/* global server */
static http_server_t* instance;

struct http_server_s {
    int fd;
    ngx_queue_t connections;
    ev_io ev_read;
    ev_timer reconnect_timer;

    int closing;
    void* data;
};

static const int HTTP_CONN_ERR        = 1 << 0;
static const int HTTP_CONN_WAIT_REDIS = 1 << 1;

struct http_conn_s {
    int fd;
    ngx_queue_t queue;
    ev_io ev_read;
    buffer* rbuf;

    int flags;

    http_server_t* server;
};

static http_conn_t* http_conn_init(int fd);
static void http_conn_close(http_conn_t* conn);

static void redis_connect_cb(const redisAsyncContext* c, int status);
static void redis_disconnect_cb(const redisAsyncContext* c, int status);
static void redis_reconnect(http_server_t* server);

static const char* const BAD_REQUEST =
    "HTTP/1.0 400 Bad Request\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 11\r\n"
    "\r\n"
    "Bad Request";
static const size_t BAD_REQUEST_LEN = 85;

static const char* const NOT_FOUND =
    "HTTP/1.0 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 9\r\n"
    "\r\n"
    "Not Found";
static const size_t NOT_FOUND_LEN = 80;

static const char* const BAD_GATEWAY =
    "HTTP/1.0 502 Bad Gateway\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 11\r\n"
    "\r\n"
    "Bad Gateway";
static const size_t BAD_GATEWAY_LEN = 85;

static const char* const OK_HDR =
    "HTTP/1.0 200 OK\r\n";
static const size_t OK_HDR_LEN = 17;

void usage() {
    fprintf(stderr,"Usage: ./redis-http --port 7777 --redis-port 8888\n");
    exit(1);
}

static redisAsyncContext* redis_connect(void) {
    redisAsyncContext* c;
    if (redis_socket) {
        c = redisAsyncConnectUnix(redis_socket);
    }
    else {
        c = redisAsyncConnect(redis_address, redis_port);
    }
    if (c->err) {
        fprintf(stderr, "Failed to connect redis server %s:%d: %s\n",
            redis_address, redis_port, c->errstr);
        redisAsyncFree(c);
        return NULL;
    }

    redisLibevAttach(EV_DEFAULT_ c);
    redisAsyncSetConnectCallback(c, redis_connect_cb);
    redisAsyncSetDisconnectCallback(c, redis_disconnect_cb);

    return c;
}

static void redis_reconnect_cb(EV_P_ ev_timer* w, int revents) {
    ev_timer_stop(EV_A_ w);

    http_server_t* server = (http_server_t*)
        (((char*)w) - offsetof(http_server_t, reconnect_timer));

    redisAsyncContext* c = redis_connect();
    if (c) {
        c->data = (void*)server;
    }
    else {
        redis_reconnect(server);
    }
}

static void redis_reconnect(http_server_t* server) {
    ev_timer_set(&server->reconnect_timer, 2., 0.);
    ev_timer_start(EV_DEFAULT_ &server->reconnect_timer);
}

static void redis_connect_cb(const redisAsyncContext* c, int status) {
    http_server_t* server = (http_server_t*)c->data;

    if (status != REDIS_OK) {
        fprintf(stderr, "redis connect error: %s\n", c->errstr);
        redis_reconnect(server);
        return;
    }
    printf("Connected redis-server (%s:%d)\n", redis_address, redis_port);

    server->data = (void*)c;
}

static void redis_disconnect_cb(const redisAsyncContext* c, int status) {
    if (status != REDIS_OK) {
        fprintf(stderr, "redis error: %d %s\n", status, c->errstr);
    }
    else {
        fprintf(stderr, "disconnected from redis\n");
    }

    http_server_t* server = (http_server_t*)c->data;
    server->data = NULL;

    if (0 == server->closing)
        redis_reconnect(server);
}

static void redis_data_cb(redisAsyncContext* c, void* r, void* privdata) {
    redisReply* reply = r;
    http_conn_t* conn = (http_conn_t*)privdata;

    conn->flags = conn->flags ^ HTTP_CONN_WAIT_REDIS;

    if (conn->flags & HTTP_CONN_ERR) {
        http_conn_close(conn);
        return;
    }

    if (reply == NULL) {
        return;
    }

    if (conn == NULL) {
        fprintf(stderr, "invalid privdata\n");
        return;
    }

    if (0 == reply->len) {
        write(conn->fd, NOT_FOUND, NOT_FOUND_LEN);
    }
    else {
        struct iovec v[3];
        v[0].iov_base = (char*)OK_HDR;
        v[0].iov_len  = OK_HDR_LEN;

        char content_length[64];
        snprintf(content_length, 64, "Content-Length: %d\r\n\r\n", reply->len);
        v[1].iov_base = content_length;
        v[1].iov_len  = strlen(content_length);

        v[2].iov_base = reply->str;
        v[2].iov_len  = reply->len;

        writev(conn->fd, v, 3);
    }
    http_conn_close(conn);
}

static void setup_sock(int fd) {
    int on = 1, r;

    if (NULL == http_socket) {
        r = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        assert(r == 0);
    }
    r = fcntl(fd, F_SETFL, O_NONBLOCK);
    assert(r == 0);
}

static void http_conn_read_cb(EV_P_ ev_io* w, int revents) {
    http_conn_t* conn = (http_conn_t*)
        (((char*)w) - offsetof(http_conn_t, ev_read));
    //http_server_t* server = conn->server;

    char buf[1024];
    ssize_t r = read(w->fd, buf, 1024);

    if (0 == r) {
        /* connection closed by peer */
#ifdef DEBUG
        fprintf(stderr, "connection closed by peer: %d\n", w->fd);
#endif
        conn->flags = conn->flags | HTTP_CONN_ERR;
        http_conn_close(conn);
        return;
    }
    else if (-1 == r) {
        /* error */
        if (EAGAIN == errno || EWOULDBLOCK == errno) { /* try again later */
            return;
        }
        else { /* fatal error */
#ifdef DEBUG
            fprintf(stderr, "fatal error: %d, %s\n", errno, strerror(errno));
#endif
            conn->flags = conn->flags | HTTP_CONN_ERR;
            http_conn_close(conn);
            return;
        }
    }
    else { /* got some data */
        buffer_append_string_len(conn->rbuf, buf, r);

        const char* method;
        const char* path;
        size_t method_len, path_len;
        int minor_version;
        size_t num_headers = 20;
        struct phr_header headers[num_headers];

        r = phr_parse_request(conn->rbuf->ptr, conn->rbuf->used, &method, &method_len,
            &path, &path_len, &minor_version, headers, &num_headers, 0);

        if (r >= 0) {
            if (0 == strncmp(method, "GET", method_len) && path_len > 1) {
                redisAsyncContext* c = (redisAsyncContext*)conn->server->data;
                if (c) {
                    redisAsyncCommand(c, redis_data_cb, conn, "GET %b", path + 1, path_len - 1);
                    conn->flags = conn->flags | HTTP_CONN_WAIT_REDIS;
                }
                else {
                    write(conn->fd, BAD_GATEWAY, BAD_GATEWAY_LEN);
                    conn->flags = conn->flags | HTTP_CONN_ERR;
                    http_conn_close(conn);
                }
            }
            else {
                write(conn->fd, BAD_REQUEST, BAD_REQUEST_LEN);
                conn->flags = conn->flags | HTTP_CONN_ERR;
                http_conn_close(conn);
            }
        }
        else if (-2 == r) {
            /* partial */
            return;
        }
        else if (-1 == r) {
            write(conn->fd, BAD_REQUEST, BAD_REQUEST_LEN);
            conn->flags = conn->flags | HTTP_CONN_ERR;
            http_conn_close(conn);
        }
    }
}

static void http_acccept_cb(EV_P_ ev_io* w, int revents) {
    int newfd = accept(w->fd, NULL, NULL);
    if (0 >= newfd) {
        fprintf(stderr, "accept failed: %d, %s\n", errno, strerror(errno));
        return;
    }

    setup_sock(newfd);

    http_server_t* server = (http_server_t*)
        (((char*)w) - offsetof(http_server_t, ev_read));

    http_conn_t* conn = http_conn_init(newfd);
    ngx_queue_insert_tail(&server->connections, &conn->queue);
    conn->server = server;

    ev_io_init(&conn->ev_read, http_conn_read_cb, newfd, EV_READ);
    ev_io_start(EV_A_ &conn->ev_read);

#ifdef DEBUG
    fprintf(stderr, "new connection: %d\n", newfd);
#endif
}

static http_server_t* http_server_init() {
    http_server_t* server = malloc(sizeof(http_server_t));
    assert(server);

    server->fd = 0;
    server->closing = 0;
    ngx_queue_init(&server->connections);

    ev_timer_init(&server->reconnect_timer, redis_reconnect_cb, 2., 0.);

    return server;
}

static void http_server_free(http_server_t* server) {
    free(server);
}

static http_conn_t* http_conn_init(int fd) {
    http_conn_t* conn = malloc(sizeof(http_conn_t));
    assert(conn);

    conn->fd = fd;
    ngx_queue_init(&conn->queue);
    conn->rbuf  = buffer_init();
    conn->flags = 0;

    return conn;
}

static void http_conn_close(http_conn_t* conn) {
    ev_io_stop(EV_DEFAULT_ &conn->ev_read);

    if (conn->flags & HTTP_CONN_WAIT_REDIS) return;
#ifdef DEBUG
    fprintf(stderr, "close conn: %d\n", conn->fd);
#endif

    http_server_t* server = conn->server;

    ngx_queue_remove(&conn->queue);
    close(conn->fd);
    buffer_free(conn->rbuf);
    free(conn);

    if (server->closing && ngx_queue_empty(&server->connections)) {
        /* stop server */
        fprintf(stderr, "stopping server\n");
        redisAsyncContext* c = (redisAsyncContext*)server->data;
        if (c) {
            redisAsyncFree(c);
        }

        ev_io_stop(EV_DEFAULT_ &server->ev_read);
        ev_timer_stop(EV_DEFAULT_ &server->reconnect_timer);
        close(server->fd);
    }
}

static void http_server_listen(http_server_t* server) {
    int listen_sock = 0, r, flag = 1;

    sds ports = sdsnew(getenv("SERVER_STARTER_PORT"));
    if (sdslen(ports)) {
        int count, pair_count;
        sds* pairs = sdssplitlen(ports, sdslen(ports), ";", 1, &count);
        int i;
        for (i = 0; i < count; i++) {
            sds* port_fd = sdssplitlen(pairs[i], sdslen(pairs[i]), "=", 1, &pair_count);
            if (pair_count < 2) {
                sdsfreesplitres(port_fd, pair_count);
                continue;
            }

            int host_port_count;
            sds* host_port = sdssplitlen(port_fd[0], sdslen(port_fd[0]),
                ":", 1, &host_port_count);

            if (host_port_count >= 2) {
                sdsfree(http_address);
                http_address = sdsnew(host_port[ host_port_count - 1 - 1 ]);
                http_port    = atoi(host_port[ host_port_count - 1 ]);
            }
            else {
                sdsfree(http_address);
                http_address = sdsnew("0.0.0.0");
                http_port    = atoi(port_fd[0]);
                if (0 == http_port) {
                    if (http_socket) sdsfree(http_socket);
                    http_socket = sdsnew(port_fd[0]);
                }
            }
            sdsfreesplitres(host_port, host_port_count);

            listen_sock = atoi(port_fd[1]);

            sdsfreesplitres(port_fd, pair_count);
            break;
        }
        sdsfreesplitres(pairs, count);
    }
    sdsfree(ports);

    if (0 == listen_sock) {
        if (http_socket) {
            listen_sock = socket(AF_UNIX, SOCK_STREAM, 0);
            assert(-1 != listen_sock);

            r = setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
            assert(0 == r);

            struct sockaddr_un listen_addr;
            memset(&listen_addr, 0, sizeof(listen_addr));
            strncpy(listen_addr.sun_path, http_socket, sizeof(listen_addr.sun_path) - 1);
            listen_addr.sun_family = AF_UNIX;
            unlink(listen_addr.sun_path);
            r = bind(listen_sock, (struct sockaddr*)&listen_addr, sizeof(listen_addr));
            if (r) {
                fprintf(stderr, "bind failed: %d, %s\n", errno, strerror(errno));
            }
            assert(0 == r);
        }
        else {
            listen_sock = socket(AF_INET, SOCK_STREAM, 0);
            assert(-1 != listen_sock);

            r = setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
            assert(0 == r);

            struct sockaddr_in listen_addr;
            listen_addr.sin_family      = AF_INET;
            listen_addr.sin_port        = htons(http_port);
            listen_addr.sin_addr.s_addr = 0; /* ANY */
            r = bind(listen_sock, (struct sockaddr*)&listen_addr, sizeof(listen_addr));
            assert(0 == r);
        }
    }

    r = listen(listen_sock, 128);
    assert(0 == r);

    setup_sock(listen_sock);

    server->fd = listen_sock;
    ev_io_init(&server->ev_read, http_acccept_cb, listen_sock, EV_READ);
    ev_io_start(EV_DEFAULT_ &server->ev_read);
}

static void sigterm_cb(EV_P_ ev_async* w, int revents) {
    fprintf(stderr, "here\n");
    http_server_t* server = (http_server_t*)w->data;
    server->closing = 1;
}

static void sigtermHandler(int sig) {
    printf("Received SIGTERM, scheduling shutdown...\n");
    http_server_t* s = instance;
    s->closing = 1;

    if (ngx_queue_empty(&s->connections)) {
        redisAsyncContext* c = (redisAsyncContext*)s->data;
        if (c) redisAsyncFree(c);

        ev_io_stop(EV_DEFAULT_ &s->ev_read);
        close(s->fd);
    }
}

int main(int argc, char** argv) {
    http_port     = 6380;
    http_address  = sdsnew("0.0.0.0");
    http_socket   = NULL;
    redis_port    = 6379;
    redis_address = sdsnew("127.0.0.1");
    redis_socket  = NULL;

    if (argc >= 2) {
        int j = 1;
        sds option = sdsempty();

        /* Handle special options --help */
        if (strcmp(argv[1], "--help") == 0 ||
            strcmp(argv[1], "-h") == 0) usage();

        while (j != argc) {
            if (argv[j][0] == '-' && argv[j][1] == '-') {
                /* Option name */
                if (sdslen(option)) {
                    fprintf(stderr, "Argument missing for option %s\n", option);
                    usage();
                }
                option = sdscat(option, argv[j] + 2);
            }
            else {
                /* Option argument */
                if (!sdslen(option)) {
                    fprintf(stderr, "Invalid arguments: %s\n", argv[j]);
                    usage();
                }

                if (0 == strcmp(option, "port")) {
                    http_port = atoi(argv[j]);
                }
                else if (0 == strcmp(option, "address")) {
                    sdsfree(http_address);
                    http_address = sdsnew(argv[j]);
                }
                else if (0 == strcmp(option, "socket")) {
                    http_socket = sdsnew(argv[j]);
                }
                else if (0 == strcmp(option, "redis-port")) {
                    redis_port = atoi(argv[j]);
                }
                else if (0 == strcmp(option, "redis-address")) {
                    sdsfree(redis_address);
                    redis_address = sdsnew(argv[j]);
                }
                else if (0 == strcmp(option, "redis-socket")) {
                    redis_socket = sdsnew(argv[j]);
                }
                else {
                    fprintf(stderr, "Unknown option: %s\n", option);
                }

                sdsfree(option);
                option = sdsempty();
            }
            j++;
        }
    }

    /* redis client */
    redisAsyncContext* c = redis_connect();
    if (NULL == c) {
        return -1;
    }

    /* http server */
    http_server_t* server = http_server_init();
    assert(server);

    http_server_listen(server);

    c->data = (void*)server;
    instance = server;

    if (http_socket) {
        printf("Launched redis-http (unix:%s) ", http_socket);
    }
    else {
        printf("Launched redis-http (%s:%d) ", http_address, http_port);
    }
    if (redis_socket) {
        printf("proxying redis (unix:%s)\n", redis_socket);
    }
    else {
        printf("proxying redis (%s:%d)\n", redis_address, redis_port);
    }

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigtermHandler;
    sigaction(SIGTERM, &act, NULL);

    /* main loop */
    ev_loop(EV_DEFAULT_ 0);

    http_server_free(server);

    sdsfree(http_address);
    sdsfree(redis_address);
    if (http_socket) sdsfree(http_socket);
    if (redis_socket) sdsfree(redis_socket);

    return 0;
}
