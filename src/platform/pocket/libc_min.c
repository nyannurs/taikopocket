/* The little C library the firmware needs. The game is freestanding on the
 * Pocket: no libc and no libgcc are linked (toolchains rarely ship them for
 * rv32imafdc/ilp32d), so string functions, snprintf, a few math functions and
 * the 64-bit helpers the compiler may call live here. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* The renderer copies pixel runs that start at any 16-bit position, so source and
 * destination are often aligned differently. Words are copied in every case: the
 * destination is brought to a word boundary, and a source that is then off by 1 to 3
 * bytes is read as aligned words and shifted together (little endian). A byte loop
 * here cost the Pocket most of its frame. */
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (n >= 16) {
        while ((uintptr_t)d & 3) {
            *d++ = *s++;
            n--;
        }
        uint32_t *dw = (uint32_t *)d;
        unsigned off = (unsigned)((uintptr_t)s & 3);
        if (off == 0) {
            const uint32_t *sw = (const uint32_t *)s;
            for (; n >= 16; n -= 16) {
                uint32_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
                dw[0] = a;
                dw[1] = b;
                dw[2] = c;
                dw[3] = e;
                dw += 4;
                sw += 4;
            }
            for (; n >= 4; n -= 4) {
                *dw++ = *sw++;
            }
            s = (const uint8_t *)sw;
        } else {
            /* the word holding s[0] and the one after it both hold bytes that are needed,
             * so reading them whole never leaves the words the source occupies */
            const uint32_t *sw = (const uint32_t *)(s - off);
            unsigned r = off * 8, l = 32 - r;
            uint32_t prev = *sw++;
            for (; n >= 8; n -= 8) {
                uint32_t a = sw[0], b = sw[1];
                dw[0] = (prev >> r) | (a << l);
                dw[1] = (a >> r) | (b << l);
                prev = b;
                dw += 2;
                sw += 2;
                s += 8;
            }
            if (n >= 4) {
                uint32_t a = *sw;
                *dw++ = (prev >> r) | (a << l);
                s += 4;
                n -= 4;
            }
        }
        d = (uint8_t *)dw;
    }
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d <= s || d >= s + n) {
        return memcpy(dst, src, n);
    }
    /* overlapping, moving up: from the end, in words where both ends are aligned alike
     * (the judge opens a gap in an array of pointers for every note of a branch) */
    if ((((uintptr_t)d ^ (uintptr_t)s) & 3) == 0) {
        while (n && ((uintptr_t)(d + n) & 3)) {
            n--;
            d[n] = s[n];
        }
        while (n >= 4) {
            n -= 4;
            *(uint32_t *)(void *)(d + n) = *(const uint32_t *)(const void *)(s + n);
        }
    }
    while (n--) {
        d[n] = s[n];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    while (n && ((uintptr_t)d & 3)) {
        *d++ = (uint8_t)c;
        n--;
    }
    uint32_t w = (uint8_t)c * 0x01010101u;
    for (; n >= 4; n -= 4, d += 4) {
        *(uint32_t *)d = w;
    }
    while (n--) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++) {
        if (*x != *y) {
            return *x - *y;
        }
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = s;
    for (; n; n--, p++) {
        if (*p == (uint8_t)c) {
            return (void *)p;
        }
    }
    return NULL;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a) {
            return (unsigned char)*a - (unsigned char)*b;
        }
    }
    return 0;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) {
            return (char *)s;
        }
        if (!*s) {
            return NULL;
        }
    }
}

/* ---- snprintf: %d %i %u %x %s %c %%, with + 0 width and l / ll ---- */
static void put(char *out, size_t cap, size_t *n, char c) {
    if (*n + 1 < cap) {
        out[*n] = c;
    }
    (*n)++;
}

int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap) {
    size_t n = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(out, cap, &n, *fmt);
            continue;
        }
        fmt++;
        int plus = 0, zero = 0, width = 0, longs = 0;
        for (;; fmt++) {
            if (*fmt == '+') {
                plus = 1;
            } else if (*fmt == '0') {
                zero = 1;
            } else {
                break;
            }
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l') {
            longs++;
            fmt++;
        }
        char digits[24];
        int len = 0, negative = 0;
        unsigned long long value = 0;
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            for (s = s ? s : "(null)"; *s; s++) {
                put(out, cap, &n, *s);
            }
            continue;
        }
        case 'c': put(out, cap, &n, (char)va_arg(ap, int)); continue;
        case '%': put(out, cap, &n, '%'); continue;
        case 'd':
        case 'i': {
            long long v = longs >= 2 ? va_arg(ap, long long) : longs ? va_arg(ap, long) : va_arg(ap, int);
            negative = v < 0;
            value = negative ? 0ull - (unsigned long long)v : (unsigned long long)v;
            break;
        }
        case 'u':
        case 'x':
            value = longs >= 2 ? va_arg(ap, unsigned long long) : longs ? va_arg(ap, unsigned long)
                                                                        : va_arg(ap, unsigned int);
            break;
        case 0: /* the format ends in a lone '%' */
            if (cap) {
                out[n < cap ? n : cap - 1] = 0;
            }
            return (int)n;
        default: continue;
        }
        unsigned base = *fmt == 'x' ? 16 : 10;
        do {
            digits[len++] = "0123456789abcdef"[value % base];
            value /= base;
        } while (value);
        int sign = negative ? '-' : plus ? '+' : 0;
        int pad = width - len - (sign ? 1 : 0);
        if (!zero) {
            for (; pad > 0; pad--) {
                put(out, cap, &n, ' ');
            }
        }
        if (sign) {
            put(out, cap, &n, (char)sign);
        }
        for (; pad > 0; pad--) {
            put(out, cap, &n, '0');
        }
        while (len) {
            put(out, cap, &n, digits[--len]);
        }
    }
    if (cap) {
        out[n < cap ? n : cap - 1] = 0;
    }
    return (int)n;
}

int snprintf(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- math ---- */
double floor(double x) {
    if (x != x || x >= 4503599627370496.0 || x <= -4503599627370496.0) {
        return x;
    }
    if (x > -2147483000.0 && x < 2147483000.0) {
        double t = (double)(int)x; /* one instruction each way; 64 bits are a library call */
        return t > x ? t - 1.0 : t;
    }
    double t = (double)(long long)x;
    return t > x ? t - 1.0 : t;
}

double ceil(double x) { return -floor(-x); }

long long llround(double x) { return (long long)(x < 0 ? x - 0.5 : x + 0.5); }

/* sine by range reduction and a Taylor series; only polar #SCROLL values use it */
double sin(double x) {
    const double pi = 3.14159265358979323846;
    double k = floor(x / (2 * pi) + 0.5);
    x -= k * 2 * pi; /* -pi .. pi */
    if (x > pi / 2) {
        x = pi - x;
    } else if (x < -pi / 2) {
        x = -pi - x;
    }
    double x2 = x * x, term = x, sum = x;
    for (int i = 1; i < 10; i++) {
        term *= -x2 / ((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}

double cos(double x) { return sin(x + 1.57079632679489661923); }

/* ---- 64-bit division, for the compiler ---- */
unsigned long long __udivmoddi4(unsigned long long n, unsigned long long d, unsigned long long *rem) {
    if (!(n >> 32) && !(d >> 32) && (uint32_t)d) { /* the CPU divides 32-bit numbers itself */
        if (rem) {
            *rem = (uint32_t)n % (uint32_t)d;
        }
        return (uint32_t)n / (uint32_t)d;
    }
    unsigned long long q = 0, r = 0;
    if (d == 0) {
        if (rem) {
            *rem = 0;
        }
        return ~0ull;
    }
    if ((n >> 32) == 0 && (d >> 32) == 0) { /* the common case fits the hardware divider */
        if (rem) {
            *rem = (uint32_t)n % (uint32_t)d;
        }
        return (uint32_t)n / (uint32_t)d;
    }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= 1ull << i;
        }
    }
    if (rem) {
        *rem = r;
    }
    return q;
}

unsigned long long __udivdi3(unsigned long long n, unsigned long long d) { return __udivmoddi4(n, d, NULL); }

unsigned long long __umoddi3(unsigned long long n, unsigned long long d) {
    unsigned long long r;
    __udivmoddi4(n, d, &r);
    return r;
}

long long __divdi3(long long n, long long d) {
    int neg = (n < 0) != (d < 0);
    unsigned long long q = __udivmoddi4(n < 0 ? 0ull - (unsigned long long)n : (unsigned long long)n,
                                        d < 0 ? 0ull - (unsigned long long)d : (unsigned long long)d, NULL);
    return neg ? -(long long)q : (long long)q;
}

long long __moddi3(long long n, long long d) {
    unsigned long long r;
    __udivmoddi4(n < 0 ? 0ull - (unsigned long long)n : (unsigned long long)n,
                 d < 0 ? 0ull - (unsigned long long)d : (unsigned long long)d, &r);
    return n < 0 ? -(long long)r : (long long)r;
}

/* ---- 64-bit integer <-> double, built from the 32-bit conversions the FPU has ---- */
double __floatundidf(unsigned long long v) {
    return (double)(uint32_t)(v >> 32) * 4294967296.0 + (double)(uint32_t)v;
}

double __floatdidf(long long v) {
    return (double)(int32_t)(v >> 32) * 4294967296.0 + (double)(uint32_t)v;
}

unsigned long long __fixunsdfdi(double x) {
    if (!(x >= 1.0)) {
        return 0;
    }
    if (x >= 18446744073709551616.0) {
        return ~0ull;
    }
    uint32_t hi = (uint32_t)(x / 4294967296.0);
    uint32_t lo = (uint32_t)(x - (double)hi * 4294967296.0);
    return ((unsigned long long)hi << 32) | lo;
}

long long __fixdfdi(double x) {
    if (x < 0) {
        unsigned long long v = __fixunsdfdi(-x);
        return v > 0x8000000000000000ull ? (long long)0x8000000000000000ull : -(long long)v;
    }
    unsigned long long v = __fixunsdfdi(x);
    return v > 0x7FFFFFFFFFFFFFFFull ? 0x7FFFFFFFFFFFFFFFll : (long long)v;
}
