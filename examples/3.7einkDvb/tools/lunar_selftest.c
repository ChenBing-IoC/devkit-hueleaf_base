/* Host-side check for main/content/lunar.c. Not part of the firmware build.
 *
 * Build and run from examples/3.7einkDvb:
 *   gcc -std=gnu11 -Wall -Wextra -Werror -I main/content \
 *       tools/lunar_selftest.c main/content/lunar.c -lm -o /tmp/lunar_selftest
 *   /tmp/lunar_selftest
 *
 * Every case below is an independently known calendar fact (Chinese New Year,
 * a leap-month first day, a cross-year boundary or a published solar term
 * date). The compressed table itself was generated offline from true new
 * moons and solar longitude; these cases guard the table and the bit layout. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lunar.h"

static int failures;
static int checks;

static void expect_lunar(int gy, int gm, int gd,
                         int ly, int lm, int ld, int leap)
{
    lunar_date_t date = { 0 };
    ++checks;
    const bool ok = lunar_from_gregorian(gy, gm, gd, &date);
    if (!ok || date.year != ly || date.month != lm || date.day != ld ||
        date.leap != (leap != 0)) {
        printf("FAIL lunar %04d-%02d-%02d: ok=%d got %d-%d-%d leap=%d "
               "want %d-%d-%d leap=%d\n",
               gy, gm, gd, (int)ok, date.year, date.month, date.day, (int)date.leap,
               ly, lm, ld, leap);
        ++failures;
    }
}

static void expect_lunar_invalid(int gy, int gm, int gd)
{
    lunar_date_t date = { 0 };
    ++checks;
    if (lunar_from_gregorian(gy, gm, gd, &date)) {
        printf("FAIL lunar %04d-%02d-%02d: expected out of range\n", gy, gm, gd);
        ++failures;
    }
}

static void expect_term(int gy, int gm, int gd, const char *want)
{
    char name[24] = { 0 };
    ++checks;
    const bool ok = lunar_solar_term_name(gy, gm, gd, name, sizeof(name));
    if (!ok || strcmp(name, want) != 0) {
        printf("FAIL term %04d-%02d-%02d: ok=%d got \"%s\" want \"%s\"\n",
               gy, gm, gd, (int)ok, name, want);
        ++failures;
    }
}

static void expect_parse(const char *text, bool want)
{
    int y = 0, m = 0, d = 0;
    ++checks;
    const bool got = lunar_parse_date(text, &y, &m, &d);
    if (got != want) {
        printf("FAIL parse \"%s\": got %d want %d\n", text, (int)got, (int)want);
        ++failures;
    }
}

int main(void)
{
    /* Chinese New Year: lunar 1-1 across the whole table range. */
    expect_lunar(1900, 1, 31, 1900, 1, 1, 0);
    expect_lunar(1950, 2, 17, 1950, 1, 1, 0);
    expect_lunar(1970, 2, 6, 1970, 1, 1, 0);
    expect_lunar(1984, 2, 2, 1984, 1, 1, 0);
    expect_lunar(2000, 2, 5, 2000, 1, 1, 0);
    expect_lunar(2010, 2, 14, 2010, 1, 1, 0);
    expect_lunar(2020, 1, 25, 2020, 1, 1, 0);
    expect_lunar(2023, 1, 22, 2023, 1, 1, 0);
    expect_lunar(2024, 2, 10, 2024, 1, 1, 0);
    expect_lunar(2025, 1, 29, 2025, 1, 1, 0);
    expect_lunar(2026, 2, 17, 2026, 1, 1, 0);
    expect_lunar(2028, 1, 26, 2028, 1, 1, 0);
    expect_lunar(2033, 1, 31, 2033, 1, 1, 0);

    /* First day of each leap month, including the 2033 leap-11 case. */
    expect_lunar(1900, 9, 24, 1900, 8, 1, 1);
    expect_lunar(2020, 5, 23, 2020, 4, 1, 1);
    expect_lunar(2023, 3, 22, 2023, 2, 1, 1);
    expect_lunar(2025, 7, 25, 2025, 6, 1, 1);
    expect_lunar(2033, 12, 22, 2033, 11, 1, 1);

    /* Cross-year boundaries. */
    expect_lunar(2025, 12, 31, 2025, 11, 12, 0);
    expect_lunar(2026, 1, 1, 2025, 11, 13, 0);
    expect_lunar(2026, 11, 27, 2026, 10, 19, 0);
    expect_lunar(2099, 12, 31, 2099, 11, 20, 0);
    expect_lunar(2100, 1, 1, 2099, 11, 21, 0);

    /* Edges and out-of-range dates must not crash. */
    expect_lunar_invalid(1899, 12, 31);
    expect_lunar_invalid(1900, 1, 30);
    expect_lunar_invalid(2101, 1, 1);
    expect_lunar_invalid(2023, 2, 29);

    /* Solar term day boundaries (2026 dates cross-checked against an
     * independent ephemeris calculation). */
    expect_term(2026, 2, 3, "MAJOR COLD");
    expect_term(2026, 2, 4, "START OF SPRING");
    expect_term(2026, 3, 19, "AWAKENING OF INSECTS");
    expect_term(2026, 3, 20, "SPRING EQUINOX");
    expect_term(2026, 6, 20, "GRAIN IN EAR");
    expect_term(2026, 6, 21, "SUMMER SOLSTICE");
    expect_term(2026, 11, 21, "START OF WINTER");
    expect_term(2026, 11, 22, "MINOR SNOW");
    expect_term(2026, 12, 21, "MAJOR SNOW");
    expect_term(2026, 12, 22, "WINTER SOLSTICE");
    expect_term(2000, 2, 4, "START OF SPRING");
    expect_term(2100, 12, 22, "WINTER SOLSTICE");

    /* Date parser used by the /api/content validator. */
    expect_parse("2026-12-31", true);
    expect_parse("1900-01-01", true);
    expect_parse("2100-12-31", true);
    expect_parse("2024-02-29", true);
    expect_parse("2023-02-29", false);
    expect_parse("2026-13-01", false);
    expect_parse("2026-1-1", false);
    expect_parse("2026/12/31", false);

    if (failures == 0) {
        printf("lunar selftest: %d checks passed\n", checks);
        return 0;
    }
    printf("lunar selftest: %d failure(s)\n", failures);
    return 1;
}
