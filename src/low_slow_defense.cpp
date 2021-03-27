#include "./low_slow_defense.h"


queue<shared_ptr<Filebuf>> free_filebufs;
char* Server::address;
unsigned short Server::port;
struct event_base* evt_base;
char read_buffer[10240];     // TODO: what size is appropriate?


Filebuf::Filebuf(): data_size{0} {
    char name[] = "/tmp/low_slow_buf_XXXXXX";
    if ((fd = mkstemp(name)) < 0) {
        ERROR("Cannot mkstemp: %s\n", ERRNOSTR);
    }
    file_name = string(name);
    // printf("%d: %s\n", fd, name);
}

void Filebuf::write(const char* buffer, int count) {
    if (write(fd, buffer, count) < 0) {  // TODO: make sure size matches
        fprintf(stderr, "Write failed: %s\n", ERRNOSTR);
    }
    data_size += count;
    printf("Written %d bytes to %s\n", count, file_name.c_str());
}

int Filebuf::read(char* buffer, int size) {
    // TODO: data_size
    return read(fd, buffer, size);
}

void Filebuf::rewind() {
    if (lseek(fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "Cannot lseek: %s\n", ERRNOSTR);
    }
}

void Filebuf::clear() {
    if (ftruncate(fd, 0) < 0) {
        fprintf(stderr, "Cannot truncate file %s: %s\n", file_name.c_str(), ERRNOSTR);
    }
    rewind();
}

Client::Client(int fd, const struct sockaddr_in& _addr, shared_ptr<Filebuf> _filebuf):
    addr{get_host_and_port(_addr)}, req_type{NONE}, content_len{0}, filebuf{_filebuf},
    filebuf_from_pool{false}
{
    // allocate events
    read_evt.reset(new_read_event(fd, Client::recv_msg, this));
    write_evt.reset(new_write_event(fd, Client::send_msg, this));
    if (!filebuf) {
        filebuf_from_pool = true;
        filebuf = free_filebufs.front();
        free_filebufs.pop();
    }
}

Client::~Client() {
    printf("Connection closed: %s\n", addr.c_str());
    int fd = event_get_fd(read_evt);
    close(fd);
    event_del(read_evt);
    event_del(write_evt);
    if (filebuf_from_pool) {
        free_filebufs.push(filebuf);
    }
}

void Client::accept_connection(int master_sock, short flag, void* arg) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int sock = accept(master_sock, (struct sockaddr*)&addr, &addr_len);
    if (evutil_make_socket_nonblocking(sock) < 0) {
        ERROR("Cannot make socket nonblocking: %s\n", ERRNOSTR);
    }
    if (free_filebufs.empty()) {
        fprintf(stderr, "Max connection %d reached.\n", MAX_CONNECTIONS);
        shared_ptr<Client> client(new Client(sock, addr, make_shared<Filebuf>()));
        init_with_503_file(client->filebuf);
        event_add(client->write_evt);
    } else {
        shared_ptr<Client> client(new Client(sock, addr));
        event_add(client->read_evt.get());
        printf("Connected by %s\n", client->addr.c_str());
    }
}

void Client::recv_msg(int fd, short flag, void* arg) {
    Client* client = (Client*)arg;
    int count = read(fd, read_buffer, sizeof(read_buffer));
    if (count <= 0) {  // premature close
        if (count < 0) {
            fprintf(stderr, "Error reading from client: %s\n", ERRNOSTR);
        }
        if (client->server) {
            client->server->~Server();
        }
        client->~Client();
        return;
    }
    client->filebuf->write(read_buffer, count);
    if (getchar() == '1') {    // TODO: at some point
        // send request to server
        event_del(client->read_evt);  // disable further reads
        client->filebuf->rewind();
        shared_ptr<Server> server(new Server(client));
        event_add(server->write_evt);
    }
}

void Client::send_msg(int fd, short flag, void* arg) {
    Client* client = (Client*)arg;
    int n = client->filebuf->read(read_buffer, sizeof(read_buffer));
    int count = write(fd, read_buffer, n);  // TODO: subtract?
    if (count <= 0) {  // TODO: catch PIPE_ERROR ?
        count;
    }
    // TODO: at some time
    if (getchar() == '4') {
        client->~Client();
    }
}

Server::Server(Client* _client): client{_client}, filebuf{_client->filebuf} {
    int sock = connectTCP(Server::address, Server::port);
    if (evutil_make_socket_nonblocking(sock) < 0) {
        ERROR("Cannot make socket nonblocking: %s\n", ERRNOSTR);
    }
    // allocate events
    read_evt.reset(new_read_event(sock, Server::recv_msg_and_reply, this));
    write_evt.reset(new_write_event(sock, Server::send_msg, this));
}

Server::~Server() {
    int fd = event_get_fd(read_evt);
    close(fd);
    event_del(read_evt);
    event_del(write_evt);
    filebuf.reset(NULL);
}

void Server::send_msg(int fd, short flag, void* arg) {
    Server* server = (Server*)arg;
    int n = server->filebuf->read(read_buffer, sizeof(read_buffer));
    int count = write(fd, read_buffer, n);  // TODO: subtract?
    if (count <= 0) {  // TODO: catch PIPE_ERROR ?
        count;
    }
    // TODO: at some point
    if (getchar() == '2') {
        // get response
        event_del(server->write_evt);
        server->filebuf->clear();
        event_add(server->read_evt);
    }
}

void Server::recv_msg(int fd, short flag, void* arg) {
    Server* server = (Server*)arg;
    int count = read(fd, read_buffer, sizeof(read_buffer));
    if (count <= 0) {  // premature close
        count;
    }
    server->filebuf->write(read_buffer, count);
    // TODO: at some point
    if (getchar() == '3') {
        // response to client
        event_del(server->read_evt);
        server->filebuf->rewind();
        server->~Server();  // destroy server
        event_add(client->write_evt);
    }
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

    raise_open_file_limit(MAX_FILE_DES);
    int master_sock = passiveTCP(port);  // fd should be 3
    for (int i = 0; i < MAX_FILEBUF; i++) {
        free_filebufs.push(make_shared<Filebuf>());
    }
    assert(master_sock == 3 && free_filebufs.back()->fd == FILEBUF_LAST_FD);
    event_add(new_read_event(master_sock, Client::accept_connection));
    if (event_base_dispatch(evt_base) < 0) {
        ERROR("Cannot dispatch event: %s\n", ERRNOSTR);
    }

    return 0;
}
