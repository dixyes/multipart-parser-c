#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "multipart_parser.h"

#define BOUNDARY_STR "hello world"
#define BUFFER_SIZE 8192

// all the callbacks pause after the current event, so the parser is resumed
// from the returned position; data callbacks are called with the part of the
// event that is available in the current feed, so accumulate one event and
// print it when it ends, keeping the output independent of feed splitting
enum event_kind
{
    e_none,
    e_header_field,
    e_header_value,
    e_part_data,
};

static enum event_kind event_type;
static char event_data[BUFFER_SIZE];
static size_t event_data_len;

// current feed, used to verify the callback span
static const char *feed_base;
static size_t feed_len;

static void flush_event(void) {
    const char *name;
    switch (event_type) {
        case e_header_field:
            name = "HEADER_FIELD";
            break;
        case e_header_value:
            name = "HEADER_VALUE";
            break;
        case e_part_data:
            name = "PART_DATA";
            break;
        default:
            return;
    }
    printf("[%s] \"%.*s\"\n", name, (int)event_data_len, event_data);
    event_type = e_none;
    event_data_len = 0;
}

static int add_event(enum event_kind type, const char *at, size_t length) {
    if (at < feed_base || (size_t)(at - feed_base) > feed_len ||
            length > feed_len - (size_t)(at - feed_base)) {
        fprintf(stderr, "callback span is outside of the current feed\n");
        return -1;
    }
    if (type != event_type) {
        flush_event();
    }
    event_type = type;
    if (length > sizeof(event_data) - event_data_len) {
        fprintf(stderr, "event is too long\n");
        return -1;
    }
    memcpy(&event_data[event_data_len], at, length);
    event_data_len += length;
    return 0;
}

// data callbacks
long on_header_field(multipart_parser *p, const char *at, size_t length) {
    (void)p;
    return add_event(e_header_field, at, length) == 0 ? MPPE_PAUSED : MPPE_ERROR;
}

long on_header_value(multipart_parser *p, const char *at, size_t length) {
    (void)p;
    return add_event(e_header_value, at, length) == 0 ? MPPE_PAUSED : MPPE_ERROR;
}

long on_part_data(multipart_parser *p, const char *at, size_t length) {
    (void)p;
    return add_event(e_part_data, at, length) == 0 ? MPPE_PAUSED : MPPE_ERROR;
}

// notify callbacks
long on_part_data_begin(multipart_parser *p) {
    (void)p;
    flush_event();
    printf("[PART_DATA_BEGIN]\n");
    return MPPE_PAUSED;
}

long on_headers_complete(multipart_parser *p) {
    (void)p;
    flush_event();
    printf("[HEADERS_COMPLETE]\n");
    return MPPE_PAUSED;
}

long on_part_data_end(multipart_parser *p) {
    (void)p;
    flush_event();
    printf("[PART_DATA_END]\n");
    return MPPE_PAUSED;
}

long on_body_end(multipart_parser *p) {
    (void)p;
    flush_event();
    printf("[BODY_END]\n");
    return MPPE_PAUSED;
}

int main(int argc, char **argv) {
    const char *boundary = BOUNDARY_STR;
    size_t read_size = 0;
    if (argc > 1) {
        boundary = argv[1];
    }
    if (argc > 2) {
        read_size = (size_t)strtoul(argv[2], NULL, 10);
    }
    if (read_size == 0) {
        read_size = BUFFER_SIZE;
    }
    size_t boundary_len = strnlen(boundary, BOUNDARY_MAX_LEN);

    multipart_parser_settings settings = { 0 };
    settings.on_header_field = (multipart_data_cb)on_header_field;
    settings.on_header_value = (multipart_data_cb)on_header_value;
    settings.on_part_data = (multipart_data_cb)on_part_data;
    settings.on_part_data_begin = (multipart_notify_cb)on_part_data_begin;
    settings.on_headers_complete = (multipart_notify_cb)on_headers_complete;
    settings.on_part_data_end = (multipart_notify_cb)on_part_data_end;
    settings.on_body_end = (multipart_notify_cb)on_body_end;

    multipart_parser parser;

    if (multipart_parser_init(&parser, boundary, boundary_len, &settings) != 0) {
        perror("multipart_parser_init");
        return 2;
    }

    // keep unconsumed bytes and append the next read behind them, so a token
    // may span any number of feeds
    char buffer[BUFFER_SIZE];
    size_t buffer_len = 0;
    int eof = 0;

    for (;;) {
        size_t parsed;

        while (buffer_len > 0) {
            feed_base = buffer;
            feed_len = buffer_len;
            parsed = multipart_parser_execute(&parser, buffer, buffer_len);
            if (parsed == MPPE_ERROR) {
                char msg[256] = { 0 };
                multipart_parser_error_msg(&parser, msg, sizeof(msg) - 1);
                fprintf(stderr, "failed parse: %s\n", msg);
                return 1;
            }
            if (parsed > buffer_len) {
                fprintf(stderr, "parser consumed more than it was given\n");
                return 1;
            }
            if (parsed == 0) {
                // need more data
                break;
            }
            buffer_len -= parsed;
            memmove(buffer, &buffer[parsed], buffer_len);
        }

        if (eof) {
            if (buffer_len > 0) {
                fprintf(stderr, "incomplete input\n");
                return 1;
            }
            break;
        }

        {
            size_t space = sizeof(buffer) - buffer_len;
            size_t want = read_size < space ? read_size : space;
            ssize_t got;
            if (want == 0) {
                fprintf(stderr, "input does not fit into the buffer\n");
                return 2;
            }
            got = read(0, &buffer[buffer_len], want);
            if (got < 0) {
                perror("read");
                return 2;
            }
            if (got == 0) {
                eof = 1;
            }
            buffer_len += (size_t)got;
        }
    }

    flush_event();
    return 0;
}
