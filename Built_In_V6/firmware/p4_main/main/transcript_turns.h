#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *text;
    size_t capacity, length, turn_start;
} transcript_turns_t;

void transcript_turns_init(transcript_turns_t *model, char *storage, size_t capacity);
void transcript_turns_begin(transcript_turns_t *model);
bool transcript_turns_update(transcript_turns_t *model, const char *hypothesis);
void transcript_turns_commit(transcript_turns_t *model);
bool transcript_turns_append(transcript_turns_t *model, const char *text);
