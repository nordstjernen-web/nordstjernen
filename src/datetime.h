/* Nordstjernen — civil-date math and HTML date/time string parsing. */
#ifndef ND_DATETIME_H
#define ND_DATETIME_H

#include <glib.h>

long nd_dt_floormod(long a, long b);
long nd_dt_days_from_civil(int y, int m, int d);
void nd_dt_civil_from_days(long z, int *y, int *m, int *d);
int  nd_dt_days_in_month(int y, int m);
int  nd_dt_iso_weeks_in_year(int y);
long nd_dt_iso_week1_monday(int y);
const char *nd_dt_rd_digits(const char *p, int min, int max, int *out);
const char *nd_dt_rd_date(const char *p, int *y, int *m, int *d);
const char *nd_dt_rd_time(const char *p, int *ms);

#endif
