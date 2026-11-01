#include "transcript_turns.h"
#include <string.h>

void transcript_turns_init(transcript_turns_t *model, char *storage, size_t capacity)
{
    *model = (transcript_turns_t){.text = storage, .capacity = capacity};
    if (storage && capacity) storage[0] = 0;
}

void transcript_turns_begin(transcript_turns_t *model) { model->turn_start = model->length; }
void transcript_turns_commit(transcript_turns_t *model) { model->turn_start = model->length; }

bool transcript_turns_update(transcript_turns_t *model, const char *hypothesis)
{
    if (!model || !model->text || !hypothesis || model->turn_start >= model->capacity) return false;
    size_t bytes = strlen(hypothesis);
    size_t separator = bytes && model->turn_start ? 1 : 0;
    size_t available = model->capacity - model->turn_start - 1;
    if (separator > available || bytes > available - separator) return false;
    char *destination = model->text + model->turn_start;
    if (separator) *destination++ = '\n';
    memcpy(destination, hypothesis, bytes + 1);
    model->length = model->turn_start + separator + bytes;
    return true;
}

bool transcript_turns_append(transcript_turns_t *model, const char *text)
{
    if (!model) return false;
    size_t previous = model->turn_start;
    transcript_turns_begin(model);
    if (!transcript_turns_update(model, text)) { model->turn_start = previous; return false; }
    transcript_turns_commit(model);
    return true;
}
