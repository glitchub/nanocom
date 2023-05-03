// Initialize telnet IAC processor and return pointer to context or NULL. Context must be passed to
// tx_telnet and rx_telnet, caller can free() it when done.
// "target" is a persisent queue (see queue.c) that is delivered to the telnet server.
// If "binary" is true then negotiate a binary connection with server, otherwise an ASCII
// connection.
// If not NULL, "termtype" points to a persistent terminal type string to be sent in response to
// server's TTYPE request (if there is one).
void *init_telnet(queue *target, bool binary, char *termtype);

// Given context and a character to be sent to telnet server, return true if caller should send it,
// or false if the character was handled internally.
bool tx_telnet(void *context, unsigned char c);

// Given context and a character received from the telnet server, return true if the caller should
// process it, or false if the character was handled internally.
bool rx_telnet(void *context, unsigned char c);

// Given context and window size, update server if the server supports. If used should be called
// after init_telnet() and then on receipt of SIGWINCH.
void resize_telnet(void *_ctx, int cols, int rows);
