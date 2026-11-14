/* Number parsing with the semantics of std::stof / std::stod / std::stoi, which
 * YataiDON's parser uses: skip leading white space, read the longest valid
 * prefix, fail when there is none. Strings are slices (not NUL-terminated).
 * Self-contained so the Pocket build does not depend on a libc's strtod. */
#ifndef TP_NUM_H
#define TP_NUM_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *p;
    size_t n;
} tp_str;

static inline tp_str tp_str_make(const char *p, size_t n) {
    tp_str s = {p, n};
    return s;
}

/* Each returns false when no number could be read ("invalid_argument"). */
bool tp_stod(tp_str s, double *out);
bool tp_stof(tp_str s, double *out);         /* value rounded to float, widened to double */
bool tp_stoi(tp_str s, int *out, bool *range); /* *range set when the value overflowed */

/* isspace() / isdigit() / isalpha() of the "C" locale */
static inline bool tp_isspace(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline bool tp_isdigit(char c) { return c >= '0' && c <= '9'; }
static inline bool tp_isalpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

tp_str tp_trim(tp_str s); /* std::isspace on both ends */
bool tp_starts_with(tp_str s, const char *prefix);
bool tp_equals(tp_str s, const char *lit);
bool tp_contains(tp_str s, char c);

#endif
