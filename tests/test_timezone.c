#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "timezone.h"

static void check_hour(const char *zone, time_t timestamp, int hour)
{
    const char *tz = satellite_posix_timezone(zone);
    assert(tz != NULL);
    assert(setenv("TZ", tz, 1) == 0);
    tzset();
    struct tm local;
    assert(localtime_r(&timestamp, &local) != NULL);
    assert(local.tm_hour == hour);
}

int main(void)
{
    // 2024-01-01 00:00 UTC and 2024-07-01 12:00 UTC.
    check_hour("America/Los_Angeles", 1704067200, 16);
    check_hour("America/Los_Angeles", 1719835200, 5);
    check_hour("Australia/Sydney", 1704067200, 11);
    check_hour("Australia/Sydney", 1719835200, 22);
    check_hour("Europe/London", 1704067200, 0);
    check_hour("Europe/London", 1719835200, 13);
    check_hour("America/Phoenix", 1719835200, 5);
    check_hour("Etc/UTC", 1719835200, 12);
    const char *posix = "AEST-10AEDT,M10.1.0,M4.1.0/3";
    assert(strcmp(satellite_posix_timezone(posix), posix) == 0);
    // Representative TZif tails returned by the dashboard USB setup.
    const char *usb_zones[] = {
        "PST8PDT,M3.2.0,M11.1.0",
        "NZST-12NZDT,M9.5.0,M4.1.0/3",
        "IST-5:30",
        "<-02>2<-01>,M3.5.0/-1,M10.5.0/0",
    };
    for (size_t i = 0; i < sizeof(usb_zones) / sizeof(usb_zones[0]); ++i) {
        assert(strcmp(satellite_posix_timezone(usb_zones[i]), usb_zones[i]) == 0);
    }
    assert(satellite_posix_timezone("America/Toronto") == NULL);
    return 0;
}
