#include "lab.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TEST
#define main main_exclude
#endif

static const char *useMsg =
    "Usage: myapp -f <from> -t <to> [-s subject] [-b body] [-p port]\n"
    "         [-H helo-host] <server>\n"
    "\n"
    "-f <from>       envelope sender, for example you@example.com\n"
    "-t <to>         envelope recipient\n"
    "-s <subject>    subject line (default: empty)\n"
    "-b <body>       message body (default: read from stdin)\n"
    "-p <port>       port or service name (default: 25)\n"
    "-H <helo-host>  host name sent with HELO (default: localhost)\n"
    "<server>        host name or address of the mail server\n";

/* Read every byte of stdin into a heap buffer, NUL terminated.
 * Returns NULL on allocation failure. */
static char *read_all_stdin(void)
{
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL)
    {
        return NULL;
    }

    for (;;)
    {
        if (len == cap)
        {
            size_t newcap = cap * 2;
            char *nb = realloc(buf, newcap);
            if (nb == NULL)
            {
                free(buf);
                return NULL;
            }
            buf = nb;
            cap = newcap;
        }

        size_t n = fread(buf + len, 1, cap - len, stdin);
        len += n;
        if (n == 0)
        {
            break;
        }
    }

    char *nb = realloc(buf, len + 1);
    if (nb == NULL)
    {
        free(buf);
        return NULL;
    }
    buf = nb;
    buf[len] = '\0';
    return buf;
}

int main(int argc, char *argv[])
{
    if (argc == 1)
    {
        printf("%s", useMsg);
        return 0;
    }

    const char *from = NULL;
    const char *to = NULL;
    const char *subject = "";
    const char *body_arg = NULL;
    const char *port = "25";
    const char *helo_host = "localhost";

    opterr = 0;
    int opt;
    while ((opt = getopt(argc, argv, "f:t:s:b:p:H:")) != -1)
    {
        switch (opt)
        {
            case 'f':
                from = optarg;
                break;
            case 't':
                to = optarg;
                break;
            case 's':
                subject = optarg;
                break;
            case 'b':
                body_arg = optarg;
                break;
            case 'p':
                port = optarg;
                break;
            case 'H':
                helo_host = optarg;
                break;
            case '?':
            default:
                if (optopt != 0)
                {
                    fprintf(stderr, "myapp: unrecognized or incomplete option -- '%c'\n",
                            optopt);
                }
                else
                {
                    fprintf(stderr, "myapp: unrecognized option '%s'\n",
                             argv[optind - 1]);
                }
                fprintf(stderr, "%s", useMsg);
                return 1;
        }
    }

    if (optind != argc - 1)
    {
        fprintf(stderr, "myapp: exactly one <server> argument is required\n");
        fprintf(stderr, "%s", useMsg);
        return 1;
    }
    const char *server = argv[optind];

    if (from == NULL || to == NULL)
    {
        fprintf(stderr, "myapp: -f <from> and -t <to> are required\n");
        fprintf(stderr, "%s", useMsg);
        return 1;
    }

    if (smtp_has_crlf_injection(from) || smtp_has_crlf_injection(to) ||
        smtp_has_crlf_injection(subject))
    {
        fprintf(stderr,
                "myapp: -f, -t and -s may not contain a bare CR or LF\n");
        return 1;
    }

    char *body_owned = NULL;
    const char *body = body_arg;
    if (body == NULL)
    {
        body_owned = read_all_stdin();
        if (body_owned == NULL)
        {
            fprintf(stderr, "myapp: out of memory reading message body from stdin\n");
            return 1;
        }
        body = body_owned;
    }

    signal(SIGPIPE, SIG_IGN);

    int fd = smtp_socket_connect(server, port);
    if (fd == -1)
    {
        free(body_owned);
        return 2;
    }

    smtp_transport_t t;
    smtp_transport_init(&t, smtp_socket_read, smtp_socket_write, &fd);

    char errbuf[SMTP_BUF_SIZE];
    int rc = smtp_run_session(&t, helo_host, from, to, subject, body, errbuf,
                               sizeof errbuf);

    smtp_socket_close(fd);
    free(body_owned);

    if (rc != SMTP_OK)
    {
        fprintf(stderr, "myapp: %s\n", errbuf);
        return 2;
    }

    printf("Message queued.\n");
    return 0;
}
