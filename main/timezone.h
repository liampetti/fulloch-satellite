#pragma once

#include <stddef.h>
#include <string.h>

// newlib understands POSIX TZ strings, not the IANA timezone database.
// Return NULL for unsupported IANA-style names; POSIX rules may contain '/'
// after their first comma (for example, M4.1.0/3).
static inline const char *satellite_posix_timezone(const char *configured)
{
    static const struct {
        const char *name;
        const char *posix;
    } zones[] = {
        {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
        {"Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
        {"Australia/Brisbane", "AEST-10"},
        {"Australia/Perth", "AWST-8"},
        {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
        {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
        {"America/Phoenix", "MST7"},
        {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
        {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
        {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
        {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Etc/UTC", "UTC0"},
    };

    for (size_t i = 0; i < sizeof(zones) / sizeof(zones[0]); ++i) {
        if (strcmp(configured, zones[i].name) == 0) {
            return zones[i].posix;
        }
    }
    const char *slash = strchr(configured, '/');
    const char *comma = strchr(configured, ',');
    return slash != NULL && (comma == NULL || slash < comma) ? NULL : configured;
}
