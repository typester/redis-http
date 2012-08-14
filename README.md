redis-http
=======================

redis-http is a tiny HTTP interface for [Redis](http://redis.io/).

Features
-----------------------

 * Only `GET` cmd is supported.
 * Graceful shutdown by SIGTERM.
 * Hot-deploy by using [start_server](http://search.cpan.org/dist/Server-Starter/start_server)

Build Instructions
-----------------------

    $ git clone git://github.com/typester/redis-http.git
    $ cd redis-http
    $ git submodule update --init --recursive
    $ make

Usage
-----------------------

    $ ./redis-http --port 6380 --redis-address 127.0.0.1 --redis-port 6379

This launch http-server on port 6380 that gateway request to redis on 127.0.0.1:6379

To get redis data, just simply do http request like:

    $ curl http://127.0.0.1:6380/foo

This is equivalent to `GET foo` on redis-cli.


Hot-deploy by using start_server
---------------------------------

redis-http also supports Hot-deployment by using [start_server](http://search.cpan.org/dist/Server-Starter/start_server).

To use this feature, boot redis-http just like:

    $ start_server --port 6380 -- ./redis-http --redis-address 127.0.0.1 --redis-port 6379

After this, you can hot-deploy new redis-http binary by sending HUP signal to start_server process.


Performance
---------------------------------

redis-http is very simple application based libev, hiredis, picohttpparser and acts quite fast.

Here is some `ab` testing to redis-http and some similar projects:

 * [webdis](http://webd.is/) - Another redis http interface has more rich and poweful features.
 * [Kyoto Tycoon](http://fallabs.com/kyototycoon/) - Not redis application, but this is KVS based HTTP interface.

### ab -c 100 -n 100000

redis-http:

    Document Path:          /foo
    Document Length:        3 bytes
    
    Concurrency Level:      100
    Time taken for tests:   8.563 seconds
    Complete requests:      100000
    Failed requests:        0
    Write errors:           0
    Total transferred:      4100000 bytes
    HTML transferred:       300000 bytes
    Requests per second:    11678.21 [#/sec] (mean)
    Time per request:       8.563 [ms] (mean)
    Time per request:       0.086 [ms] (mean, across all concurrent requests)
    Transfer rate:          467.58 [Kbytes/sec] received

Webdis:

    Concurrency Level:      100
    Time taken for tests:   12.671 seconds
    Complete requests:      100000
    Failed requests:        0
    Write errors:           0
    Total transferred:      23800000 bytes
    HTML transferred:       300000 bytes
    Requests per second:    7891.95 [#/sec] (mean)
    Time per request:       12.671 [ms] (mean)
    Time per request:       0.127 [ms] (mean, across all concurrent requests)
    Transfer rate:          1834.26 [Kbytes/sec] received


Kyoto Tycoon:

    Document Path:          /foo
    Document Length:        3 bytes
    
    Concurrency Level:      100
    Time taken for tests:   10.377 seconds
    Complete requests:      100000
    Failed requests:        0
    Write errors:           0
    Total transferred:      12500000 bytes
    HTML transferred:       300000 bytes
    Requests per second:    9636.47 [#/sec] (mean)
    Time per request:       10.377 [ms] (mean)
    Time per request:       0.104 [ms] (mean, across all concurrent requests)
    Transfer rate:          1176.33 [Kbytes/sec] received




