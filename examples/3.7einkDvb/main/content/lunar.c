#include "lunar.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Chinese calendar 1900-2100.
 *
 * The year table is the classic 32-bit compressed form: bits 0-3 hold the leap
 * month (0 = none), bit 16 marks a 30-day leap month, and bits 15..4 mark a
 * 30-day regular month for months 1..12. 1900-01-31 is lunar 1900-01-01.
 *
 * The table itself was generated offline from true new moons and apparent
 * solar longitude (Meeus, Astronomical Algorithms ch. 25 and 49) with the
 * Chinese calendar rules: months start on the local (UTC+8) day of the
 * conjunction, the month holding the winter solstice is month 11, and in a
 * 13-month sui the first month without a major solar term is the leap month.
 * tools/lunar_selftest.c checks it against known festival dates. */
static const uint32_t k_lunar_info[201] = {
    0x04bd8U, 0x04ae0U, 0x0a570U, 0x054d5U, 0x0d260U,
    0x0d950U, 0x16554U, 0x056a0U, 0x09ad0U, 0x055d2U,
    0x04ae0U, 0x0a5b6U, 0x0a4d0U, 0x0d250U, 0x1d295U,
    0x0b550U, 0x056a0U, 0x0ada2U, 0x095b0U, 0x14977U,
    0x049b0U, 0x0a4b0U, 0x0b4b5U, 0x06a50U, 0x0ad40U,
    0x0bb53U, 0x02b60U, 0x09570U, 0x05372U, 0x04970U,
    0x06566U, 0x0d4a0U, 0x0ea50U, 0x16a95U, 0x05ad0U,
    0x02b60U, 0x186e3U, 0x092e0U, 0x1c8d7U, 0x0c950U,
    0x0d4a0U, 0x1d8a6U, 0x0b550U, 0x056a0U, 0x1a5b4U,
    0x025d0U, 0x092d0U, 0x0d2b2U, 0x0a950U, 0x0b557U,
    0x06ca0U, 0x0b550U, 0x15355U, 0x04da0U, 0x0a5b0U,
    0x14573U, 0x052b0U, 0x0a9a8U, 0x0e950U, 0x06aa0U,
    0x0aea6U, 0x0ab50U, 0x04b60U, 0x0aae4U, 0x0a570U,
    0x05260U, 0x0f263U, 0x0d950U, 0x05b57U, 0x056a0U,
    0x096d0U, 0x04dd5U, 0x04ad0U, 0x0a4d0U, 0x0d4d4U,
    0x0d250U, 0x0d558U, 0x0b540U, 0x0b6a0U, 0x195a6U,
    0x095b0U, 0x049b0U, 0x0a974U, 0x0a4b0U, 0x0b27aU,
    0x06a50U, 0x06d40U, 0x0af46U, 0x0ab60U, 0x09570U,
    0x04af5U, 0x04970U, 0x064b0U, 0x074a3U, 0x0ea50U,
    0x06b58U, 0x05ac0U, 0x0ab60U, 0x096d5U, 0x092e0U,
    0x0c960U, 0x0d954U, 0x0d4a0U, 0x0da50U, 0x07552U,
    0x056a0U, 0x0abb7U, 0x025d0U, 0x092d0U, 0x0cab5U,
    0x0a950U, 0x0b4a0U, 0x1aaa3U, 0x0ad50U, 0x055d9U,
    0x04ba0U, 0x0a5b0U, 0x05575U, 0x052b0U, 0x0a930U,
    0x07954U, 0x06aa0U, 0x0ad50U, 0x05b52U, 0x04b60U,
    0x0a6e6U, 0x0a4e0U, 0x0d260U, 0x0ea65U, 0x0d530U,
    0x05aa0U, 0x076a3U, 0x096d0U, 0x04afbU, 0x04ad0U,
    0x0a4d0U, 0x1d0b6U, 0x0d250U, 0x0d520U, 0x0dd45U,
    0x0b5a0U, 0x056d0U, 0x055b2U, 0x049b0U, 0x0a577U,
    0x0a4b0U, 0x0aa50U, 0x1b255U, 0x06d20U, 0x0ada0U,
    0x14b63U, 0x09370U, 0x049f8U, 0x04970U, 0x064b0U,
    0x168a6U, 0x0ea50U, 0x06aa0U, 0x1a6c4U, 0x0aae0U,
    0x092e0U, 0x0d2e3U, 0x0c960U, 0x0d557U, 0x0d4a0U,
    0x0da50U, 0x05d55U, 0x056a0U, 0x0a6d0U, 0x055d4U,
    0x052d0U, 0x0a9b8U, 0x0a950U, 0x0b4a0U, 0x0b6a6U,
    0x0ad50U, 0x055a0U, 0x0aba4U, 0x0a5b0U, 0x052b0U,
    0x0b273U, 0x06930U, 0x07337U, 0x06aa0U, 0x0ad50U,
    0x14b55U, 0x04b60U, 0x0a570U, 0x054e4U, 0x0d160U,
    0x0e968U, 0x0d520U, 0x0daa0U, 0x16aa6U, 0x056d0U,
    0x04ae0U, 0x0a9d4U, 0x0a2d0U, 0x0d150U, 0x0f252U,
    0x0d520U,
};

#define LUNAR_YEARS 201
#define LUNAR_BASE_YEAR 1900

static int lunar_leap_month(int year)
{
    return (int)(k_lunar_info[year - LUNAR_BASE_YEAR] & 0x0Fu);
}

static int lunar_leap_days(int year)
{
    if (!lunar_leap_month(year)) return 0;
    return (k_lunar_info[year - LUNAR_BASE_YEAR] & 0x10000u) ? 30 : 29;
}

static int lunar_month_days(int year, int month)
{
    return (k_lunar_info[year - LUNAR_BASE_YEAR] & (0x10000u >> month)) ? 30 : 29;
}

static int lunar_year_days(int year)
{
    const uint32_t info = k_lunar_info[year - LUNAR_BASE_YEAR];
    int days = 348; /* 12 * 29 */
    for (int bit = 0; bit < 12; ++bit) {
        if (info & (0x8000u >> bit)) ++days;
    }
    days += lunar_leap_days(year);
    return days;
}

static bool is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int days_in_month(int year, int month)
{
    static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 0;
    return month == 2 && is_leap_year(year) ? 29 : days[month - 1];
}

/* Days since 1970-01-01 (Howard Hinnant's civil calendar algorithm). */
static int32_t days_from_civil(int year, int month, int day)
{
    int y = year - (month <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int32_t)era * 146097 + (int32_t)doe - 719468;
}

bool lunar_from_gregorian(int year, int month, int day, lunar_date_t *out)
{
    if (!out || year < 1900 || year > 2100 || month < 1 || month > 12 ||
        day < 1 || day > days_in_month(year, month)) {
        return false;
    }
    int32_t offset = days_from_civil(year, month, day) - days_from_civil(1900, 1, 31);
    if (offset < 0) {
        return false;
    }

    int lunar_year = LUNAR_BASE_YEAR;
    for (; lunar_year <= 2100; ++lunar_year) {
        const int days = lunar_year_days(lunar_year);
        if (offset < days) break;
        offset -= days;
    }
    if (lunar_year > 2100) {
        return false;
    }

    const int leap = lunar_leap_month(lunar_year);
    for (int month_no = 1; month_no <= 12; ++month_no) {
        const int days = lunar_month_days(lunar_year, month_no);
        if (offset < days) {
            out->year = lunar_year;
            out->month = month_no;
            out->day = (int)offset + 1;
            out->leap = false;
            return true;
        }
        offset -= days;
        if (month_no == leap) {
            const int leap_days = lunar_leap_days(lunar_year);
            if (offset < leap_days) {
                out->year = lunar_year;
                out->month = month_no;
                out->day = (int)offset + 1;
                out->leap = true;
                return true;
            }
            offset -= leap_days;
        }
    }
    return false;
}

/* ---- Solar terms --------------------------------------------------------- */

/* Apparent solar longitude, Meeus ch. 25 (accuracy about 0.01 degree). That is
 * enough to pick the calendar day of a term: a month boundary would have to
 * coincide with the term instant within minutes to matter. */
static double sind(double degrees)
{
    return sin(degrees * (3.14159265358979323846 / 180.0));
}

static double delta_t_seconds(int year)
{
    if (year < 1920) {
        const double t = (year - 1900) / 100.0;
        return -2.79 + 1.494119 * t - 0.0598939 * t * t +
               0.0061966 * t * t * t - 0.000197 * t * t * t * t;
    }
    if (year < 1941) {
        const double t = (year - 1920) / 100.0;
        return 21.20 + 0.84493 * t - 0.076100 * t * t + 0.0020936 * t * t * t;
    }
    if (year < 1961) {
        const double t = (year - 1950) / 100.0;
        return 29.07 + 0.407 * t - t * t / 233.0 + t * t * t / 2547.0;
    }
    if (year < 1986) {
        const double t = (year - 1975) / 100.0;
        return 45.45 + 1.067 * t - t * t / 260.0 - t * t * t / 718.0;
    }
    if (year < 2005) {
        const double t = (year - 2000) / 100.0;
        return 63.86 + 0.3345 * t - 0.060374 * t * t + 0.0017275 * t * t * t +
               0.000651814 * t * t * t * t + 0.00002373599 * t * t * t * t * t;
    }
    if (year < 2050) {
        const double t = (year - 2000) / 100.0;
        return 62.92 + 0.32217 * t + 0.005589 * t * t;
    }
    return -20.0 + 32.0 * ((year - 1820) / 100.0) * ((year - 1820) / 100.0) -
           0.5628 * (2150 - year);
}

/* Julian Day at a UT hour of a Gregorian date. Only 1900..2100 is used. */
static double julian_day_ut(int year, int month, int day, double hour)
{
    int y = year;
    int m = month;
    if (m <= 2) {
        y -= 1;
        m += 12;
    }
    const int a = y / 100;
    const int b = 2 - a + a / 4;
    return floor(365.25 * (double)(y + 4716)) +
           floor(30.6001 * (double)(m + 1)) + (double)day + (double)b - 1524.5 +
           hour / 24.0;
}

static double sun_apparent_longitude(double jd_tt)
{
    const double t = (jd_tt - 2451545.0) / 36525.0;
    const double l0 = 280.46646 + 36000.76983 * t + 0.0003032 * t * t;
    const double m = 357.52911 + 35999.05029 * t - 0.0001537 * t * t;
    const double c = (1.914602 - 0.004817 * t - 0.000014 * t * t) * sind(m) +
                     (0.019993 - 0.000101 * t) * sind(2.0 * m) +
                     0.000289 * sind(3.0 * m);
    const double omega = 125.04 - 1934.136 * t;
    double longitude = l0 + c - 0.00569 - 0.00478 * sind(omega);
    while (longitude < 0.0) longitude += 360.0;
    while (longitude >= 360.0) longitude -= 360.0;
    return longitude;
}

/* Index 0 is the term at 285 degrees (Minor Cold, early January); the list
 * runs forward 15 degrees at a time and ends at the winter solstice (270). */
static const char *const k_term_names[24] = {
    "MINOR COLD", "MAJOR COLD", "START OF SPRING", "RAIN WATER",
    "AWAKENING OF INSECTS", "SPRING EQUINOX", "PURE BRIGHTNESS", "GRAIN RAIN",
    "START OF SUMMER", "GRAIN FULL", "GRAIN IN EAR", "SUMMER SOLSTICE",
    "MINOR HEAT", "MAJOR HEAT", "START OF AUTUMN", "END OF HEAT",
    "WHITE DEW", "AUTUMN EQUINOX", "COLD DEW", "FROST DESCENT",
    "START OF WINTER", "MINOR SNOW", "MAJOR SNOW", "WINTER SOLSTICE",
};

static int term_index(double longitude)
{
    double relative = longitude - 285.0;
    while (relative < 0.0) relative += 360.0;
    while (relative >= 360.0) relative -= 360.0;
    int index = (int)(relative / 15.0);
    if (index < 0) index = 0;
    if (index > 23) index = 23;
    return index;
}

static void copy_text(char *dst, size_t size, const char *src)
{
    size_t length = strlen(src);
    if (length >= size) length = size - 1;
    memcpy(dst, src, length);
    dst[length] = '\0';
}

bool lunar_solar_term_name(int year, int month, int day, char *name, size_t size)
{
    if (!name || size < 2 || year < 1900 || year > 2100 ||
        month < 1 || month > 12 || day < 1 || day > days_in_month(year, month)) {
        return false;
    }
    /* China Standard Time is UTC+8 with no DST. Evaluating the end of the
     * local day makes a term that starts later today count for today. */
    const double jd_ut = julian_day_ut(year, month, day, 15.9999);
    const double jd_tt = jd_ut + delta_t_seconds(year) / 86400.0;
    copy_text(name, size, k_term_names[term_index(sun_apparent_longitude(jd_tt))]);
    return true;
}

/* ---- Date helpers -------------------------------------------------------- */

bool lunar_parse_date(const char *text, int *year, int *month, int *day)
{
    if (!text || strlen(text) != 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    for (int i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) continue;
        if (text[i] < '0' || text[i] > '9') return false;
    }
    const int y = (text[0] - '0') * 1000 + (text[1] - '0') * 100 +
                  (text[2] - '0') * 10 + (text[3] - '0');
    const int m = (text[5] - '0') * 10 + (text[6] - '0');
    const int d = (text[8] - '0') * 10 + (text[9] - '0');
    if (y < 1900 || y > 2100 || m < 1 || m > 12 || d < 1 || d > days_in_month(y, m)) {
        return false;
    }
    if (year) *year = y;
    if (month) *month = m;
    if (day) *day = d;
    return true;
}

int lunar_days_until(int year, int month, int day)
{
    const time_t now = time(NULL);
    struct tm local;
    if (now <= 0 || localtime_r(&now, &local) == NULL) {
        return 0;
    }
    const int32_t today = days_from_civil(local.tm_year + 1900, local.tm_mon + 1,
                                          local.tm_mday);
    return (int)(days_from_civil(year, month, day) - today);
}
