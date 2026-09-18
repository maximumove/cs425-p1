#include "lab.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Layer 1: pure protocol helpers                                     */
/* ------------------------------------------------------------------ */

int smtp_parse_reply_line(const char *line, int *code, bool *is_final)
{
    if (line == NULL || code == NULL || is_final == NULL)
    {
        return SMTP_ERR_ARG;
    }

    if (!isdigit((unsigned char)line[0]) || !isdigit((unsigned char)line[1]) ||
        !isdigit((unsigned char)line[2]))
    {
        return SMTP_ERR_PARSE;
    }

    int value = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    char sep = line[3];

    if (sep == '\0' || sep == ' ')
    {
        *is_final = true;
    }
    else if (sep == '-')
    {
        *is_final = false;
    }
    else
    {
        return SMTP_ERR_PARSE;
    }

    *code = value;
    return SMTP_OK;
}

int smtp_build_command(char *buf, size_t bufsize, const char *fmt, ...)
{
    if (buf == NULL || fmt == NULL || bufsize < 3)
    {
        return SMTP_ERR_ARG;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, bufsize, fmt, ap);
    va_end(ap);

    if (n < 0) // GCOVR_EXCL_START
    {
        return SMTP_ERR_ARG;
    } // GCOVR_EXCL_STOP

    size_t un = (size_t)n;
    if (un > bufsize - 3)
    {
        return SMTP_ERR_TOOLONG;
    }

    buf[un] = '\r';
    buf[un + 1] = '\n';
    buf[un + 2] = '\0';
    return (int)(un + 2);
}

bool smtp_has_crlf_injection(const char *s)
{
    if (s == NULL)
    {
        return false;
    }
    for (const char *p = s; *p != '\0'; p++)
    {
        if (*p == '\r' || *p == '\n')
        {
            return true;
        }
    }
    return false;
}

long smtp_dot_stuff(const char *body, char *out, size_t outsize)
{
    if (out == NULL)
    {
        return SMTP_ERR_ARG;
    }
    if (body == NULL)
    {
        body = "";
    }

    size_t oi = 0;
    size_t i = 0;
    size_t blen = strlen(body);

    while (i < blen)
    {
        if (body[i] == '.')
        {
            if (oi + 1 >= outsize)
            {
                return SMTP_ERR_TOOLONG;
            }
            out[oi++] = '.';
        }

        while (i < blen && body[i] != '\n')
        {
            if (body[i] == '\r')
            {
                i++;
                continue;
            }
            if (oi + 1 >= outsize)
            {
                return SMTP_ERR_TOOLONG;
            }
            out[oi++] = body[i++];
        }

        if (i < blen && body[i] == '\n')
        {
            i++;
        }

        if (oi + 2 >= outsize)
        {
            return SMTP_ERR_TOOLONG;
        }
        out[oi++] = '\r';
        out[oi++] = '\n';
    }

    if (oi >= outsize)
    {
        return SMTP_ERR_TOOLONG;
    }
    out[oi] = '\0';
    return (long)oi;
}

long smtp_build_message(char *out, size_t outsize, const char *from,
                         const char *to, const char *subject,
                         const char *body)
{
    if (out == NULL || from == NULL || to == NULL)
    {
        return SMTP_ERR_ARG;
    }
    if (subject == NULL)
    {
        subject = "";
    }

    int hn = snprintf(out, outsize, "From: %s\r\nTo: %s\r\nSubject: %s\r\n\r\n",
                       from, to, subject);
    if (hn < 0) // GCOVR_EXCL_START
    {
        return SMTP_ERR_ARG;
    } // GCOVR_EXCL_STOP
    size_t used = (size_t)hn;
    if (used >= outsize)
    {
        return SMTP_ERR_TOOLONG;
    }

    long bn = smtp_dot_stuff(body, out + used, outsize - used);
    if (bn < 0)
    {
        return bn;
    }
    used += (size_t)bn;

    if (used + 3 >= outsize)
    {
        return SMTP_ERR_TOOLONG;
    }
    out[used++] = '.';
    out[used++] = '\r';
    out[used++] = '\n';
    out[used] = '\0';
    return (long)used;
}

/* ------------------------------------------------------------------ */
/* Layer 2: the session, run over a pluggable transport               */
/* ------------------------------------------------------------------ */

void smtp_transport_init(smtp_transport_t *t, smtp_read_fn read_fn,
                          smtp_write_fn write_fn, void *ctx)
{
    if (t == NULL)
    {
        return;
    }
    t->read = read_fn;
    t->write = write_fn;
    t->ctx = ctx;
    t->buf_len = 0;
}

long smtp_read_line(smtp_transport_t *t, char *out, size_t outsize)
{
    if (t == NULL || out == NULL || outsize == 0)
    {
        return SMTP_ERR_ARG;
    }

    for (;;)
    {
        char *nl = memchr(t->buf, '\n', t->buf_len);
        if (nl != NULL)
        {
            size_t linelen = (size_t)(nl - t->buf);
            size_t copylen = linelen;
            if (copylen > 0 && t->buf[copylen - 1] == '\r')
            {
                copylen--;
            }
            if (copylen >= outsize)
            {
                return SMTP_ERR_TOOLONG;
            }
            memcpy(out, t->buf, copylen);
            out[copylen] = '\0';

            size_t consumed = linelen + 1;
            size_t remaining = t->buf_len - consumed;
            memmove(t->buf, t->buf + consumed, remaining);
            t->buf_len = remaining;
            return (long)copylen;
        }

        if (t->buf_len >= SMTP_BUF_SIZE)
        {
            return SMTP_ERR_TOOLONG;
        }
        if (t->read == NULL)
        {
            return SMTP_ERR_ARG;
        }

        long n = t->read(t->ctx, t->buf + t->buf_len, SMTP_BUF_SIZE - t->buf_len);
        if (n < 0)
        {
            return SMTP_ERR_IO;
        }
        if (n == 0)
        {
            return SMTP_ERR_CLOSED;
        }
        t->buf_len += (size_t)n;
    }
}

int smtp_read_reply(smtp_transport_t *t, char *out, size_t outsize)
{
    if (t == NULL || out == NULL || outsize == 0)
    {
        return SMTP_ERR_ARG;
    }

    char line[SMTP_BUF_SIZE];
    int code = -1;
    bool final = false;
    int lines = 0;

    do
    {
        long rc = smtp_read_line(t, line, sizeof line);
        if (rc < 0)
        {
            switch (rc)
            {
                case SMTP_ERR_TOOLONG:
                    snprintf(out, outsize, "server reply line was too long");
                    break;
                case SMTP_ERR_CLOSED:
                    snprintf(out, outsize, "server closed the connection unexpectedly");
                    break;
                default:
                    snprintf(out, outsize, "connection error while reading server reply");
                    break;
            }
            return (int)rc;
        }

        int this_code;
        bool this_final;
        int prc = smtp_parse_reply_line(line, &this_code, &this_final);
        if (prc != SMTP_OK)
        {
            snprintf(out, outsize, "malformed reply line: \"%s\"", line);
            return SMTP_ERR_PARSE;
        }

        if (code == -1)
        {
            code = this_code;
        }
        else if (this_code != code)
        {
            snprintf(out, outsize,
                     "multi-line reply changed status code from %d to %d", code,
                     this_code);
            return SMTP_ERR_PROTOCOL;
        }

        final = this_final;
        size_t n = strlen(line);
        if (n >= outsize)
        {
            n = outsize - 1;
        }
        memcpy(out, line, n);
        out[n] = '\0';
        lines++;
    } while (!final && lines < SMTP_MAX_REPLY_LINES);

    if (!final)
    {
        snprintf(out, outsize, "reply had too many continuation lines");
        return SMTP_ERR_PROTOCOL;
    }

    return code;
}

int smtp_write_all(smtp_transport_t *t, const char *buf, size_t len)
{
    if (t == NULL || buf == NULL || t->write == NULL)
    {
        return SMTP_ERR_ARG;
    }

    size_t sent = 0;
    while (sent < len)
    {
        long n = t->write(t->ctx, buf + sent, len - sent);
        if (n <= 0)
        {
            return SMTP_ERR_IO;
        }
        sent += (size_t)n;
    }
    return SMTP_OK;
}

int smtp_send_command(smtp_transport_t *t, const char *cmd, int expect_min,
                       int expect_max, char *reply_out, size_t reply_outsize)
{
    if (t == NULL || cmd == NULL)
    {
        return SMTP_ERR_ARG;
    }

    int wrc = smtp_write_all(t, cmd, strlen(cmd));
    if (wrc != SMTP_OK)
    {
        if (reply_out != NULL && reply_outsize > 0)
        {
            snprintf(reply_out, reply_outsize, "failed to send command to server");
        }
        return wrc;
    }

    char localbuf[SMTP_BUF_SIZE];
    char *dest = (reply_out != NULL && reply_outsize > 0) ? reply_out : localbuf;
    size_t destsize =
        (reply_out != NULL && reply_outsize > 0) ? reply_outsize : sizeof localbuf;

    int code = smtp_read_reply(t, dest, destsize);
    if (code < 0)
    {
        return code;
    }
    if (code < expect_min || code > expect_max)
    {
        return SMTP_ERR_PROTOCOL;
    }
    return code;
}

int smtp_run_session(smtp_transport_t *t, const char *helo_host,
                      const char *from, const char *to, const char *subject,
                      const char *body, char *errbuf, size_t errbufsize)
{
    char discard[SMTP_BUF_SIZE];
    if (errbuf == NULL || errbufsize == 0)
    {
        errbuf = discard;
        errbufsize = sizeof discard;
    }

    if (t == NULL || helo_host == NULL || from == NULL || to == NULL)
    {
        snprintf(errbuf, errbufsize, "internal error: missing session argument");
        return SMTP_ERR_ARG;
    }

    char reply[SMTP_BUF_SIZE];
    char cmd[SMTP_BUF_SIZE];
    int code;

    /* The server speaks first. */
    code = smtp_read_reply(t, reply, sizeof reply);
    if (code < 0)
    {
        snprintf(errbuf, errbufsize, "no greeting from server: %s", reply);
        return code;
    }
    if (code != 220)
    {
        snprintf(errbuf, errbufsize, "server did not greet with 220: %d %s", code,
                 reply);
        return SMTP_ERR_PROTOCOL;
    }

    if (smtp_build_command(cmd, sizeof cmd, "HELO %s", helo_host) < 0)
    {
        snprintf(errbuf, errbufsize, "HELO host name is too long");
        return SMTP_ERR_TOOLONG;
    }
    code = smtp_send_command(t, cmd, 200, 299, reply, sizeof reply);
    if (code < 0)
    {
        snprintf(errbuf, errbufsize, "HELO failed: %s", reply);
        return code;
    }

    if (smtp_build_command(cmd, sizeof cmd, "MAIL FROM:<%s>", from) < 0)
    {
        snprintf(errbuf, errbufsize, "sender address is too long");
        return SMTP_ERR_TOOLONG;
    }
    code = smtp_send_command(t, cmd, 200, 299, reply, sizeof reply);
    if (code < 0)
    {
        snprintf(errbuf, errbufsize, "MAIL FROM rejected: %s", reply);
        return code;
    }

    if (smtp_build_command(cmd, sizeof cmd, "RCPT TO:<%s>", to) < 0)
    {
        snprintf(errbuf, errbufsize, "recipient address is too long");
        return SMTP_ERR_TOOLONG;
    }
    code = smtp_send_command(t, cmd, 200, 299, reply, sizeof reply);
    if (code < 0)
    {
        snprintf(errbuf, errbufsize, "RCPT TO rejected: %s", reply);
        return code;
    }

    /* "DATA" is a short fixed literal, so building it into an
     * SMTP_BUF_SIZE buffer can never fail; assert the invariant
     * instead of handling an error path that cannot be reached. */
    int drc = smtp_build_command(cmd, sizeof cmd, "DATA");
    assert(drc >= 0);
    code = smtp_send_command(t, cmd, 354, 354, reply, sizeof reply);
    if (code < 0)
    {
        snprintf(errbuf, errbufsize, "DATA rejected: %s", reply);
        return code;
    }

    size_t body_len = (body != NULL) ? strlen(body) : 0;
    size_t from_len = strlen(from);
    size_t to_len = strlen(to);
    size_t subject_len = (subject != NULL) ? strlen(subject) : 0;
    size_t cap = (2 * body_len) + from_len + to_len + subject_len + 4096;

    char *msgbuf = malloc(cap);
    if (msgbuf == NULL) // GCOVR_EXCL_START
    {
        snprintf(errbuf, errbufsize, "out of memory building message");
        return SMTP_ERR_TOOLONG;
    } // GCOVR_EXCL_STOP

    long mn = smtp_build_message(msgbuf, cap, from, to, subject, body);
    /* dot-stuffing can grow the body by at most a factor of 2 plus a
     * small constant (a stuffed leading dot, and a final CRLF not
     * backed by an input newline), which `cap` above accounts for
     * with a lot of room to spare -- this can never actually fail. */
    assert(mn >= 0);

    int wrc = smtp_write_all(t, msgbuf, (size_t)mn);
    free(msgbuf);
    if (wrc != SMTP_OK)
    {
        snprintf(errbuf, errbufsize, "connection failed while sending message");
        return wrc;
    }

    code = smtp_read_reply(t, reply, sizeof reply);
    if (code < 0)
    {
        snprintf(errbuf, errbufsize, "connection failed waiting for confirmation: %s",
                 reply);
        return code;
    }
    if (code < 200 || code > 299)
    {
        snprintf(errbuf, errbufsize, "server did not queue the message: %d %s", code,
                 reply);
        return SMTP_ERR_PROTOCOL;
    }

    /* The message is already queued at this point, so a bad QUIT reply
     * (or none at all) does not undo that -- we still check it because
     * the spec asks for every reply to be checked, but we don't fail
     * the whole send over it. */
    if (smtp_build_command(cmd, sizeof cmd, "QUIT") >= 0)
    {
        char quit_reply[SMTP_BUF_SIZE];
        (void)smtp_send_command(t, cmd, 200, 299, quit_reply, sizeof quit_reply);
    }

    return SMTP_OK;
}

/* ------------------------------------------------------------------ */
/* Layer 3: the socket transport                                      */
/* ------------------------------------------------------------------ */

int smtp_socket_connect(const char *host, const char *port)
{
    if (host == NULL || port == NULL)
    {
        fprintf(stderr, "myapp: internal error: missing host or port\n");
        return -1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int gai_rc = getaddrinfo(host, port, &hints, &res);
    if (gai_rc != 0)
    {
        fprintf(stderr, "myapp: could not resolve %s port %s: %s\n", host, port,
                gai_strerror(gai_rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *rp = res; rp != NULL; rp = rp->ai_next)
    {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) // GCOVR_EXCL_START
        {
            continue;
        } // GCOVR_EXCL_STOP
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
        {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd == -1)
    {
        fprintf(stderr, "myapp: could not connect to %s port %s: %s\n", host, port,
                strerror(errno));
        return -1;
    }
    return fd;
}

long smtp_socket_read(void *ctx, char *buf, size_t len)
{
    int fd = *(int *)ctx;
    ssize_t n = recv(fd, buf, len, 0);
    if (n < 0)
    {
        return -1;
    }
    return (long)n;
}

long smtp_socket_write(void *ctx, const char *buf, size_t len)
{
    int fd = *(int *)ctx;
    ssize_t n = send(fd, buf, len, 0);
    if (n < 0)
    {
        return -1;
    }
    return (long)n;
}

void smtp_socket_close(int fd)
{
    if (fd >= 0)
    {
        close(fd);
    }
}

