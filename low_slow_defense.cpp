#include "./low_slow_defense.h"


unordered_map<int, unique_ptr<Client>> clients(MAX_CLIENTS);  // use client_fd as key
unordered_map<int, unique_ptr<Server>> servers(MAX_SERVERS);  // use server_fd as key
queue<unique_ptr<Filebuf>> free_filebufs;
char* Server::address;
unsigned short Server::port;
fd_set active_fds;           // fd-set used by select
fd_set read_fds;
char read_buffer[10240];     // TODO: what size is appropriate?


Filebuf::Filebuf() {
    char template[] = "/tmp/low_slow_buf_XXXXXX";
    if ((fd = mkstemp(template)) < 0) {
        ERROR("Cannot mkstemp: %s\n", ERRNOSTR);
    }
    file_name = string(template);
}

void Filebuf::clear() {
    if (truncate(fd, 0) < 0) {
        fprintf(stderr, "Cannot truncate file %s: %s\n", file_name.c_str(), ERRNOSTR);
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {  // rewind cursor
        fprintf(stderr, "Cannot lseek: %s\n", ERRNOSTR);
    }
}

Client::Client(const struct sockaddr_in& _addr):
    addr{get_host_and_port(_addr)}, server_fd{IVAL_FILENO}, req_type{NONE}, content_len{0}
{
    // allocate filebuf
    assert(!free_filebufs.empty());
    filebuf = free_filebufs.front();
    free_filebufs.pop();
}

Client::~Client() {
    // release filebuf
    free_filebufs.push(filebuf);
}

void Client::recv_msg(int fd) {
    int count = read(fd, read_buffer, sizeof(read_buffer));
    if (count <= 0) {  // premature close
        if (server_fd != IVAL_FILENO) {
            Server::close_connection(server_fd);
            server_fd = IVAL_FILENO;
        }
        Client::close_connection(fd);
        return;
    }
    // open fd somewhere, with RDWR flag?

    write(1, read_buffer, count);

}

int Client::accept_connection(int master_sock) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int sock = accept(master_sock, (struct sockaddr*)&addr, &addr_len);

    clients[sock] = make_unique<Client>(addr);
    FD_SET(sock, &active_fds);  // keep track
    return sock;
}

void Client::close_connection(int client_fd) {
    printf("Connection closed: %s\n", clients[client_fd]->addr.c_str());
    close(client_fd);
    FD_CLR(client_fd, &active_fds);
    clients.erase(client_fd);
}

Server::Server(int _client_fd): client_fd{_client_fd} {}

void Server::recv_msg_and_reply(int fd) {

}

int Server::create_connection() {
    // int sock = connectTCP(Server::address, 80);

}

void Server::close_connection(int server_fd) {
    close(server_fd);
    FD_CLR(server_fd, &active_fds);
    servers[server_fd].reset(NULL);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("SYNOPSIS\n\t%s <server_addr> <server_port> [port]\n", argv[0]);
        printf("\tserver_addr\tThe address of the server to be protected.\n");
        printf("\tserver_port\tThe port of the server to be protected.\n");
        printf("\tport\tThe port of this proxy to be listening on. (default=8080)\n");
        exit(0);
    }
    Server::address = argv[1];
    Server::port = atoi(argv[2]);
    unsigned short port = (argc >= 4) ? atoi(argv[3]) : 8080;

    int master_sock = passiveTCP(port);  // fd should be 3
    for (int i = 0; i < MAX_FILEBUF; i++) {
        free_filebufs.push(make_unique<Filebuf>());
    }
    assert(master_sock == 3 && free_filebufs.back()->fd == 3 + MAX_FILEBUF);
    int filebuf_last_fd = 3 + MAX_FILEBUF;
    FD_ZERO(&active_fds);
    FD_SET(master_sock, &active_fds);  // keep track master_sock
    raise_open_file_limit(MAX_FILE_DES);

    while (true) {
        memcpy(&read_fds, &active_fds, sizeof(read_fds));

        if (select(MAX_FILE_DES, &read_fds, NULL, NULL, NULL) < 0) {
            ERROR("Error in select: %s\n", ERRNOSTR);
        }
        for (int fd = filebuf_last_fd + 1 ; fd < MAX_FILE_DES; fd++) {
            if (FD_ISSET(fd, &read_fds)) {
                if (fd == master_sock) {
                    // new connection
                    int sock = Client::accept_connection(master_sock);
                    printf("Connected by %s\n", clients[sock]->addr.c_str());
                } else if (clients.contains(fd)) {
                    // message from client
                    clients[fd]->recv_msg(fd);
                } else if (servers.contains(fd)) {
                    // message from server
                    servers[fd]->recv_msg_and_reply(fd);
                    // TODO: when to close connection?
                } else {
                    ERROR("[bug] Unrecognized fd %d not belonging to anyone.\n", fd);
                }
            }
        }
    }
    return 0;
}
