#include "json.h"

#include "core/encoding.h"

#include <stdio.h>

void json_init(json_writer *w, char *buf, int size)
{
    w->buf = buf;
    w->size = size;
    w->len = 0;
    w->needs_comma = 0;
    w->overflow = 0;
    buf[0] = 0;
}

static void append(json_writer *w, const char *text)
{
    while (*text) {
        if (w->len >= w->size - 1) {
            w->overflow = 1;
            break;
        }
        w->buf[w->len++] = *text++;
    }
    w->buf[w->len] = 0;
}

static void append_escaped(json_writer *w, const char *text)
{
    char tmp[8];
    append(w, "\"");
    for (; *text; text++) {
        unsigned char c = (unsigned char) *text;
        if (c == '"' || c == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char) c;
            tmp[2] = 0;
            append(w, tmp);
        } else if (c < 0x20) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", c);
            append(w, tmp);
        } else {
            tmp[0] = (char) c;
            tmp[1] = 0;
            append(w, tmp);
        }
    }
    append(w, "\"");
}

static void key(json_writer *w, const char *name)
{
    if (w->needs_comma) {
        append(w, ",");
    }
    if (name) {
        append_escaped(w, name);
        append(w, ":");
    }
    w->needs_comma = 1;
}

void json_object_start(json_writer *w, const char *name)
{
    key(w, name);
    append(w, "{");
    w->needs_comma = 0;
}

void json_object_end(json_writer *w)
{
    append(w, "}");
    w->needs_comma = 1;
}

void json_array_start(json_writer *w, const char *name)
{
    key(w, name);
    append(w, "[");
    w->needs_comma = 0;
}

void json_array_end(json_writer *w)
{
    append(w, "]");
    w->needs_comma = 1;
}

void json_int(json_writer *w, const char *name, int value)
{
    char tmp[16];
    key(w, name);
    snprintf(tmp, sizeof(tmp), "%d", value);
    append(w, tmp);
}

void json_bool(json_writer *w, const char *name, int value)
{
    key(w, name);
    append(w, value ? "true" : "false");
}

void json_string(json_writer *w, const char *name, const char *value)
{
    key(w, name);
    append_escaped(w, value ? value : "");
}

void json_game_string(json_writer *w, const char *name, const uint8_t *value)
{
    char utf8[512];
    utf8[0] = 0;
    if (value) {
        encoding_to_utf8(value, utf8, sizeof(utf8), 0);
    }
    json_string(w, name, utf8);
}
