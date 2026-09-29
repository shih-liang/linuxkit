#include "os_release.h"
#include <errno.h>
#include <string.h>

int np_os_release_value(const char *text, const char *key, char *out, size_t capacity) {
    if (!text || !key || !*key || !out || !capacity) { errno = EINVAL; return -1; }
    out[0] = 0;
    size_t key_length = strlen(key);
    for (const char *line = text; *line; ) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        const char *next = *end ? end + 1 : end;
        while (line < end && (*line == ' ' || *line == '\t')) line++;
        while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
        if ((size_t)(end - line) > key_length && !memcmp(line, key, key_length) && line[key_length] == '=') {
            const char *value = line + key_length + 1;
            char quote = value < end && (*value == '"' || *value == '\'') ? *value++ : 0;
            if (quote) {
                if (value == end || end[-1] != quote) { errno = EINVAL; return -1; }
                end--;
            }
            size_t n = 0;
            while (value < end) {
                if (*value == '\\' && quote != '\'' && value + 1 < end &&
                    (!quote || strchr("\"\\$`", value[1]))) value++;
                if (n + 1 >= capacity) { out[0] = 0; errno = E2BIG; return -1; }
                out[n++] = *value++;
            }
            out[n] = 0;
            return 1;
        }
        line = next;
    }
    return 0;
}
