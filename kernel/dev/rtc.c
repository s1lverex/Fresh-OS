/* Fresh OS kernel - CMOS real time clock. */
#include "../kernel.h"

static uint8_t cmos(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static uint64_t boot_epoch;
static uint64_t boot_tick;

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static uint64_t read_rtc(void)
{
    uint8_t s, m, h, d, mo, y, c, s2, m2, h2, d2, mo2, y2;
    int guard = 0;
    do {
        while ((cmos(0x0A) & 0x80) && ++guard < 1000000)
            ;
        s = cmos(0);
        m = cmos(2);
        h = cmos(4);
        d = cmos(7);
        mo = cmos(8);
        y = cmos(9);
        while ((cmos(0x0A) & 0x80) && ++guard < 1000000)
            ;
        s2 = cmos(0);
        m2 = cmos(2);
        h2 = cmos(4);
        d2 = cmos(7);
        mo2 = cmos(8);
        y2 = cmos(9);
    } while ((s != s2 || m != m2 || h != h2 || d != d2 || mo != mo2 || y != y2) && ++guard < 2000000);
    uint8_t b = cmos(0x0B);
    c = 0;
    bool pm = h & 0x80;
    h &= 0x7F;
    if (!(b & 0x04)) {
#define BCD(x) (uint8_t)(((x) & 0x0F) + ((x) >> 4) * 10)
        s = BCD(s);
        m = BCD(m);
        h = BCD(h);
        d = BCD(d);
        mo = BCD(mo);
        y = BCD(y);
    }
    if (!(b & 0x02)) { /* 12 hour clock */
        if (pm && h != 12)
            h = (uint8_t)(h + 12);
        else if (!pm && h == 12)
            h = 0;
    }
    int year = 2000 + y;
    if (c)
        year = c * 100 + y;
    if (mo < 1 || mo > 12 || d < 1 || d > 31)
        return 0;
    int64_t days = days_from_civil(year, mo, d);
    return (uint64_t)(days * 86400 + h * 3600 + m * 60 + s);
}

void rtc_init(void)
{
    boot_epoch = read_rtc();
    boot_tick = g_ticks;
}

uint64_t rtc_unix_time(void)
{
    return boot_epoch + (g_ticks - boot_tick) / TIMER_HZ;
}
