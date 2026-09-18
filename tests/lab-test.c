#include "harness/unity.h"
#include "../src/lab.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

/* ==================================================================
 * A scripted in-memory "server" used to drive the session layer
 * without a network. `script` is what the server sends back, doled
 * out `chunk` bytes at a time (0 means "as much as the reader asks
 * for"), and everything the client writes is captured in `written`
 * so tests can inspect exactly which commands were sent.
 * ================================================================== */

#define MOCK_WRITTEN_CAP 8192

typedef struct
{
    const char *script;
    size_t script_len;
    size_t pos;
    size_t chunk;
    char written[MOCK_WRITTEN_CAP + 1];
    size_t written_len;
    int write_calls;
    int fail_write_on_call; /* 0 = never fail; N = the Nth write() call fails */
} mock_server_t;

static void mock_init(mock_server_t *m, const char *script, size_t chunk)
{
    memset(m, 0, sizeof *m);
    m->script = script;
    m->script_len = strlen(script);
    m->chunk = chunk;
}

static long mock_read(void *ctx, char *buf, size_t len)
{
    mock_server_t *m = (mock_server_t *)ctx;
    size_t remaining = m->script_len - m->pos;
    if (remaining == 0)
    {
        return 0; /* peer closed / script exhausted */
    }
    size_t want = len;
    if (m->chunk != 0 && m->chunk < want)
    {
        want = m->chunk;
    }
    if (want > remaining)
    {
        want = remaining;
    }
    memcpy(buf, m->script + m->pos, want);
    m->pos += want;
    return (long)want;
}

static long mock_write(void *ctx, const char *buf, size_t len)
{
    mock_server_t *m = (mock_server_t *)ctx;
    m->write_calls++;
    if (m->fail_write_on_call != 0 && m->write_calls == m->fail_write_on_call)
    {
        return -1;
    }
    size_t space = MOCK_WRITTEN_CAP - m->written_len;
    size_t n = (len < space) ? len : space;
    memcpy(m->written + m->written_len, buf, n);
    m->written_len += n;
    m->written[m->written_len] = '\0';
    return (long)len;
}

static long always_fail_read(void *ctx, char *buf, size_t len)
{
    (void)ctx;
    (void)buf;
    (void)len;
    return -1;
}

static int run_session_with_script(const char *script, size_t chunk,
                                    const char *from, const char *to,
                                    const char *subject, const char *body,
                                    mock_server_t *m, char *errbuf,
                                    size_t errbufsize)
{
    mock_init(m, script, chunk);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, m);
    return smtp_run_session(&t, "test-client.example", from, to, subject, body,
                             errbuf, errbufsize);
}

#define GREETING_OK "220 mail.example.com ESMTP ready\r\n"
#define GREETING_MULTI                                                       \
    "220-mail.example.com ESMTP\r\n220-be nice\r\n220 ready for action\r\n"
#define HELO_OK "250 mail.example.com\r\n"
#define MAIL_OK "250 OK\r\n"
#define RCPT_OK "250 OK\r\n"
#define DATA_OK "354 Start input, end with <CRLF>.<CRLF>\r\n"
#define QUEUED_OK "250 Queued as 12345\r\n"
#define QUIT_OK "221 Bye\r\n"

/* ==================================================================
 * Layer 1: pure protocol helpers
 * ================================================================== */

void test_parse_reply_line_final_with_space(void)
{
    int code = 0;
    bool final = false;
    TEST_ASSERT_EQUAL_INT(SMTP_OK,
                           smtp_parse_reply_line("250 OK", &code, &final));
    TEST_ASSERT_EQUAL_INT(250, code);
    TEST_ASSERT_TRUE(final);
}

void test_parse_reply_line_continuation_with_dash(void)
{
    int code = 0;
    bool final = true;
    TEST_ASSERT_EQUAL_INT(
        SMTP_OK, smtp_parse_reply_line("250-more coming", &code, &final));
    TEST_ASSERT_EQUAL_INT(250, code);
    TEST_ASSERT_FALSE(final);
}

void test_parse_reply_line_bare_three_digits_is_final(void)
{
    int code = 0;
    bool final = false;
    TEST_ASSERT_EQUAL_INT(SMTP_OK, smtp_parse_reply_line("250", &code, &final));
    TEST_ASSERT_EQUAL_INT(250, code);
    TEST_ASSERT_TRUE(final);
}

void test_parse_reply_line_rejects_non_digits(void)
{
    int code = 0;
    bool final = false;
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_PARSE,
                           smtp_parse_reply_line("2X0 OK", &code, &final));
}

void test_parse_reply_line_rejects_bad_separator(void)
{
    int code = 0;
    bool final = false;
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_PARSE,
                           smtp_parse_reply_line("250xOK", &code, &final));
}

void test_parse_reply_line_rejects_null_args(void)
{
    int code = 0;
    bool final = false;
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG,
                           smtp_parse_reply_line(NULL, &code, &final));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG,
                           smtp_parse_reply_line("250 OK", NULL, &final));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG,
                           smtp_parse_reply_line("250 OK", &code, NULL));
}

void test_build_command_basic(void)
{
    char buf[64];
    int n = smtp_build_command(buf, sizeof buf, "HELO %s", "myhost");
    TEST_ASSERT_EQUAL_INT((int)strlen("HELO myhost\r\n"), n);
    TEST_ASSERT_EQUAL_STRING("HELO myhost\r\n", buf);
}

void test_build_command_no_substitution(void)
{
    char buf[64];
    int n = smtp_build_command(buf, sizeof buf, "QUIT");
    TEST_ASSERT_EQUAL_INT((int)strlen("QUIT\r\n"), n);
    TEST_ASSERT_EQUAL_STRING("QUIT\r\n", buf);
}

void test_build_command_too_long(void)
{
    char buf[8];
    int n = smtp_build_command(buf, sizeof buf, "MAIL FROM:<%s>",
                                "someone@example.com");
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOOLONG, n);
}

void test_build_command_rejects_null_args(void)
{
    char buf[64];
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, smtp_build_command(NULL, 64, "QUIT"));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG,
                           smtp_build_command(buf, sizeof buf, NULL));
}

void test_crlf_injection_detects_cr(void)
{
    TEST_ASSERT_TRUE(smtp_has_crlf_injection("evil\rMAIL FROM:<x>"));
}

void test_crlf_injection_detects_lf(void)
{
    TEST_ASSERT_TRUE(smtp_has_crlf_injection("evil\nRCPT TO:<x>"));
}

void test_crlf_injection_clean_string_is_false(void)
{
    TEST_ASSERT_FALSE(smtp_has_crlf_injection("perfectly.normal@example.com"));
}

void test_crlf_injection_null_is_false(void)
{
    TEST_ASSERT_FALSE(smtp_has_crlf_injection(NULL));
}

void test_dot_stuff_plain_line_unchanged(void)
{
    char out[64];
    long n = smtp_dot_stuff("Hello\n", out, sizeof out);
    TEST_ASSERT_EQUAL_INT((int)strlen("Hello\r\n"), (int)n);
    TEST_ASSERT_EQUAL_STRING("Hello\r\n", out);
}

void test_dot_stuff_stuffs_leading_dot(void)
{
    char out[64];
    long n = smtp_dot_stuff(".oops\n", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("..oops\r\n", out);
    TEST_ASSERT_EQUAL_INT((int)strlen("..oops\r\n"), (int)n);
}

void test_dot_stuff_multiple_lines(void)
{
    char out[128];
    const char *body = "Hello there.\nThis line starts with a dot:\n.oops\nBye\n";
    long n = smtp_dot_stuff(body, out, sizeof out);
    const char *expect = "Hello there.\r\nThis line starts with a dot:\r\n"
                          "..oops\r\nBye\r\n";
    TEST_ASSERT_EQUAL_STRING(expect, out);
    TEST_ASSERT_EQUAL_INT((int)strlen(expect), (int)n);
}

void test_dot_stuff_normalizes_bare_lf(void)
{
    char out[64];
    smtp_dot_stuff("a\nb\n", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("a\r\nb\r\n", out);
}

void test_dot_stuff_does_not_double_existing_crlf(void)
{
    char out[64];
    smtp_dot_stuff("a\r\nb\r\n", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("a\r\nb\r\n", out);
}

void test_dot_stuff_terminates_line_without_trailing_newline(void)
{
    char out[64];
    smtp_dot_stuff("abc", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("abc\r\n", out);
}

void test_dot_stuff_empty_body(void)
{
    char out[16];
    long n = smtp_dot_stuff("", out, sizeof out);
    TEST_ASSERT_EQUAL_INT(0, (int)n);
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_dot_stuff_buffer_too_small(void)
{
    char out[3];
    long n = smtp_dot_stuff("Hello\n", out, sizeof out);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOOLONG, (int)n);
}

void test_dot_stuff_rejects_null_out(void)
{
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, (int)smtp_dot_stuff("hi", NULL, 10));
}

void test_dot_stuff_overflow_stuffing_the_leading_dot(void)
{
    char out[1];
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOOLONG, (int)smtp_dot_stuff(".", out, 1));
}

void test_dot_stuff_overflow_appending_crlf(void)
{
    char out[2];
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOOLONG, (int)smtp_dot_stuff("A", out, 2));
}

void test_dot_stuff_zero_size_buffer(void)
{
    char out[1];
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOOLONG, (int)smtp_dot_stuff("", out, 0));
}

void test_build_message_full(void)
{
    char out[256];
    long n = smtp_build_message(out, sizeof out, "alice@example.com",
                                 "bob@example.com", "Hi", "Line1\nLine2\n");
    const char *expect = "From: alice@example.com\r\nTo: bob@example.com\r\n"
                          "Subject: Hi\r\n\r\nLine1\r\nLine2\r\n.\r\n";
    TEST_ASSERT_EQUAL_STRING(expect, out);
    TEST_ASSERT_EQUAL_INT((int)strlen(expect), (int)n);
}

void test_build_message_null_subject_and_body(void)
{
    char out[256];
    smtp_build_message(out, sizeof out, "a@x.com", "b@y.com", NULL, NULL);
    const char *expect =
        "From: a@x.com\r\nTo: b@y.com\r\nSubject: \r\n\r\n.\r\n";
    TEST_ASSERT_EQUAL_STRING(expect, out);
}

void test_build_message_buffer_too_small(void)
{
    char out[5];
    long n = smtp_build_message(out, sizeof out, "a@x.com", "b@y.com", "S",
                                 "body");
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOOLONG, (int)n);
}

void test_build_message_propagates_dot_stuff_overflow(void)
{
    char headers_only[128];
    int hn = snprintf(headers_only, sizeof headers_only,
                       "From: %s\r\nTo: %s\r\nSubject: %s\r\n\r\n", "a", "b", "");
    TEST_ASSERT_TRUE(hn > 0);

    char out[128];
    /* room for the headers exactly, and one spare byte -- not enough
     * for the body to be dot-stuffed into. */
    size_t outsize = (size_t)hn + 1;
    long n = smtp_build_message(out, outsize, "a", "b", "", "X");
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOOLONG, (int)n);
}

void test_build_message_no_room_for_terminator(void)
{
    char headers_only[128];
    int hn = snprintf(headers_only, sizeof headers_only,
                       "From: %s\r\nTo: %s\r\nSubject: %s\r\n\r\n", "a", "b", "");
    TEST_ASSERT_TRUE(hn > 0);

    char out[128];
    /* room for the headers plus dot_stuff's own NUL on an empty body,
     * but not enough left over for the ".\r\n" terminator. */
    size_t outsize = (size_t)hn + 1;
    long n = smtp_build_message(out, outsize, "a", "b", "", "");
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOOLONG, (int)n);
}

void test_build_message_rejects_null_args(void)
{
    char out[64];
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG,
                           (int)smtp_build_message(out, sizeof out, NULL,
                                                    "b@y.com", "S", "body"));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG,
                           (int)smtp_build_message(out, sizeof out, "a@x.com",
                                                    NULL, "S", "body"));
}

/* ==================================================================
 * Layer 2: read_line / read_reply / send_command primitives
 * ================================================================== */

void test_read_line_simple(void)
{
    mock_server_t m;
    mock_init(&m, "250 OK\r\n", 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char line[64];
    long n = smtp_read_line(&t, line, sizeof line);
    TEST_ASSERT_EQUAL_INT((int)strlen("250 OK"), (int)n);
    TEST_ASSERT_EQUAL_STRING("250 OK", line);
}

void test_read_line_multiple_lines_share_one_read(void)
{
    mock_server_t m;
    mock_init(&m, "250-a\r\n250 b\r\n", 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char line[64];
    TEST_ASSERT_EQUAL_INT((int)strlen("250-a"),
                           (int)smtp_read_line(&t, line, sizeof line));
    TEST_ASSERT_EQUAL_STRING("250-a", line);
    /* second line must come straight from the buffer, no further "read" */
    size_t pos_before = m.pos;
    TEST_ASSERT_EQUAL_INT((int)strlen("250 b"),
                           (int)smtp_read_line(&t, line, sizeof line));
    TEST_ASSERT_EQUAL_STRING("250 b", line);
    TEST_ASSERT_EQUAL_INT((int)pos_before, (int)m.pos);
}

void test_read_line_split_across_many_reads(void)
{
    mock_server_t m;
    mock_init(&m, "250 a bit at a time\r\n", 1); /* one byte per read() call */
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char line[64];
    long n = smtp_read_line(&t, line, sizeof line);
    TEST_ASSERT_EQUAL_INT((int)strlen("250 a bit at a time"), (int)n);
    TEST_ASSERT_EQUAL_STRING("250 a bit at a time", line);
}

void test_read_line_peer_closed_mid_line(void)
{
    mock_server_t m;
    mock_init(&m, "250 no newline ever comes", 0); /* no \n at all */
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char line[64];
    long n = smtp_read_line(&t, line, sizeof line);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_CLOSED, (int)n);
}

void test_read_line_too_long_for_transport_buffer(void)
{
    static char script[SMTP_BUF_SIZE + 200];
    memset(script, 'x', sizeof script - 1);
    script[sizeof script - 1] = '\0'; /* still no CRLF anywhere */

    mock_server_t m;
    mock_init(&m, script, 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char line[SMTP_BUF_SIZE];
    long n = smtp_read_line(&t, line, sizeof line);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOOLONG, (int)n);
}

void test_read_line_too_long_for_caller_buffer(void)
{
    mock_server_t m;
    mock_init(&m, "250 this line is longer than the tiny caller buffer\r\n", 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char line[8];
    long n = smtp_read_line(&t, line, sizeof line);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOOLONG, (int)n);
}

void test_read_line_rejects_null_args(void)
{
    mock_server_t m;
    mock_init(&m, GREETING_OK, 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char out[8];
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, (int)smtp_read_line(NULL, out, sizeof out));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, (int)smtp_read_line(&t, NULL, sizeof out));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, (int)smtp_read_line(&t, out, 0));
}

void test_read_line_rejects_null_read_callback(void)
{
    smtp_transport_t t;
    smtp_transport_init(&t, NULL, mock_write, NULL);

    char out[64];
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, (int)smtp_read_line(&t, out, sizeof out));
}

void test_read_line_reports_io_error(void)
{
    smtp_transport_t t;
    smtp_transport_init(&t, always_fail_read, mock_write, NULL);

    char out[64];
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_IO, (int)smtp_read_line(&t, out, sizeof out));
}

void test_read_reply_single_line(void)
{
    mock_server_t m;
    mock_init(&m, GREETING_OK, 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char out[128];
    int code = smtp_read_reply(&t, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(220, code);
}

void test_read_reply_multiline_same_code(void)
{
    mock_server_t m;
    mock_init(&m, GREETING_MULTI, 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char out[128];
    int code = smtp_read_reply(&t, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(220, code);
    TEST_ASSERT_EQUAL_STRING("220 ready for action", out);
}

void test_read_reply_multiline_mismatched_code(void)
{
    mock_server_t m;
    mock_init(&m, "250-first\r\n251 second\r\n", 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char out[128];
    int code = smtp_read_reply(&t, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_PROTOCOL, code);
}

void test_read_reply_malformed_line(void)
{
    mock_server_t m;
    mock_init(&m, "not-a-reply\r\n", 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char out[128];
    int code = smtp_read_reply(&t, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_PARSE, code);
}

void test_read_reply_too_many_continuation_lines(void)
{
    static char script[SMTP_MAX_REPLY_LINES * 16 + 16];
    size_t pos = 0;
    for (int i = 0; i < SMTP_MAX_REPLY_LINES; i++)
    {
        int w = snprintf(script + pos, sizeof script - pos, "250-line %d\r\n", i);
        pos += (size_t)w;
    }

    mock_server_t m;
    mock_init(&m, script, 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char out[128];
    int code = smtp_read_reply(&t, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_PROTOCOL, code);
}

void test_read_reply_rejects_null_args(void)
{
    mock_server_t m;
    mock_init(&m, GREETING_OK, 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, smtp_read_reply(NULL, NULL, 0));
    char out[8];
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, smtp_read_reply(&t, out, 0));
}

void test_read_reply_io_error_hits_default_case(void)
{
    smtp_transport_t t;
    smtp_transport_init(&t, always_fail_read, mock_write, NULL);

    char out[64];
    int code = smtp_read_reply(&t, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_IO, code);
    TEST_ASSERT_EQUAL_STRING("connection error while reading server reply", out);
}

void test_read_reply_truncates_into_small_out_buffer(void)
{
    char longtext[101];
    memset(longtext, 'x', sizeof longtext - 1);
    longtext[sizeof longtext - 1] = '\0';
    char script[256];
    snprintf(script, sizeof script, "250 %s\r\n", longtext);

    mock_server_t m;
    mock_init(&m, script, 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char out[10];
    int code = smtp_read_reply(&t, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(250, code);
    TEST_ASSERT_EQUAL_INT(9, (int)strlen(out));
}

void test_transport_init_rejects_null_transport(void)
{
    smtp_transport_init(NULL, mock_read, mock_write, NULL); /* must not crash */
    TEST_PASS();
}

static long always_fail_write(void *ctx, const char *buf, size_t len)
{
    (void)ctx;
    (void)buf;
    (void)len;
    return -1;
}

void test_write_all_rejects_null_args(void)
{
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, NULL);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, smtp_write_all(NULL, "x", 1));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG, smtp_write_all(&t, NULL, 1));
}

void test_write_all_reports_io_error(void)
{
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, always_fail_write, NULL);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_IO, smtp_write_all(&t, "hello", 5));
}

void test_send_command_rejects_null_args(void)
{
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, NULL);
    char reply[64];
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_ARG,
                           smtp_send_command(NULL, "QUIT\r\n", 200, 299, reply,
                                              sizeof reply));
    TEST_ASSERT_EQUAL_INT(
        SMTP_ERR_ARG, smtp_send_command(&t, NULL, 200, 299, reply, sizeof reply));
}

void test_send_command_reports_write_failure(void)
{
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, always_fail_write, NULL);
    char reply[64];
    int code =
        smtp_send_command(&t, "QUIT\r\n", 200, 299, reply, sizeof reply);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_IO, code);
}

void test_send_command_without_reply_buffer_still_works(void)
{
    mock_server_t m;
    mock_init(&m, HELO_OK, 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    int code = smtp_send_command(&t, "HELO me\r\n", 200, 299, NULL, 0);
    TEST_ASSERT_EQUAL_INT(250, code);
}

void test_send_command_success(void)
{
    mock_server_t m;
    mock_init(&m, HELO_OK, 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char reply[128];
    int code = smtp_send_command(&t, "HELO me\r\n", 200, 299, reply,
                                  sizeof reply);
    TEST_ASSERT_EQUAL_INT(250, code);
    TEST_ASSERT_EQUAL_STRING("HELO me\r\n", m.written);
}

void test_send_command_wrong_code(void)
{
    mock_server_t m;
    mock_init(&m, "550 nope\r\n", 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char reply[128];
    int code = smtp_send_command(&t, "MAIL FROM:<a@b.com>\r\n", 200, 299,
                                  reply, sizeof reply);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_PROTOCOL, code);
    TEST_ASSERT_EQUAL_STRING("550 nope", reply);
}

/* ==================================================================
 * Layer 2: the whole session
 * ================================================================== */

void test_session_happy_path(void)
{
    const char *script = GREETING_OK HELO_OK MAIL_OK RCPT_OK DATA_OK
        QUEUED_OK QUIT_OK;
    mock_server_t m;
    char errbuf[256];
    int rc = run_session_with_script(script, 0, "alice@example.com",
                                      "bob@example.com", "Hi",
                                      "Hello\nWorld\n", &m, errbuf,
                                      sizeof errbuf);

    TEST_ASSERT_EQUAL_INT(SMTP_OK, rc);
    TEST_ASSERT_NOT_NULL(strstr(m.written, "HELO test-client.example\r\n"));
    TEST_ASSERT_NOT_NULL(
        strstr(m.written, "MAIL FROM:<alice@example.com>\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(m.written, "RCPT TO:<bob@example.com>\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(m.written, "DATA\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(
        m.written, "From: alice@example.com\r\nTo: bob@example.com\r\n"
                   "Subject: Hi\r\n\r\nHello\r\nWorld\r\n.\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(m.written, "QUIT\r\n"));
}

void test_session_rejects_dot_stuffed_body(void)
{
    const char *script = GREETING_OK HELO_OK MAIL_OK RCPT_OK DATA_OK
        QUEUED_OK QUIT_OK;
    mock_server_t m;
    char errbuf[256];
    int rc = run_session_with_script(script, 0, "a@x.com", "b@y.com", "S",
                                      ".sneaky line\n", &m, errbuf,
                                      sizeof errbuf);
    TEST_ASSERT_EQUAL_INT(SMTP_OK, rc);
    TEST_ASSERT_NOT_NULL(strstr(m.written, "\r\n\r\n..sneaky line\r\n.\r\n"));
}

void test_session_multiline_greeting(void)
{
    const char *script = GREETING_MULTI HELO_OK MAIL_OK RCPT_OK DATA_OK
        QUEUED_OK QUIT_OK;
    mock_server_t m;
    char errbuf[256];
    int rc = run_session_with_script(script, 0, "a@x.com", "b@y.com", "S",
                                      "body\n", &m, errbuf, sizeof errbuf);
    TEST_ASSERT_EQUAL_INT(SMTP_OK, rc);
}

void test_session_replies_arrive_a_few_bytes_at_a_time(void)
{
    const char *script = GREETING_OK HELO_OK MAIL_OK RCPT_OK DATA_OK
        QUEUED_OK QUIT_OK;
    mock_server_t m;
    char errbuf[256];
    int rc = run_session_with_script(script, 1, "a@x.com", "b@y.com", "S",
                                      "body\n", &m, errbuf, sizeof errbuf);
    TEST_ASSERT_EQUAL_INT(SMTP_OK, rc);
}

void test_session_reply_too_long_for_buffer(void)
{
    static char script[SMTP_BUF_SIZE + 200];
    memset(script, 'x', sizeof script - 1);
    script[sizeof script - 1] = '\0'; /* greeting line with no CRLF ever */

    mock_server_t m;
    char errbuf[256];
    int rc = run_session_with_script(script, 0, "a@x.com", "b@y.com", "S",
                                      "body\n", &m, errbuf, sizeof errbuf);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOOLONG, rc);
    TEST_ASSERT_TRUE(strlen(errbuf) > 0);
}

void test_session_server_hangs_up_mid_session(void)
{
    /* Closes right after RCPT TO succeeds, before a DATA reply arrives. */
    const char *script = GREETING_OK HELO_OK MAIL_OK RCPT_OK;
    mock_server_t m;
    char errbuf[256];
    int rc = run_session_with_script(script, 0, "a@x.com", "b@y.com", "S",
                                      "body\n", &m, errbuf, sizeof errbuf);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_CLOSED, rc);
    TEST_ASSERT_NOT_NULL(strstr(m.written, "DATA\r\n"));
    TEST_ASSERT_NULL(strstr(m.written, "From: "));
}

void test_session_wrong_code_at_greeting(void)
{
    const char *script = "421 Service not available\r\n";
    mock_server_t m;
    char errbuf[256];
    int rc = run_session_with_script(script, 0, "a@x.com", "b@y.com", "S",
                                      "body\n", &m, errbuf, sizeof errbuf);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_PROTOCOL, rc);
    TEST_ASSERT_EQUAL_INT(0, (int)m.written_len);
}

void test_session_wrong_code_at_helo(void)
{
    const char *script = GREETING_OK "500 Command not recognized\r\n";
    mock_server_t m;
    char errbuf[256];
    int rc = run_session_with_script(script, 0, "a@x.com", "b@y.com", "S",
                                      "body\n", &m, errbuf, sizeof errbuf);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_PROTOCOL, rc);
    TEST_ASSERT_NOT_NULL(strstr(m.written, "HELO test-client.example\r\n"));
    TEST_ASSERT_NULL(strstr(m.written, "MAIL FROM"));
}

void test_session_wrong_code_at_mail_from(void)
{
    const char *script = GREETING_OK HELO_OK "550 Mailbox unavailable\r\n";
    mock_server_t m;
    char errbuf[256];
    int rc = run_session_with_script(script, 0, "a@x.com", "b@y.com", "S",
                                      "body\n", &m, errbuf, sizeof errbuf);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_PROTOCOL, rc);
    TEST_ASSERT_NOT_NULL(strstr(m.written, "MAIL FROM:<a@x.com>\r\n"));
    TEST_ASSERT_NULL(strstr(m.written, "RCPT TO"));
}

void test_session_wrong_code_at_rcpt_to(void)
{
    const char *script = GREETING_OK HELO_OK MAIL_OK "551 User not local\r\n";
    mock_server_t m;
    char errbuf[256];
    int rc = run_session_with_script(script, 0, "a@x.com", "b@y.com", "S",
                                      "body\n", &m, errbuf, sizeof errbuf);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_PROTOCOL, rc);
    TEST_ASSERT_NOT_NULL(strstr(m.written, "RCPT TO:<b@y.com>\r\n"));
    TEST_ASSERT_NULL(strstr(m.written, "DATA\r\n"));
}

void test_session_wrong_code_at_data(void)
{
    const char *script =
        GREETING_OK HELO_OK MAIL_OK RCPT_OK "503 Bad sequence\r\n";
    mock_server_t m;
    char errbuf[256];
    int rc = run_session_with_script(script, 0, "a@x.com", "b@y.com", "S",
                                      "body\n", &m, errbuf, sizeof errbuf);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_PROTOCOL, rc);
    TEST_ASSERT_NOT_NULL(strstr(m.written, "DATA\r\n"));
    TEST_ASSERT_NULL(strstr(m.written, "From: "));
}

void test_session_helo_host_too_long(void)
{
    static char long_host[2000];
    memset(long_host, 'h', sizeof long_host - 1);
    long_host[sizeof long_host - 1] = '\0';

    mock_server_t m;
    mock_init(&m, GREETING_OK, 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char errbuf[256];
    int rc = smtp_run_session(&t, long_host, "a@x.com", "b@y.com", "S",
                               "body\n", errbuf, sizeof errbuf);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOOLONG, rc);
}

void test_session_from_address_too_long(void)
{
    static char long_from[2000];
    memset(long_from, 'a', sizeof long_from - 1);
    long_from[sizeof long_from - 1] = '\0';

    const char *script = GREETING_OK HELO_OK;
    mock_server_t m;
    char errbuf[256];
    int rc = run_session_with_script(script, 0, long_from, "b@y.com", "S",
                                      "body\n", &m, errbuf, sizeof errbuf);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOOLONG, rc);
}

void test_session_to_address_too_long(void)
{
    static char long_to[2000];
    memset(long_to, 'b', sizeof long_to - 1);
    long_to[sizeof long_to - 1] = '\0';

    const char *script = GREETING_OK HELO_OK MAIL_OK;
    mock_server_t m;
    char errbuf[256];
    int rc = run_session_with_script(script, 0, "a@x.com", long_to, "S",
                                      "body\n", &m, errbuf, sizeof errbuf);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOOLONG, rc);
}

void test_session_rejects_null_args(void)
{
    mock_server_t m;
    mock_init(&m, GREETING_OK, 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);
    char errbuf[64];

    TEST_ASSERT_EQUAL_INT(
        SMTP_ERR_ARG, smtp_run_session(NULL, "host", "a@x.com", "b@y.com", "S",
                                        "body", errbuf, sizeof errbuf));
    TEST_ASSERT_EQUAL_INT(
        SMTP_ERR_ARG, smtp_run_session(&t, NULL, "a@x.com", "b@y.com", "S",
                                        "body", errbuf, sizeof errbuf));
    TEST_ASSERT_EQUAL_INT(
        SMTP_ERR_ARG, smtp_run_session(&t, "host", NULL, "b@y.com", "S",
                                        "body", errbuf, sizeof errbuf));
    TEST_ASSERT_EQUAL_INT(
        SMTP_ERR_ARG, smtp_run_session(&t, "host", "a@x.com", NULL, "S",
                                        "body", errbuf, sizeof errbuf));
}

void test_session_handles_missing_errbuf(void)
{
    /* Fails fast at the greeting, so no script beyond it is needed;
     * exercises the errbuf==NULL/0 substitution path. */
    mock_server_t m;
    mock_init(&m, "421 no\r\n", 0);
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    int rc = smtp_run_session(&t, "host", "a@x.com", "b@y.com", "S", "body",
                               NULL, 0);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_PROTOCOL, rc);
}

void test_session_message_write_fails(void)
{
    const char *script = GREETING_OK HELO_OK MAIL_OK RCPT_OK DATA_OK;
    mock_server_t m;
    mock_init(&m, script, 0);
    /* Writes: HELO(1), MAIL FROM(2), RCPT TO(3), DATA(4), message(5). */
    m.fail_write_on_call = 5;
    smtp_transport_t t;
    smtp_transport_init(&t, mock_read, mock_write, &m);

    char errbuf[256];
    int rc = smtp_run_session(&t, "test-client.example", "a@x.com",
                               "b@y.com", "S", "body\n", errbuf, sizeof errbuf);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_IO, rc);
}

void test_session_hangs_up_after_message_before_confirmation(void)
{
    /* Ends right after the 354 DATA prompt -- the message gets sent,
     * but the server vanishes before confirming it was queued. */
    const char *script = GREETING_OK HELO_OK MAIL_OK RCPT_OK DATA_OK;
    mock_server_t m;
    char errbuf[256];
    int rc = run_session_with_script(script, 0, "a@x.com", "b@y.com", "S",
                                      "body\n", &m, errbuf, sizeof errbuf);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_CLOSED, rc);
    TEST_ASSERT_NOT_NULL(strstr(m.written, "From: a@x.com\r\n"));
}

void test_session_wrong_code_after_message(void)
{
    const char *script = GREETING_OK HELO_OK MAIL_OK RCPT_OK DATA_OK
        "554 Transaction failed\r\n";
    mock_server_t m;
    char errbuf[256];
    int rc = run_session_with_script(script, 0, "a@x.com", "b@y.com", "S",
                                      "body\n", &m, errbuf, sizeof errbuf);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_PROTOCOL, rc);
    TEST_ASSERT_NOT_NULL(strstr(m.written, "From: a@x.com\r\n"));
    TEST_ASSERT_NULL(strstr(m.written, "QUIT\r\n"));
}

/* ==================================================================
 * Layer 3: the socket transport, tested over the loopback interface
 * so no real network or mail server is involved.
 * ================================================================== */

void test_socket_connect_and_read_write_over_loopback(void)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        TEST_IGNORE_MESSAGE("could not create a loopback socket in this sandbox");
        return;
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* let the kernel pick a free port */

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(listen_fd, 1) != 0)
    {
        close(listen_fd);
        TEST_IGNORE_MESSAGE("could not bind/listen on loopback in this sandbox");
        return;
    }

    socklen_t alen = sizeof addr;
    TEST_ASSERT_EQUAL_INT(
        0, getsockname(listen_fd, (struct sockaddr *)&addr, &alen));
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", (int)ntohs(addr.sin_port));

    pid_t pid = fork();
    TEST_ASSERT_TRUE(pid >= 0);
    if (pid == 0)
    {
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd >= 0)
        {
            char buf[16] = {0};
            ssize_t rn = read(cfd, buf, sizeof buf);
            (void)rn;
            ssize_t wn = write(cfd, "PONG\r\n", 6);
            (void)wn;
            close(cfd);
        }
        close(listen_fd);
        _exit(0);
    }

    int fd = smtp_socket_connect("127.0.0.1", portstr);
    TEST_ASSERT_TRUE(fd >= 0);

    long wrote = smtp_socket_write(&fd, "PING\r\n", 6);
    TEST_ASSERT_EQUAL_INT(6, (int)wrote);

    char rbuf[16] = {0};
    long got = smtp_socket_read(&fd, rbuf, sizeof rbuf - 1);
    TEST_ASSERT_TRUE(got > 0);
    rbuf[got] = '\0';
    TEST_ASSERT_EQUAL_STRING("PONG\r\n", rbuf);

    smtp_socket_close(fd);
    close(listen_fd);
    int status = 0;
    waitpid(pid, &status, 0);
}

void test_socket_connect_refused(void)
{
    /* Nothing listens on loopback port 1; connect should fail cleanly. */
    int fd = smtp_socket_connect("127.0.0.1", "1");
    TEST_ASSERT_EQUAL_INT(-1, fd);
}

void test_socket_connect_rejects_null_args(void)
{
    TEST_ASSERT_EQUAL_INT(-1, smtp_socket_connect(NULL, "25"));
    TEST_ASSERT_EQUAL_INT(-1, smtp_socket_connect("127.0.0.1", NULL));
}

void test_socket_connect_bad_service_name(void)
{
    /* Resolving a service name is a local lookup (no DNS needed), so
     * this fails fast and deterministically. */
    int fd = smtp_socket_connect("127.0.0.1", "not-a-real-service-name");
    TEST_ASSERT_EQUAL_INT(-1, fd);
}

void test_socket_read_write_error_on_closed_fd(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        TEST_IGNORE_MESSAGE("could not create a socket in this sandbox");
        return;
    }
    close(fd); /* fd is now invalid */

    char buf[4];
    TEST_ASSERT_EQUAL_INT(-1, (int)smtp_socket_read(&fd, buf, sizeof buf));
    TEST_ASSERT_EQUAL_INT(-1, (int)smtp_socket_write(&fd, "hi", 2));
}

void test_socket_close_accepts_negative_fd(void)
{
    smtp_socket_close(-1); /* must not crash */
    TEST_PASS();
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_parse_reply_line_final_with_space);
    RUN_TEST(test_parse_reply_line_continuation_with_dash);
    RUN_TEST(test_parse_reply_line_bare_three_digits_is_final);
    RUN_TEST(test_parse_reply_line_rejects_non_digits);
    RUN_TEST(test_parse_reply_line_rejects_bad_separator);
    RUN_TEST(test_parse_reply_line_rejects_null_args);

    RUN_TEST(test_build_command_basic);
    RUN_TEST(test_build_command_no_substitution);
    RUN_TEST(test_build_command_too_long);
    RUN_TEST(test_build_command_rejects_null_args);

    RUN_TEST(test_crlf_injection_detects_cr);
    RUN_TEST(test_crlf_injection_detects_lf);
    RUN_TEST(test_crlf_injection_clean_string_is_false);
    RUN_TEST(test_crlf_injection_null_is_false);

    RUN_TEST(test_dot_stuff_plain_line_unchanged);
    RUN_TEST(test_dot_stuff_stuffs_leading_dot);
    RUN_TEST(test_dot_stuff_multiple_lines);
    RUN_TEST(test_dot_stuff_normalizes_bare_lf);
    RUN_TEST(test_dot_stuff_does_not_double_existing_crlf);
    RUN_TEST(test_dot_stuff_terminates_line_without_trailing_newline);
    RUN_TEST(test_dot_stuff_empty_body);
    RUN_TEST(test_dot_stuff_buffer_too_small);
    RUN_TEST(test_dot_stuff_rejects_null_out);
    RUN_TEST(test_dot_stuff_overflow_stuffing_the_leading_dot);
    RUN_TEST(test_dot_stuff_overflow_appending_crlf);
    RUN_TEST(test_dot_stuff_zero_size_buffer);

    RUN_TEST(test_build_message_full);
    RUN_TEST(test_build_message_null_subject_and_body);
    RUN_TEST(test_build_message_buffer_too_small);
    RUN_TEST(test_build_message_propagates_dot_stuff_overflow);
    RUN_TEST(test_build_message_no_room_for_terminator);
    RUN_TEST(test_build_message_rejects_null_args);

    RUN_TEST(test_read_line_simple);
    RUN_TEST(test_read_line_multiple_lines_share_one_read);
    RUN_TEST(test_read_line_split_across_many_reads);
    RUN_TEST(test_read_line_peer_closed_mid_line);
    RUN_TEST(test_read_line_too_long_for_transport_buffer);
    RUN_TEST(test_read_line_too_long_for_caller_buffer);
    RUN_TEST(test_read_line_rejects_null_args);
    RUN_TEST(test_read_line_rejects_null_read_callback);
    RUN_TEST(test_read_line_reports_io_error);

    RUN_TEST(test_read_reply_single_line);
    RUN_TEST(test_read_reply_multiline_same_code);
    RUN_TEST(test_read_reply_multiline_mismatched_code);
    RUN_TEST(test_read_reply_malformed_line);
    RUN_TEST(test_read_reply_too_many_continuation_lines);
    RUN_TEST(test_read_reply_rejects_null_args);
    RUN_TEST(test_read_reply_io_error_hits_default_case);
    RUN_TEST(test_read_reply_truncates_into_small_out_buffer);

    RUN_TEST(test_transport_init_rejects_null_transport);
    RUN_TEST(test_write_all_rejects_null_args);
    RUN_TEST(test_write_all_reports_io_error);

    RUN_TEST(test_send_command_rejects_null_args);
    RUN_TEST(test_send_command_reports_write_failure);
    RUN_TEST(test_send_command_without_reply_buffer_still_works);
    RUN_TEST(test_send_command_success);
    RUN_TEST(test_send_command_wrong_code);

    RUN_TEST(test_session_happy_path);
    RUN_TEST(test_session_rejects_dot_stuffed_body);
    RUN_TEST(test_session_multiline_greeting);
    RUN_TEST(test_session_replies_arrive_a_few_bytes_at_a_time);
    RUN_TEST(test_session_reply_too_long_for_buffer);
    RUN_TEST(test_session_server_hangs_up_mid_session);
    RUN_TEST(test_session_wrong_code_at_greeting);
    RUN_TEST(test_session_wrong_code_at_helo);
    RUN_TEST(test_session_wrong_code_at_mail_from);
    RUN_TEST(test_session_wrong_code_at_rcpt_to);
    RUN_TEST(test_session_wrong_code_at_data);
    RUN_TEST(test_session_wrong_code_after_message);
    RUN_TEST(test_session_helo_host_too_long);
    RUN_TEST(test_session_from_address_too_long);
    RUN_TEST(test_session_to_address_too_long);
    RUN_TEST(test_session_rejects_null_args);
    RUN_TEST(test_session_handles_missing_errbuf);
    RUN_TEST(test_session_message_write_fails);
    RUN_TEST(test_session_hangs_up_after_message_before_confirmation);

    RUN_TEST(test_socket_connect_and_read_write_over_loopback);
    RUN_TEST(test_socket_connect_refused);
    RUN_TEST(test_socket_connect_rejects_null_args);
    RUN_TEST(test_socket_connect_bad_service_name);
    RUN_TEST(test_socket_read_write_error_on_closed_fd);
    RUN_TEST(test_socket_close_accepts_negative_fd);

    return UNITY_END();
}