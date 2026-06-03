/* Nordstjernen — civil-date math and HTML date/time string parsing. */

#include "datetime.h"

long
nd_dt_floormod(long a, long b)
{
    long r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

long
nd_dt_days_from_civil(int y, int m, int d)
{
    int yy = y - (m <= 2);
    long era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}

void
nd_dt_civil_from_days(long z, int *y, int *m, int *d)
{
    z += 719468L;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yy = (int)yoe + (int)(era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp + (mp < 10 ? 3 : -9);
    *y = yy + (mm <= 2);
    *m = (int)mm;
    *d = (int)dd;
}

int
nd_dt_days_in_month(int y, int m)
{
    static const int dim[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m < 1 || m > 12) return 0;
    if (m == 2) {
        gboolean leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        return leap ? 29 : 28;
    }
    return dim[m - 1];
}

int
nd_dt_iso_weeks_in_year(int y)
{
    int p  = (y + y / 4 - y / 100 + y / 400) % 7;
    int py = y - 1;
    int p1 = (py + py / 4 - py / 100 + py / 400) % 7;
    return (p == 4 || p1 == 3) ? 53 : 52;
}

long
nd_dt_iso_week1_monday(int y)
{
    long jan4 = nd_dt_days_from_civil(y, 1, 4);
    int m0 = (int)nd_dt_floormod(jan4 + 3, 7);
    return jan4 - m0;
}

const char *
nd_dt_rd_digits(const char *p, int min, int max, int *out)
{
    int val = 0, cnt = 0;
    while (*p >= '0' && *p <= '9' && cnt < max) {
        val = val * 10 + (*p - '0');
        p++;
        cnt++;
    }
    if (cnt < min) return NULL;
    *out = val;
    return p;
}

const char *
nd_dt_rd_date(const char *p, int *y, int *m, int *d)
{
    p = nd_dt_rd_digits(p, 4, 9, y); if (!p || *p != '-') return NULL; p++;
    p = nd_dt_rd_digits(p, 2, 2, m); if (!p || *p != '-') return NULL; p++;
    p = nd_dt_rd_digits(p, 2, 2, d); if (!p) return NULL;
    if (*y < 1 || *m < 1 || *m > 12 || *d < 1 || *d > nd_dt_days_in_month(*y, *m))
        return NULL;
    return p;
}

const char *
nd_dt_rd_time(const char *p, int *ms)
{
    int h, mi, se = 0, frac = 0;
    p = nd_dt_rd_digits(p, 2, 2, &h);  if (!p || *p != ':') return NULL; p++;
    p = nd_dt_rd_digits(p, 2, 2, &mi); if (!p) return NULL;
    if (*p == ':') {
        p++;
        p = nd_dt_rd_digits(p, 2, 2, &se); if (!p) return NULL;
        if (*p == '.') {
            p++;
            int v = 0, c = 0;
            while (*p >= '0' && *p <= '9' && c < 3) {
                v = v * 10 + (*p - '0');
                p++;
                c++;
            }
            if (c == 0) return NULL;
            while (c < 3) {
                v *= 10;
                c++;
            }
            while (*p >= '0' && *p <= '9') p++;
            frac = v;
        }
    }
    if (h > 23 || mi > 59 || se > 59) return NULL;
    *ms = ((h * 60 + mi) * 60 + se) * 1000 + frac;
    return p;
}
