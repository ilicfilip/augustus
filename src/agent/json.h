#ifndef AGENT_JSON_H
#define AGENT_JSON_H

#include <stdint.h>

/**
 * Minimal JSON writer into a fixed buffer. Commas are inserted automatically.
 * Output is truncated (and marked invalid) if the buffer is too small.
 */
typedef struct {
    char *buf;
    int size;
    int len;
    int needs_comma;
    int overflow;
} json_writer;

void json_init(json_writer *w, char *buf, int size);
void json_object_start(json_writer *w, const char *key);
void json_object_end(json_writer *w);
void json_array_start(json_writer *w, const char *key);
void json_array_end(json_writer *w);
void json_int(json_writer *w, const char *key, int value);
void json_bool(json_writer *w, const char *key, int value);
void json_string(json_writer *w, const char *key, const char *value);
/** Writes a game-encoded string, converted to UTF-8 */
void json_game_string(json_writer *w, const char *key, const uint8_t *value);

#endif // AGENT_JSON_H
