#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
    #define close closesocket
    #define sleep Sleep
#else
    #include <netdb.h>
    #include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include "client.h"
#include "tinycthread.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define RECV_SIZE 4096
#endif

#define QUEUE_SIZE 1048576

static bool client_enabled = false;
static bool running = 0;
static int sd = 0;
static int bytes_sent = 0;
#ifndef __EMSCRIPTEN__
static int bytes_received = 0;
static mtx_t mutex;
static thrd_t recv_thread;
#endif
static char *queue = 0;
static int qsize = 0;

void client_enable() {
    client_enabled = true;
}

void client_disable() {
    client_enabled = false;
}

bool get_client_enabled() {
    return client_enabled;
}

int client_sendall(int sd, char *data, int length) {
    if (!client_enabled) {
        return 0;
    }
    int count = 0;
    while (count < length) {
        int n = send(sd, data + count, length, 0);
        if (n == -1) {
            return -1;
        }
        count += n;
        length -= n;
        bytes_sent += n;
    }
    return 0;
}

void client_send(char *data) {
    if (!client_enabled) {
        return;
    }
    if (client_sendall(sd, data, strlen(data)) == -1) {
        perror("client_sendall");
        exit(1);
    }
}

void client_version(int version) {
    if (!client_enabled) {
        return;
    }
    char buffer[1024];
    snprintf(buffer, 1024, "V,%d\n", version);
    client_send(buffer);
}

void client_login(const char *username, const char *identity_token) {
    if (!client_enabled) {
        return;
    }
    char buffer[1024];
    snprintf(buffer, 1024, "A,%s,%s\n", username, identity_token);
    client_send(buffer);
}

void client_position(float x, float y, float z, float rx, float ry) {
    if (!client_enabled) {
        return;
    }
    static float px, py, pz, prx, pry = 0;
    float distance =
        (px - x) * (px - x) +
        (py - y) * (py - y) +
        (pz - z) * (pz - z) +
        (prx - rx) * (prx - rx) +
        (pry - ry) * (pry - ry);
    if (distance < 0.0001) {
        return;
    }
    px = x; py = y; pz = z; prx = rx; pry = ry;
    char buffer[1024];
    snprintf(buffer, 1024, "P,%.2f,%.2f,%.2f,%.2f,%.2f\n", x, y, z, rx, ry);
    client_send(buffer);
}

void client_chunk(int p, int q, int key) {
    if (!client_enabled) {
        return;
    }
    char buffer[1024];
    snprintf(buffer, 1024, "C,%d,%d,%d\n", p, q, key);
    client_send(buffer);
}

void client_block(int x, int y, int z, int w) {
    if (!client_enabled) {
        return;
    }
    char buffer[1024];
    snprintf(buffer, 1024, "B,%d,%d,%d,%d\n", x, y, z, w);
    client_send(buffer);
}

void client_light(int x, int y, int z, int w) {
    if (!client_enabled) {
        return;
    }
    char buffer[1024];
    snprintf(buffer, 1024, "L,%d,%d,%d,%d\n", x, y, z, w);
    client_send(buffer);
}

void client_sign(int x, int y, int z, int face, const char *text) {
    if (!client_enabled) {
        return;
    }
    char buffer[1024];
    snprintf(buffer, 1024, "S,%d,%d,%d,%d,%s\n", x, y, z, face, text);
    client_send(buffer);
}

void client_talk(const char *text) {
    if (!client_enabled) {
        return;
    }
    if (strlen(text) == 0) {
        return;
    }
    char buffer[1024];
    snprintf(buffer, 1024, "T,%s\n", text);
    client_send(buffer);
}

#ifdef __EMSCRIPTEN__
void client_message(int fd, void *userData) {
    char buf[4096];
    int len = recv(fd, &buf, sizeof(buf), 0);

    //fprintf(stderr, "read %d bytes\n", len);
    if (len == -1) return;

    buf[len] = 0;

    void (*parse_buffer)(char *) = (void (*)(char *))userData;
    parse_buffer(buf);
}
#else
char *client_recv() {
    if (!client_enabled) {
        return 0;
    }
    char *result = 0;
    mtx_lock(&mutex);
    char *p = queue + qsize - 1;
    while (p >= queue && *p != '\n') {
        p--;
    }
    if (p >= queue) {
        int length = p - queue + 1;
        result = malloc(sizeof(char) * (length + 1));
        memcpy(result, queue, sizeof(char) * length);
        result[length] = '\0';
        int remaining = qsize - length;
        memmove(queue, p + 1, remaining);
        qsize -= length;
        bytes_received += length;
    }
    mtx_unlock(&mutex);
    return result;
}

int recv_worker(void *arg) {
    char *data = malloc(sizeof(char) * RECV_SIZE);
    while (1) {
        int length;
        if ((length = recv(sd, data, RECV_SIZE - 1, 0)) <= 0) {
            if (running) {
                perror("recv");
                exit(1);
            }
            else {
                break;
            }
        }
        data[length] = '\0';
        while (1) {
            int done = 0;
            mtx_lock(&mutex);
            if (qsize + length < QUEUE_SIZE) {
                memcpy(queue + qsize, data, sizeof(char) * (length + 1));
                qsize += length;
                done = 1;
            }
            mtx_unlock(&mutex);
            if (done) {
                break;
            }
            sleep(0);
        }
    }
    free(data);
    return 0;
}
#endif

void client_connect(char *hostname, int port) {
    if (!client_enabled) {
        return;
    }
    struct hostent *host;
    struct sockaddr_in address;
    if ((host = gethostbyname(hostname)) == 0) {
        perror("gethostbyname");
        exit(1);
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ((struct in_addr *)(host->h_addr_list[0]))->s_addr;
    address.sin_port = htons(port);
    if ((sd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }
#ifdef __EMSCRIPTEN__
    // Override the address passed to connect(), for WebSockets:
    // If ws:// or wss:// URL is given, pass it directly - this is how secure WebSockets
    // can be used, example: wss://localhost:1234/craftws. Or alternate paths.
    // Otherwise, use hostname and port, ws scheme, and /craftws path by default.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension" // EM_ASM JavaScript
    EM_ASM_ARGS({
            function getString(p) {
                var s = String(); // not '' because empty C character constant warning
                var c = 0; // var since let fails uglify-js in release-build-js
                while ((c = Module.HEAP8[p++])) {
                    s += String.fromCharCode(c);
                }
                return s;
            }
            const host = getString($0);
            const port = $1;
            console.log('websocket inputs, host='+host+', port='+port);
            const ws = 'ws:' + String.fromCharCode(47) + String.fromCharCode(47); // to avoid //
            const wss = 'wss:' + String.fromCharCode(47) + String.fromCharCode(47);
            if (host === '-') {
                // Special case '-' connects back to ourselves
                var protocol = document.location.protocol === 'https:' ? wss : ws;
                var path = '/craftws';
                // Note, document.location.host contains both hostname AND port, if any
                Module['websocket']['url'] = protocol + document.location.host + path;
            } else if (host.startsWith(ws) || host.startsWith(wss)) {
                // full URL
                Module['websocket']['url'] = host;
            } else {
                // hostname
                Module['websocket']['url'] = ws + host + ':' + port + '/craftws';
            }
            console.log('websocket url:', Module['websocket']['url']);
        }, hostname, port);
#pragma clang diagnostic pop
#endif

    if (connect(sd, (struct sockaddr *)&address, sizeof(address)) == -1) {
#ifdef __EMSCRIPTEN__
        // Websockets are async, so connect() always returns EINPROGRESS
        if (errno == EINPROGRESS) return;
#endif
        perror("connect");
        exit(1);
    }
}

void client_start() {
    if (!client_enabled) {
        return;
    }
    running = true;
    queue = (char *)calloc(QUEUE_SIZE, sizeof(char));
    qsize = 0;
#ifndef __EMSCRIPTEN__ // emscripten gets async message callbacks instead, no receiver thread
    mtx_init(&mutex, mtx_plain);
    if (thrd_create(&recv_thread, recv_worker, NULL) != thrd_success) {
        perror("thrd_create");
        exit(1);
    }
#endif
}

void client_stop() {
    if (!client_enabled) {
        return;
    }
    running = false;
    close(sd);
    // if (thrd_join(recv_thread, NULL) != thrd_success) {
    //     perror("thrd_join");
    //     exit(1);
    // }
    // mtx_destroy(&mutex);
    qsize = 0;
    free(queue);
    // printf("Bytes Sent: %d, Bytes Received: %d\n",
    //     bytes_sent, bytes_received);
}
