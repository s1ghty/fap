#include <string.h>
#include "fap.h"
#include "json.h"

void fap_json_skip_ws(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r')
        (*p)++;
}

int fap_json_string(const char **p, char *out, size_t outsz)
{
    const char *s = *p;
    if (*s != '"')
        return fap_error("json: expected string");
    s++;
    size_t i = 0;
    while (*s && *s != '"') {
        char c = *s;
        if (c == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            default:  c = *s;   break; /* ", \, / and anything else: literal */
            }
        }
        if (i + 1 < outsz)
            out[i++] = c;
        s++;
    }
    if (*s != '"')
        return fap_error("json: unterminated string");
    out[i] = '\0';
    *p = s + 1;
    return 0;
}

const char *fap_json_match(const char *p)
{
    char open = *p, close = (open == '{') ? '}' : ']';
    int depth = 0;
    for (; *p; p++) {
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1])
                    p++;
                p++;
            }
            if (!*p)
                break;
        } else if (*p == open) {
            depth++;
        } else if (*p == close) {
            depth--;
            if (depth == 0)
                return p;
        }
    }
    return NULL;
}

static int json_string_array(const char *arr, const char *arr_end,
                              char dest[][FAP_MAX_NAME], int *count, int max_count,
                              const char *key)
{
    const char *p = arr + 1;
    fap_json_skip_ws(&p);
    while (p < arr_end && *p != ']') {
        if (*count >= max_count)
            return fap_error("json: too many \"%s\" entries (max %d)", key, max_count);
        if (fap_json_string(&p, dest[*count], FAP_MAX_NAME) < 0)
            return -1;
        (*count)++;
        fap_json_skip_ws(&p);
        if (p < arr_end && *p == ',') {
            p++;
            fap_json_skip_ws(&p);
        }
    }
    return 0;
}

int fap_json_optional_array(const char *obj, const char *obj_end, const char *key,
                             char dest[][FAP_MAX_NAME], int *count, int max_count)
{
    const char *v;
    if (fap_json_find(obj, obj_end, key, &v) != 0 || *v != '[')
        return 0;
    const char *v_end = fap_json_match(v);
    if (!v_end)
        return fap_error("json: unterminated \"%s\" array", key);
    return json_string_array(v, v_end, dest, count, max_count, key);
}

int fap_json_find(const char *obj, const char *obj_end,
                   const char *key, const char **out)
{
    size_t klen = strlen(key);
    const char *p = obj;
    while (p < obj_end) {
        if (*p == '"') {
            const char *q = p + 1;
            while (q < obj_end && *q != '"') {
                if (*q == '\\' && q + 1 < obj_end)
                    q++;
                q++;
            }
            if (q >= obj_end)
                return -1;
            if ((size_t)(q - (p + 1)) == klen && strncmp(p + 1, key, klen) == 0) {
                const char *v = q + 1;
                fap_json_skip_ws(&v);
                if (*v == ':') {
                    v++;
                    fap_json_skip_ws(&v);
                    *out = v;
                    return 0;
                }
            }
            p = q + 1;
        } else {
            p++;
        }
    }
    return -1;
}
