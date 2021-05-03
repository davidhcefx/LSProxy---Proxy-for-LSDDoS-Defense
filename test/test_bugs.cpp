

/*********************************************************
 * # Send FIN instead of RST
 *  - client send message, but server not listening
 *  > Cannot see the 503 meesage because of RST.
 *
 * # Signal not been handled correctly
 *  - client connects
 *  - send SIGUSR1
 *  - terminate client before timeout
 *  > Crash.
 *
 * # SIGPIPE error
 *  - client connects, but server not listening
 *  - terminate client
 *  > "Shutdown error #(13): Transport endpoint is not connected"
 *
 * # ????
 *  - send SIGUSR1 in the background every 0.1 seconds
 *  - client send "GET / HTTP/1.1\r\n\r\n", and terminate.
 *  - server send "\n\n", and terminate
 *  > "Cannot del event: Bad file descriptor"
 *********************************************************/