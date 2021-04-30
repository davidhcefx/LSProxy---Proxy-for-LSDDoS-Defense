#include "ls_proxy.h"


void Connection::set_slow_mode() {
    LOG2("[%s] Connection setting to slow mode...\n", client->c_addr());
    fast_mode = false;
    in_transition = true;
    client->request_buf = new Filebuf("request_buf");
    client->request_tmp_buf = new Filebuf("request_tmp_buf");
    client->response_buf = new FIFOfilebuf("response_buf");
    if (!parser) {  // parser & server not created
        parser = new HttpParser();
        client->pause_rw();
        /* history must be empty since no request had been sent */
        client->resume_rw();
        set_transition_done();
        return;
    }

    // pause client, wait until server finished its msg then close server
    client->pause_rw();
    struct timeval timeout = {.tv_sec = TRANSIT_TIMEOUT, .tv_usec = 0};
    del_event(server->read_evt);
    add_event(server->read_evt, &timeout);  // attach a timeout to recv
    if (server->queued_output->data_size() > 0) {
        add_event(server->write_evt);
    } else {
        server->free_queued_output();
    }

    // retrieve previous packets and decommission fast-mode queue
    client->copy_history_to(client->request_buf);
    while (client->queued_output->data_size() > 0) {
        client->queued_output->dump_to(client->response_buf);
    }
    client->free_queued_output();
}

void Connection::fast_forward(Client*/*client*/, Server*/*server*/) {
    // first store to global_buffer
    auto queue_s = server->queued_output;
    auto stat_c = read_all(client->get_fd(), global_buffer,
                           queue_s->remaining_space());
    if (stat_c.has_error && errno != EAGAIN && errno != EINTR) { [[unlikely]]
        ERROR("Read failed (#%d)", client->get_fd());
    }
    try {
        client->keep_track_request_history(global_buffer, stat_c.nbytes);
    } catch (ParserError& err) {
        // close connection, since we can't reliably parse requests
        free_server();
        free_parser();
        if (client->queued_output->data_size() > 0) {
            client->set_reply_only_mode();
        } else {
            delete this;
        }
        return;
    }
    // then append to server's queued output, and write them out
    assert(queue_s->copy_from(global_buffer, stat_c.nbytes) == stat_c.nbytes);
    auto stat_s = queue_s->write_all_to(server->get_fd());
    LOG2("[%s] %6lu >>>> queue(%lu) >>>> %-6lu [SERVER]\n", client->c_addr(),
         stat_c.nbytes, queue_s->data_size(), stat_s.nbytes);
    if (stat_c.has_eof) { [[unlikely]]  // client closed
        delete this;
        return;
    }
    if (stat_s.nbytes == 0) { [[unlikely]]  // server unwritable
        // disable client's recv temporarily
        del_event(client->read_evt);
        add_event(server->write_evt);
        LOG2("[%s] Server temporarily unwritable.\n", client->c_addr());
    }
}

void Connection::fast_forward(Server*/*server*/, Client*/*client*/) {
    // append to client's queued output, and write them out
    auto stat_s = client->queued_output->read_all_from(server->get_fd());
    auto stat_c = client->queued_output->write_all_to(client->get_fd());
    LOG2("[%s] %6lu <<<< queue(%lu) <<<< %-6lu [SERVER]\n", client->c_addr(),
         stat_c.nbytes, client->queued_output->data_size(), stat_s.nbytes);
    if (stat_s.has_eof) { [[unlikely]]  // server closed
        free_server();
        free_parser();
        if (client->queued_output->data_size() > 0) {
            client->set_reply_only_mode();
        } else {
            delete this;
        }
        return;
    }
    if (stat_c.nbytes == 0) { [[unlikely]]  // client unwritable
        // disable server's recv temporily
        del_event(server->read_evt);
        add_event(client->write_evt);
        LOG2("[%s] Client temporarily unwritable.\n", client->c_addr());
    }
}

void Connection::accept_new(int master_sock, short/*flag*/, void*/*arg*/) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int sock = accept(master_sock, (struct sockaddr*)&addr, &addr_len);
    if (evutil_make_socket_nonblocking(sock) < 0) { [[unlikely]]
        ERROR_EXIT("Cannot make socket nonblocking");
    }
    if (free_hybridbuf.empty()) { [[unlikely]]
        WARNING("Max connection <%d> reached", MAX_CONNECTION);
        reply_with_503_unavailable(sock);
        LOG1("[%s] Connection closed.\n", get_host_and_port(addr).c_str());
        close_socket_gracefully(sock);
        return;
    }
    Connection* conn = new Connection();
    conn->client = new Client(sock, addr, conn);
    add_event(conn->client->read_evt);
    LOG1("[%s] Connection created (#%d)\n", conn->client->c_addr(), sock);
}
