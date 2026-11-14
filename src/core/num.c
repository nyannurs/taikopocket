#include "num.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

tp_str tp_trim(tp_str s) {
    while (s.n && tp_isspace(s.p[0])) {
        s.p++;
        s.n--;
    }
    while (s.n && tp_isspace(s.p[s.n - 1])) {
        s.n--;
    }
    return s;
}

bool tp_starts_with(tp_str s, const char *prefix) {
    size_t n = strlen(prefix);
    return s.n >= n && memcmp(s.p, prefix, n) == 0;
}

bool tp_equals(tp_str s, const char *lit) {
    size_t n = strlen(lit);
    return s.n == n && memcmp(s.p, lit, n) == 0;
}

bool tp_contains(tp_str s, char c) { return s.n && memchr(s.p, c, s.n) != NULL; }

static bool word_ci(const char *p, size_t n, const char *word) {
    size_t len = strlen(word);
    if (n < len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != word[i]) {
            return false;
        }
    }
    return true;
}

/* 10^0 .. 10^22 are exact doubles; one multiply or divide by them is correctly rounded. */
static const double pow10_exact[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                       1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                       1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

bool tp_stod(tp_str s, double *out) {
    const char *p = s.p, *end = s.p + s.n;
    while (p < end && tp_isspace(*p)) {
        p++;
    }
    bool neg = false;
    if (p < end && (*p == '+' || *p == '-')) {
        neg = *p == '-';
        p++;
    }
    if (word_ci(p, (size_t)(end - p), "inf")) {
        *out = neg ? -INFINITY : INFINITY;
        return true;
    }
    if (word_ci(p, (size_t)(end - p), "nan")) {
        *out = NAN;
        return true;
    }
    uint64_t mant = 0;
    int digits = 0;     /* significant digits taken into mant */
    int exp10 = 0;      /* decimal exponent applied to mant */
    bool any = false, dot = false;
    for (; p < end; p++) {
        if (tp_isdigit(*p)) {
            any = true;
            if (digits < 19) {
                mant = mant * 10 + (uint64_t)(*p - '0');
                if (mant) {
                    digits++;
                }
                if (dot) {
                    exp10--;
                }
            } else if (!dot) {
                exp10++; /* digits beyond 19 only scale the value */
            }
        } else if (*p == '.' && !dot) {
            dot = true;
        } else {
            break;
        }
    }
    if (!any) {
        return false;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1;
        bool eneg = false;
        if (q < end && (*q == '+' || *q == '-')) {
            eneg = *q == '-';
            q++;
        }
        if (q < end && tp_isdigit(*q)) {
            int e = 0;
            for (; q < end && tp_isdigit(*q); q++) {
                if (e < 10000) {
                    e = e * 10 + (*q - '0');
                }
            }
            exp10 += eneg ? -e : e;
        }
    }
    double v = (double)mant;
    if (mant != 0) {
        if (exp10 < 0) {
            int e = -exp10;
            while (e > 22) {
                v /= 1e22;
                e -= 22;
                if (v == 0) {
                    break;
                }
            }
            v /= pow10_exact[e > 22 ? 22 : e];
        } else {
            int e = exp10;
            while (e > 22) {
                v *= 1e22;
                e -= 22;
                if (isinf(v)) {
                    break;
                }
            }
            v *= pow10_exact[e > 22 ? 22 : e];
        }
    }
    *out = neg ? -v : v;
    return true;
}

bool tp_stof(tp_str s, double *out) {
    double v;
    if (!tp_stod(s, &v)) {
        return false;
    }
    float f = (float)v;
    if (isinf(f) && !isinf(v)) {
        return false; /* std::stof throws out_of_range */
    }
    *out = (double)f;
    return true;
}

bool tp_stoi(tp_str s, int *out, bool *range) {
    const char *p = s.p, *end = s.p + s.n;
    while (p < end && tp_isspace(*p)) {
        p++;
    }
    bool neg = false;
    if (p < end && (*p == '+' || *p == '-')) {
        neg = *p == '-';
        p++;
    }
    if (p >= end || !tp_isdigit(*p)) {
        return false;
    }
    int64_t v = 0;
    bool over = false;
    for (; p < end && tp_isdigit(*p); p++) {
        v = v * 10 + (*p - '0');
        if (v > (int64_t)INT_MAX + 1) {
            over = true;
            v = (int64_t)INT_MAX + 1;
        }
    }
    if (neg) {
        v = -v;
    }
    if (v > INT_MAX || v < INT_MIN) {
        over = true;
    }
    if (range) {
        *range = over;
    }
    *out = v > INT_MAX ? INT_MAX : v < INT_MIN ? INT_MIN : (int)v;
    return true;
}
