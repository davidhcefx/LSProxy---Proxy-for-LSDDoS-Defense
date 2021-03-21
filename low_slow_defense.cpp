#include "./low_slow_defense.h"


array<unique_ptr<Client>, MAX_FILE_DES> clients;  // use fd as key; these are mutually exclusive
array<unique_ptr<Server>, MAX_FILE_DES> servers;  // use fd as key
fd_set active_fds;           // fd-set used by select
fd_set read_fds;
char read_buffer[10240];     // TODO: what size is appropriate?


Client::Client(const struct sockaddr_in& _addr):
    addr{get_host_and_port(_addr)}, server_fd{IVAL_FILENO}, req_type{0}, content_len{0}
{}

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
    if (req_type == NONE) {
        // determines req_type

    }

    write(1, read_buffer, count);

}

int Client::accept_connection(int master_sock) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int sock = accept(master_sock, (struct sockaddr*)&addr, &addr_len);

    clients[sock].reset(new Client(sock, addr));
    FD_SET(sock, &active_fds);  // keep track
    return sock;
}

void Client::close_connection(int client_fd) {
    close(client_fd);
    FD_CLR(client_fd, &active_fds);
    printf("Connection closed: %s\n", clients[client_fd]->addr.c_str());
    clients[client_fd].reset(NULL);
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
    int master_sock = passiveTCP(port);
    FD_ZERO(&active_fds);
    FD_SET(master_sock, &active_fds);  // keep track master_sock
    raise_open_file_limit(MAX_FILE_DES);

    while (true) {
        memcpy(&read_fds, &active_fds, sizeof(read_fds));

        if (select(MAX_FILE_DES, &read_fds, NULL, NULL, NULL) < 0) {
            ERROR("Error in select: %s\n", ERRNOSTR);
        }
        for (int fd = 0; fd < MAX_FILE_DES; fd++) {
            if (FD_ISSET(fd, &read_fds)) {
                if (fd == master_sock) {
                    // new connection
                    int sock = Client::accept_connection(master_sock);
                    printf("Connected by %s\n", clients[sock]->addr.c_str());
                } else if (clients[fd]) {
                    // message from client
                    clients[fd].recv_msg(fd);
                } else if (server[fd]) {
                    // message from server
                    servers[fd].recv_msg_and_reply(fd);
                    // TODO: when to close connection?
                } else {
                    ERROR("[bug] Unrecognized fd %d not belonging to anyone.\n", fd);
                }
            }
        }
    }
    return 0;
}
