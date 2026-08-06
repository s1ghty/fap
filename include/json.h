#ifndef FAP_JSON_H
#define FAP_JSON_H

#include <stddef.h>
#include "fap.h"

/*
 * json.h — minimal JSON scanning helpers shared by lock.c and registry.c.
 *
 * Not a general-purpose parser: just enough to walk flat objects/arrays
 * of the fixed shapes used by fap.lock and the channel index format.
 */

void fap_json_skip_ws(const char **p);

/* *p must point at the opening '"'; copies the decoded string into out
 * and advances *p past the closing '"'. */
int fap_json_string(const char **p, char *out, size_t outsz);

/* *p must point at an opening '{' or '['; returns a pointer to its
 * matching close, skipping over the contents of any JSON strings. */
const char *fap_json_match(const char *p);

/* Search [obj, obj_end) for "key" and point *out at its value,
 * skipping leading whitespace. Returns -1 if the key isn't found. */
int fap_json_find(const char *obj, const char *obj_end,
                   const char *key, const char **out);

/* Parses an optional JSON array of strings under "key" within
 * [obj, obj_end) into dest[0..*count), FAP_MAX_NAME wide each,
 * incrementing *count per entry. A missing key is not an error (it
 * just leaves *count unchanged); a present-but-malformed or
 * over-long array is. Used for "bin"/"deps" style fields. */
int fap_json_optional_array(const char *obj, const char *obj_end, const char *key,
                             char dest[][FAP_MAX_NAME], int *count, int max_count);

#endif /* FAP_JSON_H */
