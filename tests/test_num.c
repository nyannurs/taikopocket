/* tp_stof/tp_stod/tp_stoi against the C library (what std::stof etc. call). */
#include <stdlib.h>

#include "core/num.h"
#include "test.h"

static tp_str S(const char *s) { return tp_str_make(s, strlen(s)); }

static void same_as_libc(const char *text) {
    char *end;
    float ref = strtof(text, &end);
    double got = 123;
    bool ok = tp_stof(S(text), &got);
    if (end == text) {
        CHECK(!ok);
    } else {
        CHECK(ok);
        if ((double)ref != got && !(ref != ref && got != got)) {
            test_failures++;
            printf("  FAIL stof(\"%s\") = %.9g, libc %.9g\n", text, got, (double)ref);
        }
        test_checks++;
    }
}

int main(void) {
    static const char *cases[] = {"120", "1.5", " 2", "-0.053", "+.5", "5.", ".5e1", "1e", "1e+", "1e3x",
                                  "174.99", "0.668", "-1.59", "abc", "", " ", "-", ".", "1,5", "12abc",
                                  "0.25", "3.14159265358979", "1e-3", "100000000", "0.000001", "7.240",
                                  "48.850", "2112410403927243233368", "0.1", "0.2", "0.3", "0.7", "299.99",
                                  "inf", "-INF", "1.0000001", "16777217", "33.333333333", "1e10",
                                  "\t\n 4/4", "000123.4500"};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        same_as_libc(cases[i]);
    }
    /* a sweep of the values charts are made of: d.ddd with up to 6 decimals */
    unsigned seed = 1;
    char buf[64];
    for (int i = 0; i < 200000; i++) {
        seed = seed * 1103515245u + 12345u;
        unsigned whole = (seed >> 8) % 2000;
        seed = seed * 1103515245u + 12345u;
        unsigned frac = (seed >> 8) % 1000000;
        int decimals = (int)((seed >> 28) % 7);
        if (decimals) {
            snprintf(buf, sizeof buf, "%s%u.%0*u", (seed & 1) ? "-" : "", whole, decimals,
                     frac % (unsigned)pow(10, decimals));
        } else {
            snprintf(buf, sizeof buf, "%u", whole);
        }
        same_as_libc(buf);
    }

    double d;
    CHECK(tp_stod(S(" 0.5 "), &d) && d == 0.5);
    CHECK(tp_stod(S("3/5"), &d) && d == 3.0);
    CHECK(!tp_stod(S("x"), &d));
    int v;
    bool range;
    CHECK(tp_stoi(S(" 42abc"), &v, &range) && v == 42 && !range);
    CHECK(tp_stoi(S("-7"), &v, &range) && v == -7);
    CHECK(tp_stoi(S("99999999999"), &v, &range) && range && v == 2147483647);
    CHECK(!tp_stoi(S(""), &v, &range));
    CHECK(!tp_stoi(S(" "), &v, &range));
    CHECK(!tp_stoi(S("a1"), &v, &range));

    CHECK(tp_equals(tp_trim(S(" \t a b\r\n")), "a b"));
    CHECK(tp_starts_with(S("#START P1"), "#START"));
    CHECK(!tp_starts_with(S("#ST"), "#START"));
    TEST_MAIN_END;
}
