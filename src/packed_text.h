/* Built-in text, stored losslessly in a compact, shared table. */
#ifndef CLAWC_PACKED_TEXT_H
#define CLAWC_PACKED_TEXT_H

#include "config.h"
#include "util.h"

enum {
#define TEXT(id, text) id,
#include "packed_text.def"
#undef TEXT
    PACKED_TEXT_COUNT
};
const char *packed_text(int id);
#endif
