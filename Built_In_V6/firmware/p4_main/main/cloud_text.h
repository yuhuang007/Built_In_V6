#pragma once
#include "volc_conv_ai.h"

/* Same RTC ExternalTextToLLM command as the SDK, with bounded ownership. */
int cloud_text_send(volc_engine_t engine, const char *text);
