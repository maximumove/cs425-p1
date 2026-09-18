#ifndef LAB_H
#define LAB_H

#include <stdbool.h>
#include <stddef.h>

/*
 * A from-scratch RFC 5321 SMTP client, split into three layers so the
 * protocol logic can be unit tested without a network:
 *
 *   1. Pure protocol helpers   - strings in, strings/codes out, no I/O.
 *   2. The session             - runs over a read/write callback pair
 *                                 (a "transport"), so tests can plug in
 *                                 a scripted in-memory server.
 *   3. The socket transport    - thin wrappers over getaddrinfo/connect/
 *                                 recv/send that satisfy layer 2's
 *                                 callback interface for the real client.
 */

/* Maximum size of one buffered read from the transport, and therefore
 * the longest reply line smtp_read_line() can hold. RFC 5321 4.5.3.1.4
 * caps a reply line at 512 octets including CRLF; this leaves headroom. */
#define SMTP_BUF_SIZE 1024

/* Upper bound on how many lines a single (possibly multi-line) reply
 * may have, so a server that never sends a final line can't spin the
 * reader forever. */
#define SMTP_MAX_REPLY_LINES 200

/* Status/error codes returned by layer 1 and layer 2 functions.
 * Successful reads of a numeric SMTP reply return that reply code
 * (a positive int, e.g. 250) instead of SMTP_OK. */
typedef enum
{
    SMTP_OK = 0,
    SMTP_ERR_PARSE = -1,     /* a reply line was not "###<sp-or-dash>..." */
    SMTP_ERR_TOOLONG = -2,   /* a line/command/message did not fit in the buffer given */
    SMTP_ERR_CLOSED = -3,    /* the peer closed the connection before a full line arrived */
    SMTP_ERR_IO = -4,        /* the read or write callback reported a failure */
    SMTP_ERR_PROTOCOL = -5,  /* the server's reply code was not what this step expected,
                                 or a multi-line reply's lines disagreed on their code */
    SMTP_ERR_INJECTION = -6, /* a caller-supplied field contains a bare CR or LF */
    SMTP_ERR_ARG = -7        /* a bad argument was passed (NULL, zero-size buffer, etc.) */
} smtp_status_t;

/* ------------------------------------------------------------------ */
/* Layer 1: pure protocol helpers. No I/O anywhere in this section.   */
/* ------------------------------------------------------------------ */

/* Parse one reply line (CRLF already stripped by the caller), e.g.
 * "250 OK" or "250-Hello there". On success returns SMTP_OK, stores
 * the three digit status code in *code, and stores in *is_final
 * whether this is the last line of the reply: true when the line is
 * exactly three digits or the fourth character is a space, false when
 * the fourth character is '-'. Returns SMTP_ERR_PARSE if the line
 * does not begin with three digits or its fourth character (if any)
 * is neither ' ' nor '-'. */
int smtp_parse_reply_line(const char *line, int *code, bool *is_final);

/* Format one SMTP command line into buf and terminate it with CRLF,
 * e.g. smtp_build_command(buf, sizeof buf, "MAIL FROM:<%s>", from)
 * produces "MAIL FROM:<addr>\r\n". Returns the number of bytes
 * written, excluding the terminating NUL, or SMTP_ERR_TOOLONG if the
 * formatted command plus CRLF would not fit in bufsize, or
 * SMTP_ERR_ARG for a NULL/zero-size argument. */
int smtp_build_command(char *buf, size_t bufsize, const char *fmt, ...);

/* True if s contains a bare CR or LF. Reject any address, subject, or
 * other header-bound field that fails this check rather than sending
 * it -- passing one through would let whoever supplied it inject an
 * extra SMTP command or mail header into the session. */
bool smtp_has_crlf_injection(const char *s);

/* Dot-stuff body per RFC 5321 4.5.2: for every line, if it begins
 * with '.', an extra leading '.' is written; every line (whether or
 * not it ended with a newline in the input) is written CRLF
 * terminated in the output. Input lines may be separated by a bare
 * '\n' or by "\r\n"; either way the output always uses "\r\n". out is
 * NUL terminated on success. Returns the number of bytes written,
 * excluding the NUL, or SMTP_ERR_TOOLONG if it would not fit in
 * outsize, or SMTP_ERR_ARG for a NULL argument. */
long smtp_dot_stuff(const char *body, char *out, size_t outsize);

/* Build the full DATA payload: "From:", "To:" and "Subject:" headers,
 * a blank line, the dot-stuffed body, and the terminating ".\r\n"
 * line. subject and body may be NULL, treated as empty. Returns the
 * number of bytes written, excluding the NUL, or SMTP_ERR_TOOLONG if
 * it would not fit in outsize, or SMTP_ERR_ARG for a NULL from/to. */
long smtp_build_message(char *out, size_t outsize, const char *from,
                         const char *to, const char *subject,
                         const char *body);

/* ------------------------------------------------------------------ */
/* Layer 2: the session, run over a pluggable transport.              */
/* ------------------------------------------------------------------ */

/* Read up to len bytes into buf. Returns the number of bytes read
 * (0 meaning the peer closed the connection), or a negative value on
 * failure. Implemented by the socket transport for the real client,
 * and by a scripted in-memory server in tests. */
typedef long (*smtp_read_fn)(void *ctx, char *buf, size_t len);

/* Write len bytes from buf, blocking until all of them are sent or an
 * error occurs. Returns the number of bytes written, or a negative
 * value on failure. */
typedef long (*smtp_write_fn)(void *ctx, const char *buf, size_t len);

/* A transport: a read/write callback pair, a context pointer passed
 * back to both callbacks, and the internal buffer smtp_read_line()
 * uses to hold bytes that have been read but not yet consumed as a
 * line (so a reply that arrives a few bytes at a time, or several
 * replies that arrive in a single read, are both handled correctly). */
typedef struct
{
    smtp_read_fn read;
    smtp_write_fn write;
    void *ctx;
    char buf[SMTP_BUF_SIZE];
    size_t buf_len;
} smtp_transport_t;

/* Initialize a transport with its callbacks and context. Always
 * succeeds. */
void smtp_transport_init(smtp_transport_t *t, smtp_read_fn read_fn,
                          smtp_write_fn write_fn, void *ctx);

/* Read one CRLF- (or bare LF-) terminated line, with the line ending
 * stripped, into out. Refills the internal buffer only when it does
 * not already hold a complete line. Returns the line length on
 * success, SMTP_ERR_CLOSED if the peer closed before a full line
 * arrived, SMTP_ERR_TOOLONG if a line would not fit in outsize or
 * would overflow the transport's internal buffer before a newline is
 * seen, or SMTP_ERR_IO if the read callback failed. */
long smtp_read_line(smtp_transport_t *t, char *out, size_t outsize);

/* Read one full, possibly multi-line, reply (e.g. a "250-...", ...,
 * "250 ..." sequence) and verify every line shares the same status
 * code. On success returns that code and copies the text of the
 * final line into out. On failure returns a negative smtp_status_t
 * and copies a description of what went wrong into out. */
int smtp_read_reply(smtp_transport_t *t, char *out, size_t outsize);

/* Write all len bytes of buf through the transport, handling short
 * writes. Returns SMTP_OK on success or SMTP_ERR_IO on failure. */
int smtp_write_all(smtp_transport_t *t, const char *buf, size_t len);

/* Send a single already-built command line (as produced by
 * smtp_build_command) and read its reply. Succeeds only if the
 * reply's code is within [expect_min, expect_max] inclusive; a
 * command not part of that pattern (e.g. a code class check like
 * 200-299) can pass min==max for an exact match. Returns the reply
 * code on success, or a negative smtp_status_t on failure; either
 * way the reply's final line text is copied into reply_out. */
int smtp_send_command(smtp_transport_t *t, const char *cmd, int expect_min,
                       int expect_max, char *reply_out, size_t reply_outsize);

/* Run an entire SMTP session over t: read the greeting, then send
 * HELO, MAIL FROM, RCPT TO, DATA, the message built from from/to/
 * subject/body, and QUIT, checking each reply in turn and stopping at
 * the first one that is not what the protocol expects. Returns
 * SMTP_OK if the message was queued (the reply to the terminating
 * "." was in the 2xx class). On any failure returns a negative
 * smtp_status_t and writes a human-readable explanation -- including
 * the server's actual reply where there is one -- into errbuf. */
int smtp_run_session(smtp_transport_t *t, const char *helo_host,
                      const char *from, const char *to, const char *subject,
                      const char *body, char *errbuf, size_t errbufsize);

/* ------------------------------------------------------------------ */
/* Layer 3: the socket transport -- thin wrappers, no protocol logic. */
/* ------------------------------------------------------------------ */

/* Resolve host/port with getaddrinfo (host need not be a dotted
 * quad) and connect a TCP socket to it. Returns the connected file
 * descriptor, or -1 on failure (with a message already printed to
 * stderr describing which step failed). */
int smtp_socket_connect(const char *host, const char *port);

/* smtp_read_fn / smtp_write_fn implementations over a plain TCP
 * socket. ctx is a pointer to the int file descriptor returned by
 * smtp_socket_connect. */
long smtp_socket_read(void *ctx, char *buf, size_t len);
long smtp_socket_write(void *ctx, const char *buf, size_t len);

/* Close a socket previously returned by smtp_socket_connect. */
void smtp_socket_close(int fd);

#endif /* LAB_H */
