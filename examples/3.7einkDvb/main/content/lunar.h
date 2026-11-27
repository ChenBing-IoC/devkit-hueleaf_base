#ifndef CONTENT_LUNAR_H
#define CONTENT_LUNAR_H

#include <stdbool.h>
#include <stddef.h>

/* Chinese lunar date. Produced from the compressed 1900-2100 year table; the
 * table cannot express dates before 1900-01-31 (lunar 1900-01-01). */
typedef struct {
    int year;   /* lunar year, 1900..2100 */
    int month;  /* 1..12 */
    int day;    /* 1..30 */
    bool leap;  /* true for the leap copy of `month` */
} lunar_date_t;

/* Convert a Gregorian calendar date. Returns false outside 1900..2100 and for
 * the first 30 days of 1900, which precede the table base date. `out` is left
 * untouched on failure. */
bool lunar_from_gregorian(int year, int month, int day, lunar_date_t *out);

/* English name of the solar term in effect on this local (UTC+8) calendar
 * day, e.g. "MINOR SNOW". A term that starts anywhere during the day is used
 * for the whole day, matching printed almanacs. Returns false outside
 * 1900..2100 or when the name buffer is too small. */
bool lunar_solar_term_name(int year, int month, int day, char *name, size_t size);

/* Strict YYYY-MM-DD parser for 1900-01-01..2100-12-31. */
bool lunar_parse_date(const char *text, int *year, int *month, int *day);

/* Signed day count from today's local date to the given date. The caller must
 * pass a date accepted by lunar_parse_date. */
int lunar_days_until(int year, int month, int day);

#endif /* CONTENT_LUNAR_H */
