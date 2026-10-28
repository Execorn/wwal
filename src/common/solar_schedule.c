// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/solar_schedule.h"

#include "waywal/path.h"
#include "waywal/we_json.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline double deg_to_rad(double deg)
{
    return deg * (M_PI / 180.0);
}

static inline double rad_to_deg(double rad)
{
    return rad * (180.0 / M_PI);
}

static inline double clamp_d(double v, double lo, double hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static inline double wrap_360(double deg)
{
    double r = fmod(deg, 360.0);
    if (r < 0.0)
        r += 360.0;
    return r;
}

static inline double wrap_1440(double mins)
{
    double r = fmod(mins, 1440.0);
    if (r < 0.0)
        r += 1440.0;
    return r;
}

void waywal_solar_compute(double latitude_deg, double longitude_deg, time_t utc_now,
                          const struct tm *local_tm, waywal_solar_state_t *out_state)
{
    if (!out_state)
        return;
    memset(out_state, 0, sizeof(*out_state));

    if (!isfinite(latitude_deg))
        latitude_deg = 0.0;
    if (!isfinite(longitude_deg))
        longitude_deg = 0.0;

    double lat_clamped = clamp_d(latitude_deg, -89.8, 89.8);
    double lon_clamped = clamp_d(longitude_deg, -180.0, 180.0);

    double jd = ((double)utc_now / 86400.0) + 2440587.5;
    double t = (jd - 2451545.0) / 36525.0;

    double l0_deg = wrap_360(280.46646 + t * (36000.76983 + 0.0003032 * t));
    double m_deg = 357.52911 + t * (35999.05029 - 0.0001537 * t);
    double ecc = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);

    double m_rad = deg_to_rad(m_deg);
    double c_deg = sin(m_rad) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
                   sin(2.0 * m_rad) * (0.019993 - 0.000101 * t) + sin(3.0 * m_rad) * 0.000289;

    double sun_true_lon_deg = l0_deg + c_deg;
    double omega_deg = 125.04 - 1934.136 * t;
    double lambda_deg = sun_true_lon_deg - 0.00569 - 0.00478 * sin(deg_to_rad(omega_deg));

    double eps0_deg =
        23.0 + (26.0 + ((21.448 - t * (46.8150 + t * (0.00059 - 0.001813 * t))) / 60.0)) / 60.0;
    double eps_deg = eps0_deg + 0.00256 * cos(deg_to_rad(omega_deg));

    double eps_rad = deg_to_rad(eps_deg);
    double lambda_rad = deg_to_rad(lambda_deg);
    double sin_delta = clamp_d(sin(eps_rad) * sin(lambda_rad), -1.0, 1.0);
    double delta_rad = asin(sin_delta);
    double delta_deg = rad_to_deg(delta_rad);

    double y = tan(eps_rad / 2.0);
    y *= y;
    double l0_rad = deg_to_rad(l0_deg);
    double eq_time_min =
        4.0 * rad_to_deg(y * sin(2.0 * l0_rad) - 2.0 * ecc * sin(m_rad) +
                         4.0 * ecc * y * sin(m_rad) * cos(2.0 * l0_rad) -
                         0.5 * y * y * sin(4.0 * l0_rad) - 1.25 * ecc * ecc * sin(2.0 * m_rad));

    int64_t day_sec = (int64_t)utc_now % 86400;
    if (day_sec < 0)
        day_sec += 86400;
    time_t utc_day_start = utc_now - (time_t)day_sec;
    double utc_min = (double)day_sec / 60.0;

    double tz_off_min = 0.0;
    if (local_tm) {
        tz_off_min = (double)local_tm->tm_gmtoff / 60.0;
    }

    double noon_utc_min = 720.0 - 4.0 * lon_clamped - eq_time_min;
    if (utc_min - noon_utc_min < -720.0) {
        noon_utc_min -= 1440.0;
    } else if (utc_min - noon_utc_min > 720.0) {
        noon_utc_min += 1440.0;
    }
    double solar_noon_min = wrap_1440(noon_utc_min + tz_off_min);
    double tst = wrap_1440(utc_min + eq_time_min + 4.0 * lon_clamped);
    double hour_angle_deg = (tst / 4.0) - 180.0;

    double phi_rad = deg_to_rad(lat_clamped);
    double h_rad = deg_to_rad(hour_angle_deg);
    double cos_zenith =
        clamp_d(sin(phi_rad) * sin(delta_rad) + cos(phi_rad) * cos(delta_rad) * cos(h_rad), -1.0,
                1.0);
    double zenith_rad = acos(cos_zenith);
    double zenith_deg = rad_to_deg(zenith_rad);
    double elevation_deg = 90.0 - zenith_deg;

    double azimuth_deg = 180.0;
    double denom_az = cos(phi_rad) * sin(zenith_rad);
    if (fabs(denom_az) > 1e-9) {
        double cos_az = clamp_d((sin(phi_rad) * cos_zenith - sin(delta_rad)) / denom_az, -1.0, 1.0);
        double az_base = rad_to_deg(acos(cos_az));
        if (hour_angle_deg > 0.0) {
            azimuth_deg = wrap_360(az_base + 180.0);
        } else {
            azimuth_deg = wrap_360(540.0 - az_base);
        }
    }

    double denom_ha = cos(phi_rad) * cos(delta_rad);
    if (fabs(denom_ha) < 1e-9)
        denom_ha = (denom_ha >= 0.0) ? 1e-9 : -1e-9;

    double cos_omega_sr = (sin(deg_to_rad(-0.833)) - sin(phi_rad) * sin(delta_rad)) / denom_ha;
    double cos_omega_civ = (sin(deg_to_rad(-6.0)) - sin(phi_rad) * sin(delta_rad)) / denom_ha;

    out_state->julian_day = jd;
    out_state->julian_century = t;
    out_state->geom_mean_lon_deg = l0_deg;
    out_state->geom_mean_anom_deg = m_deg;
    out_state->eq_of_center_deg = c_deg;
    out_state->apparent_lon_deg = lambda_deg;
    out_state->obliquity_deg = eps_deg;
    out_state->declination_deg = delta_deg;
    out_state->eq_of_time_min = eq_time_min;
    out_state->true_solar_time_min = tst;
    out_state->hour_angle_deg = hour_angle_deg;
    out_state->elevation_deg = elevation_deg;
    out_state->azimuth_deg = azimuth_deg;
    out_state->solar_noon_min = solar_noon_min;
    out_state->solar_noon_utc = utc_day_start + (time_t)llround(noon_utc_min * 60.0);

    if (cos_omega_sr < -1.0) {
        out_state->has_sunrise_sunset = false;
        out_state->is_polar_day = true;
        out_state->is_polar_night = false;
        out_state->is_daylight = true;
        out_state->sunrise_min = 0.0;
        out_state->sunset_min = 1440.0;
        out_state->civil_dawn_min = 0.0;
        out_state->civil_dusk_min = 1440.0;
        out_state->sunrise_utc = utc_day_start;
        out_state->sunset_utc = utc_day_start + 86400;
        out_state->dawn_utc = utc_day_start;
        out_state->dusk_utc = utc_day_start + 86400;
    } else if (cos_omega_sr > 1.0) {
        out_state->has_sunrise_sunset = false;
        out_state->is_polar_day = false;
        out_state->is_polar_night = true;
        out_state->is_daylight = false;
        out_state->sunrise_min = -1.0;
        out_state->sunset_min = -1.0;
        out_state->civil_dawn_min = -1.0;
        out_state->civil_dusk_min = -1.0;
        out_state->sunrise_utc = 0;
        out_state->sunset_utc = 0;
        out_state->dawn_utc = 0;
        out_state->dusk_utc = 0;
    } else {
        out_state->has_sunrise_sunset = true;
        out_state->is_polar_day = false;
        out_state->is_polar_night = false;

        double omega_sr_deg = rad_to_deg(acos(clamp_d(cos_omega_sr, -1.0, 1.0)));
        double sr_utc_min = noon_utc_min - 4.0 * omega_sr_deg;
        double ss_utc_min = noon_utc_min + 4.0 * omega_sr_deg;

        out_state->sunrise_utc = utc_day_start + (time_t)llround(sr_utc_min * 60.0);
        out_state->sunset_utc = utc_day_start + (time_t)llround(ss_utc_min * 60.0);
        out_state->sunrise_min = wrap_1440(sr_utc_min + tz_off_min);
        out_state->sunset_min = wrap_1440(ss_utc_min + tz_off_min);

        double omega_civ_deg = rad_to_deg(acos(clamp_d(cos_omega_civ, -1.0, 1.0)));
        double dawn_utc_min = noon_utc_min - 4.0 * omega_civ_deg;
        double dusk_utc_min = noon_utc_min + 4.0 * omega_civ_deg;
        out_state->dawn_utc = utc_day_start + (time_t)llround(dawn_utc_min * 60.0);
        out_state->dusk_utc = utc_day_start + (time_t)llround(dusk_utc_min * 60.0);
        out_state->civil_dawn_min = wrap_1440(dawn_utc_min + tz_off_min);
        out_state->civil_dusk_min = wrap_1440(dusk_utc_min + tz_off_min);

        out_state->is_daylight = (elevation_deg >= -0.833);
    }

    if (elevation_deg < -6.0) {
        out_state->phase = WAYWAL_SOLAR_NIGHT;
    } else if (elevation_deg < 6.0) {
        out_state->phase = (hour_angle_deg < 0.0) ? WAYWAL_SOLAR_DAWN : WAYWAL_SOLAR_DUSK;
    } else {
        out_state->phase = WAYWAL_SOLAR_DAY;
    }
}

waywal_solar_state_t waywal_solar_calculate(double lat_deg, double lon_deg, time_t utc_now)
{
    waywal_solar_state_t st;
    waywal_solar_compute(lat_deg, lon_deg, utc_now, NULL, &st);
    return st;
}

bool waywal_solar_is_daytime_at_ex(double latitude_deg, double longitude_deg, bool has_coordinates,
                                   int32_t sunrise_fallback_min, int32_t sunset_fallback_min,
                                   time_t utc_now, const struct tm *local_tm,
                                   waywal_solar_phase_t *out_phase)
{
    if (has_coordinates) {
        waywal_solar_state_t st;
        waywal_solar_compute(latitude_deg, longitude_deg, utc_now, local_tm, &st);
        if (out_phase)
            *out_phase = st.phase;
        return st.is_daylight;
    }

    struct tm fallback_tm;
    if (!local_tm) {
        localtime_r(&utc_now, &fallback_tm);
        local_tm = &fallback_tm;
    }

    int sr_min =
        (sunrise_fallback_min > 0 && sunrise_fallback_min < 1440) ? sunrise_fallback_min : 390;
    int ss_min =
        (sunset_fallback_min > 0 && sunset_fallback_min < 1440) ? sunset_fallback_min : 1110;
    if (ss_min <= sr_min) {
        sr_min = 390;
        ss_min = 1110;
    }

    int mins = local_tm->tm_hour * 60 + local_tm->tm_min;
    int dawn_start = (sr_min >= 60) ? (sr_min - 60) : 0;
    int dawn_end = (sr_min + 30 < ss_min) ? (sr_min + 30) : sr_min;
    int dusk_start = (ss_min - 60 > dawn_end) ? (ss_min - 60) : ss_min;
    int dusk_end = (ss_min + 90 <= 1440) ? (ss_min + 90) : 1440;

    waywal_solar_phase_t phase = WAYWAL_SOLAR_NIGHT;
    bool is_day = false;

    if (mins >= dawn_start && mins < dawn_end) {
        phase = WAYWAL_SOLAR_DAWN;
        is_day = (mins >= sr_min);
    } else if (mins >= dawn_end && mins < dusk_start) {
        phase = WAYWAL_SOLAR_DAY;
        is_day = true;
    } else if (mins >= dusk_start && mins < dusk_end) {
        phase = WAYWAL_SOLAR_DUSK;
        is_day = (mins < ss_min);
    } else {
        phase = WAYWAL_SOLAR_NIGHT;
        is_day = false;
    }

    if (out_phase)
        *out_phase = phase;
    return is_day;
}

bool waywal_solar_is_daytime_at(double latitude_deg, double longitude_deg, bool has_coordinates,
                                time_t utc_now, const struct tm *local_tm,
                                waywal_solar_phase_t *out_phase)
{
    return waywal_solar_is_daytime_at_ex(latitude_deg, longitude_deg, has_coordinates, 390, 1110,
                                         utc_now, local_tm, out_phase);
}

void waywal_solar_synthesize_local(int32_t sunrise_fallback_min, int32_t sunset_fallback_min,
                                   time_t utc_now, const struct tm *local_tm,
                                   waywal_solar_state_t *out_state)
{
    if (!out_state)
        return;
    memset(out_state, 0, sizeof(*out_state));

    struct tm fallback_tm;
    if (!local_tm) {
        localtime_r(&utc_now, &fallback_tm);
        local_tm = &fallback_tm;
    }

    int sr_min =
        (sunrise_fallback_min > 0 && sunrise_fallback_min < 1440) ? sunrise_fallback_min : 390;
    int ss_min =
        (sunset_fallback_min > 0 && sunset_fallback_min < 1440) ? sunset_fallback_min : 1110;
    if (ss_min <= sr_min) {
        sr_min = 390;
        ss_min = 1110;
    }

    double tod_min = (double)local_tm->tm_hour * 60.0 + (double)local_tm->tm_min +
                     (double)local_tm->tm_sec / 60.0;
    out_state->true_solar_time_min = tod_min;
    out_state->hour_angle_deg = (tod_min / 4.0) - 180.0;
    out_state->solar_noon_min = 720.0;
    out_state->sunrise_min = (double)sr_min;
    out_state->sunset_min = (double)ss_min;
    out_state->civil_dawn_min = (double)((sr_min >= 60) ? (sr_min - 60) : 0);
    out_state->civil_dusk_min = (double)((ss_min + 90 <= 1440) ? (ss_min + 90) : 1440);
    out_state->has_sunrise_sunset = true;
    out_state->elevation_deg = -90.0 * cos(deg_to_rad(out_state->hour_angle_deg));
    out_state->azimuth_deg = (out_state->hour_angle_deg < 0.0) ? 90.0 : 270.0;
    waywal_solar_phase_t fb_phase = WAYWAL_SOLAR_NIGHT;
    out_state->is_daylight = waywal_solar_is_daytime_at_ex(
        0.0, 0.0, false, sr_min, ss_min, utc_now, local_tm, &fb_phase);
    out_state->phase = fb_phase;
}

const char *waywal_solar_phase_to_string(waywal_solar_phase_t phase)
{
    switch ((int)phase) {
    case WAYWAL_SOLAR_NIGHT:
        return "night";
    case WAYWAL_SOLAR_DAWN:
        return "dawn";
    case WAYWAL_SOLAR_DAY:
        return "day";
    case WAYWAL_SOLAR_DUSK:
        return "dusk";
    case WAYWAL_SOLAR_MORNING:
        return "morning";
    case WAYWAL_SOLAR_NOON:
        return "noon";
    case WAYWAL_SOLAR_AFTERNOON:
        return "afternoon";
    case WAYWAL_SOLAR_SUNSET:
        return "sunset";
    case WAYWAL_SOLAR_EVENING:
        return "evening";
    default:
        return "night";
    }
}

bool waywal_solar_phase_from_string(const char *str, waywal_solar_phase_t *out_phase)
{
    if (!str || str[0] == '\0')
        return false;

    waywal_solar_phase_t p = WAYWAL_SOLAR_NIGHT;
    if (strcasecmp(str, "night") == 0 || strcasecmp(str, "midnight") == 0) {
        p = WAYWAL_SOLAR_NIGHT;
    } else if (strcasecmp(str, "dawn") == 0 || strcasecmp(str, "civil_dawn") == 0 ||
               strcasecmp(str, "sunrise") == 0) {
        p = WAYWAL_SOLAR_DAWN;
    } else if (strcasecmp(str, "day") == 0 || strcasecmp(str, "daylight") == 0 ||
               strcasecmp(str, "daytime") == 0) {
        p = WAYWAL_SOLAR_DAY;
    } else if (strcasecmp(str, "dusk") == 0 || strcasecmp(str, "civil_dusk") == 0) {
        p = WAYWAL_SOLAR_DUSK;
    } else if (strcasecmp(str, "morning") == 0) {
        p = WAYWAL_SOLAR_MORNING;
    } else if (strcasecmp(str, "noon") == 0 || strcasecmp(str, "solar_noon") == 0) {
        p = WAYWAL_SOLAR_NOON;
    } else if (strcasecmp(str, "afternoon") == 0 || strcasecmp(str, "golden_hour") == 0 ||
               strcasecmp(str, "golden-hour") == 0) {
        p = WAYWAL_SOLAR_AFTERNOON;
    } else if (strcasecmp(str, "sunset") == 0) {
        p = WAYWAL_SOLAR_SUNSET;
    } else if (strcasecmp(str, "evening") == 0) {
        p = WAYWAL_SOLAR_EVENING;
    } else {
        return false;
    }

    if (out_phase)
        *out_phase = p;
    return true;
}

bool waywal_parse_hhmm_minutes(const char *hhmm, int32_t *out_minutes_of_day)
{
    if (!hhmm)
        return false;
    while (*hhmm == ' ' || *hhmm == '\t')
        hhmm++;
    if (!isdigit((unsigned char)hhmm[0]))
        return false;

    char *end_h = NULL;
    long h = strtol(hhmm, &end_h, 10);
    if (!end_h || *end_h != ':' || h < 0 || h > 23)
        return false;

    const char *m_str = end_h + 1;
    if (!isdigit((unsigned char)m_str[0]) || !isdigit((unsigned char)m_str[1]))
        return false;

    char *end_m = NULL;
    long m = strtol(m_str, &end_m, 10);
    if (!end_m || m < 0 || m > 59)
        return false;

    while (*end_m == ' ' || *end_m == '\t')
        end_m++;
    if (*end_m != '\0')
        return false;

    if (out_minutes_of_day)
        *out_minutes_of_day = (int32_t)(h * 60 + m);
    return true;
}

bool waywal_dynwall_is_xml_path(const char *path)
{
    if (!path || path[0] == '\0')
        return false;
    const char *dot = strrchr(path, '.');
    return (dot != NULL && strcasecmp(dot, ".xml") == 0);
}

bool waywal_dynwall_is_manifest_path(const char *path)
{
    if (!path || path[0] == '\0')
        return false;
    if (waywal_dynwall_is_xml_path(path))
        return true;

    const char *dot = strrchr(path, '.');
    if (!dot || strcasecmp(dot, ".json") != 0)
        return false;

    const char *base = strrchr(path, '/');
    base = base ? (base + 1) : path;
    if (strcasecmp(base, "project.json") == 0 || strcasecmp(base, "scene.json") == 0 ||
        strcasecmp(base, "models.json") == 0 || strcasecmp(base, "materials.json") == 0) {
        return false;
    }
    return true;
}

bool waywal_dynamic_manifest_is_path(const char *path)
{
    return waywal_dynwall_is_manifest_path(path);
}

static bool path_has_dotdot_segment(const char *p)
{
    if (!p || p[0] == '\0')
        return true;
    const char *cur = p;
    while (*cur != '\0') {
        while (*cur == '/' || *cur == '\\')
            cur++;
        if (*cur == '\0')
            break;
        const char *seg_start = cur;
        while (*cur != '\0' && *cur != '/' && *cur != '\\')
            cur++;
        size_t seg_len = (size_t)(cur - seg_start);
        if (seg_len == 2 && seg_start[0] == '.' && seg_start[1] == '.') {
            return true;
        }
    }
    return false;
}

static bool resolve_manifest_frame_path(const char *raw_path, const char *base_dir, char *out_buf,
                                        size_t out_cap)
{
    if (!raw_path || !out_buf || out_cap == 0)
        return false;

    while (*raw_path == ' ' || *raw_path == '\t' || *raw_path == '\n' || *raw_path == '\r')
        raw_path++;

    size_t len = strlen(raw_path);
    while (len > 0 && (raw_path[len - 1] == ' ' || raw_path[len - 1] == '\t' ||
                       raw_path[len - 1] == '\n' || raw_path[len - 1] == '\r')) {
        len--;
    }
    if (len == 0 || len >= 4096)
        return false;

    char trimmed[4096];
    memcpy(trimmed, raw_path, len);
    trimmed[len] = '\0';

    if (path_has_dotdot_segment(trimmed) || strstr(trimmed, "&#") != NULL)
        return false;

    if (trimmed[0] == '~' || strchr(trimmed, '$') != NULL) {
        const char *raw_home = getenv("HOME");
        if (raw_home && path_has_dotdot_segment(raw_home))
            return false;
    }

    /* Reject any $VAR whose raw environment value contains '..' */
    if (strchr(trimmed, '$') != NULL) {
        const char *dlr = strchr(trimmed, '$');
        while (dlr) {
            const char *vstart = dlr + 1;
            bool braced = (*vstart == '{');
            if (braced)
                vstart++;
            const char *vend = vstart;
            while (*vend && (isalnum((unsigned char)*vend) || *vend == '_'))
                vend++;
            char vname[128] = {0};
            size_t vlen = (size_t)(vend - vstart);
            if (vlen > 0 && vlen < sizeof(vname)) {
                memcpy(vname, vstart, vlen);
                const char *ev = getenv(vname);
                if (ev && path_has_dotdot_segment(ev))
                    return false;
            }
            dlr = strchr(vend, '$');
        }
    }

    /*
     * Expand ~ and $VAR / ${VAR} WITHOUT running waywal_normalize_path() first,
     * so composite traversal like '/.$DOT/etc/passwd' (with DOT='.') produces
     * '/.. /etc/passwd' and is caught by path_has_dotdot_segment(unnorm) before
     * normalization can collapse '..'.
     */
    char unnorm[4096] = {0};
    if (trimmed[0] == '~') {
        if (trimmed[1] != '\0' && trimmed[1] != '/')
            return false;
        char home_buf[4096] = {0};
        if (!waywal_expand_path("~", home_buf, sizeof(home_buf)) || home_buf[0] == '\0' ||
            path_has_dotdot_segment(home_buf)) {
            return false;
        }
        if (trimmed[1] == '\0') {
            int hn = snprintf(unnorm, sizeof(unnorm), "%s", home_buf);
            if (hn < 0 || (size_t)hn >= sizeof(unnorm))
                return false;
        } else {
            char rel_in[4096];
            char rel_exp[4096];
            int rn = snprintf(rel_in, sizeof(rel_in), ".%s", trimmed + 1);
            if (rn < 0 || (size_t)rn >= sizeof(rel_in))
                return false;
            if (!waywal_expand_path(rel_in, rel_exp, sizeof(rel_exp)) || rel_exp[0] != '.')
                return false;
            if (path_has_dotdot_segment(rel_exp + 1))
                return false;
            int un = snprintf(unnorm, sizeof(unnorm), "%s%s", home_buf, rel_exp + 1);
            if (un < 0 || (size_t)un >= sizeof(unnorm))
                return false;
        }
    } else if (strchr(trimmed, '$') != NULL) {
        char rel_in[4096];
        char rel_exp[4096];
        int rn = snprintf(rel_in, sizeof(rel_in), "./%s", trimmed);
        if (rn < 0 || (size_t)rn >= sizeof(rel_in))
            return false;
        if (!waywal_expand_path(rel_in, rel_exp, sizeof(rel_exp)) || rel_exp[0] != '.' ||
            rel_exp[1] != '/') {
            return false;
        }
        int un = snprintf(unnorm, sizeof(unnorm), "%s", rel_exp + 2);
        if (un <= 0 || (size_t)un >= sizeof(unnorm))
            return false;
    } else {
        memcpy(unnorm, trimmed, len + 1);
    }

    if (unnorm[0] == '\0' || path_has_dotdot_segment(unnorm))
        return false;

    if (unnorm[0] == '/') {
        size_t ulen = strlen(unnorm);
        if (ulen + 1 > out_cap)
            return false;
        memcpy(out_buf, unnorm, ulen + 1);
        waywal_normalize_path(out_buf);
        return out_buf[0] != '\0' && !path_has_dotdot_segment(out_buf);
    }

    if (base_dir && base_dir[0] != '\0') {
        if (path_has_dotdot_segment(base_dir))
            return false;
        int n = snprintf(out_buf, out_cap, "%s/%s", base_dir, unnorm);
        if (n < 0 || (size_t)n >= out_cap)
            return false;
        if (path_has_dotdot_segment(out_buf))
            return false;
        if (out_buf[0] == '/') {
            waywal_normalize_path(out_buf);
        }
        return out_buf[0] != '\0' && !path_has_dotdot_segment(out_buf);
    }

    size_t ulen = strlen(unnorm);
    if (ulen + 1 > out_cap)
        return false;
    memcpy(out_buf, unnorm, ulen + 1);
    return !path_has_dotdot_segment(out_buf);
}

static bool xml_unescape_and_trim(const char *src, size_t len, char *dst, size_t dst_cap)
{
    if (!dst || dst_cap == 0)
        return false;
    dst[0] = '\0';
    if (!src || len == 0)
        return true;

    size_t start = 0;
    while (start < len && isspace((unsigned char)src[start]))
        start++;
    while (len > start && isspace((unsigned char)src[len - 1]))
        len--;

    size_t out = 0;
    size_t i = start;
    for (; i < len && out + 1 < dst_cap;) {
        if (src[i] == '&') {
            if (i + 5 <= len && strncmp(src + i, "&amp;", 5) == 0) {
                dst[out++] = '&';
                i += 5;
                continue;
            }
            if (i + 4 <= len && strncmp(src + i, "&lt;", 4) == 0) {
                dst[out++] = '<';
                i += 4;
                continue;
            }
            if (i + 4 <= len && strncmp(src + i, "&gt;", 4) == 0) {
                dst[out++] = '>';
                i += 4;
                continue;
            }
            if (i + 6 <= len && strncmp(src + i, "&quot;", 6) == 0) {
                dst[out++] = '"';
                i += 6;
                continue;
            }
            if (i + 6 <= len && strncmp(src + i, "&apos;", 6) == 0) {
                dst[out++] = '\'';
                i += 6;
                continue;
            }
        }
        dst[out++] = src[i++];
    }
    dst[out] = '\0';
    return (i == len);
}

static const char *find_xml_open_tag(const char *start, const char *end, const char *tag_name,
                                     const char **out_tag_gt)
{
    char pat[64];
    int n = snprintf(pat, sizeof(pat), "<%s", tag_name);
    if (n <= 1 || (size_t)n >= sizeof(pat))
        return NULL;
    size_t pat_len = (size_t)n;

    const char *p = start;
    while (p && p + pat_len < end) {
        const char *found = strstr(p, pat);
        if (!found || found + pat_len >= end)
            return NULL;
        char next_c = found[pat_len];
        if (next_c == '>' || isspace((unsigned char)next_c)) {
            const char *gt = strchr(found + pat_len, '>');
            if (!gt || gt >= end)
                return NULL;
            if (out_tag_gt)
                *out_tag_gt = gt;
            return found;
        }
        p = found + pat_len;
    }
    return NULL;
}

static bool xml_extract_child_tag(const char *block_start, const char *block_end,
                                  const char *tag_name, char *out_val, size_t out_cap)
{
    char close_pat[64];
    snprintf(close_pat, sizeof(close_pat), "</%s>", tag_name);

    const char *tag_gt = NULL;
    const char *found = find_xml_open_tag(block_start, block_end, tag_name, &tag_gt);
    if (!found || !tag_gt || tag_gt + 1 > block_end)
        return false;
    const char *val_start = tag_gt + 1;
    const char *close_found = strstr(val_start, close_pat);
    if (!close_found || close_found > block_end)
        return false;
    return xml_unescape_and_trim(val_start, (size_t)(close_found - val_start), out_val, out_cap);
}

bool waywal_dynwall_parse_xml_string(const char *xml_text, size_t xml_len, const char *base_dir,
                                     waywal_dynwall_manifest_t *out_manifest)
{
    if (!xml_text || xml_len == 0 || !out_manifest)
        return false;

    memset(out_manifest, 0, sizeof(*out_manifest));
    out_manifest->kind = WAYWAL_DYNWALL_KIND_XML_TIMED;

    char *buf = (char *)malloc(xml_len + 1);
    if (!buf)
        return false;
    memcpy(buf, xml_text, xml_len);
    buf[xml_len] = '\0';

    /* Strip XML comments <!-- ... --> in-place with spaces so strstr never matches commented tags */
    char *c_pos = buf;
    while ((c_pos = strstr(c_pos, "<!--")) != NULL) {
        char *c_end = strstr(c_pos + 4, "-->");
        if (!c_end) {
            free(buf);
            return false;
        }
        memset(c_pos, ' ', (size_t)((c_end + 3) - c_pos));
        c_pos = c_end + 3;
    }

    const char *end = buf + xml_len;
    const char *st_gt = NULL;
    const char *st_open = find_xml_open_tag(buf, end, "starttime", &st_gt);
    if (st_open && st_gt) {
        const char *st_close = strstr(st_gt + 1, "</starttime>");
        if (!st_close) {
            free(buf);
            return false;
        }
        char vbuf[64];
        out_manifest->has_start_tm = true;
        if (xml_extract_child_tag(st_gt + 1, st_close, "year", vbuf, sizeof(vbuf))) {
            int y = atoi(vbuf);
            if (y >= 1900)
                out_manifest->start_tm.tm_year = y - 1900;
        }
        if (xml_extract_child_tag(st_gt + 1, st_close, "month", vbuf, sizeof(vbuf))) {
            int m = atoi(vbuf);
            if (m >= 1 && m <= 12)
                out_manifest->start_tm.tm_mon = m - 1;
        }
        if (xml_extract_child_tag(st_gt + 1, st_close, "day", vbuf, sizeof(vbuf))) {
            int d = atoi(vbuf);
            if (d >= 1 && d <= 31)
                out_manifest->start_tm.tm_mday = d;
        }
        if (xml_extract_child_tag(st_gt + 1, st_close, "hour", vbuf, sizeof(vbuf))) {
            int hr = atoi(vbuf);
            if (hr >= 0 && hr <= 23)
                out_manifest->start_tm.tm_hour = hr;
        }
        if (xml_extract_child_tag(st_gt + 1, st_close, "minute", vbuf, sizeof(vbuf))) {
            int mn = atoi(vbuf);
            if (mn >= 0 && mn <= 59)
                out_manifest->start_tm.tm_min = mn;
        }
        if (xml_extract_child_tag(st_gt + 1, st_close, "second", vbuf, sizeof(vbuf))) {
            int sc = atoi(vbuf);
            if (sc >= 0 && sc <= 60)
                out_manifest->start_tm.tm_sec = sc;
        }
    }

    const char *cursor = buf;
    double cumulative_sec = 0.0;

    while (cursor < end) {
        const char *static_gt = NULL;
        const char *trans_gt = NULL;
        const char *next_static = find_xml_open_tag(cursor, end, "static", &static_gt);
        const char *next_trans = find_xml_open_tag(cursor, end, "transition", &trans_gt);
        if (!next_static && !next_trans)
            break;

        bool is_trans = false;
        const char *tag_gt = NULL;
        if (next_static && (!next_trans || next_static < next_trans)) {
            tag_gt = static_gt;
            is_trans = false;
        } else {
            tag_gt = trans_gt;
            is_trans = true;
        }

        if (!tag_gt) {
            free(buf);
            return false;
        }
        const char *close_tag = is_trans ? "</transition>" : "</static>";
        const char *tag_close = strstr(tag_gt + 1, close_tag);
        if (!tag_close) {
            free(buf);
            return false;
        }

        if (out_manifest->entry_count >= WAYWAL_DYNAMIC_MAX_STEPS) {
            free(buf);
            return false;
        }

        char dur_str[64] = {0};
        if (!xml_extract_child_tag(tag_gt + 1, tag_close, "duration", dur_str, sizeof(dur_str))) {
            free(buf);
            return false;
        }
        char *dur_end = NULL;
        double dur = strtod(dur_str, &dur_end);
        while (dur_end && isspace((unsigned char)*dur_end))
            dur_end++;
        if (!dur_end || dur_end == dur_str || *dur_end != '\0' || !isfinite(dur) || dur <= 0.0 ||
            dur > 315360000.0 || !isfinite(cumulative_sec + dur) ||
            cumulative_sec + dur > 315360000.0 * (double)WAYWAL_DYNAMIC_MAX_STEPS) {
            free(buf);
            return false;
        }

        waywal_dynwall_entry_t *ent = &out_manifest->entries[out_manifest->entry_count];
        memset(ent, 0, sizeof(*ent));
        ent->duration_sec = dur;
        ent->start_offset_sec = cumulative_sec;
        ent->time_minutes_of_day = -1;
        ent->phase = WAYWAL_SOLAR_PHASE_COUNT;
        ent->is_transition = is_trans;

        if (!is_trans) {
            char raw_file[4096] = {0};
            if (!xml_extract_child_tag(tag_gt + 1, tag_close, "file", raw_file, sizeof(raw_file)) ||
                !resolve_manifest_frame_path(raw_file, base_dir, ent->from_path,
                                             sizeof(ent->from_path))) {
                free(buf);
                return false;
            }
        } else {
            char raw_from[4096] = {0};
            char raw_to[4096] = {0};
            if (!xml_extract_child_tag(tag_gt + 1, tag_close, "from", raw_from, sizeof(raw_from)) ||
                !xml_extract_child_tag(tag_gt + 1, tag_close, "to", raw_to, sizeof(raw_to)) ||
                !resolve_manifest_frame_path(raw_from, base_dir, ent->from_path,
                                             sizeof(ent->from_path)) ||
                !resolve_manifest_frame_path(raw_to, base_dir, ent->to_path,
                                             sizeof(ent->to_path))) {
                free(buf);
                return false;
            }
        }

        cumulative_sec += dur;
        out_manifest->entry_count++;
        cursor = tag_close + strlen(close_tag);
    }

    free(buf);
    if (out_manifest->entry_count == 0 || cumulative_sec <= 0.0)
        return false;

    out_manifest->total_duration_sec = cumulative_sec;
    return true;
}

static bool parse_json_manifest_entry_obj(const we_jval_t *item, const char *base_dir,
                                          waywal_dynwall_entry_t *ent)
{
    if (!item || item->type != JVAL_OBJ || !ent)
        return false;

    memset(ent, 0, sizeof(*ent));
    ent->time_minutes_of_day = -1;
    ent->phase = WAYWAL_SOLAR_PHASE_COUNT;

    const char *raw_path = we_jval_str(we_jobj_get(item, "file"), NULL);
    if (!raw_path)
        raw_path = we_jval_str(we_jobj_get(item, "path"), NULL);
    if (!raw_path)
        raw_path = we_jval_str(we_jobj_get(item, "image"), NULL);
    if (!raw_path ||
        !resolve_manifest_frame_path(raw_path, base_dir, ent->from_path, sizeof(ent->from_path))) {
        return false;
    }

    const we_jval_t *elev_v = we_jobj_get(item, "elevation");
    if (!elev_v)
        elev_v = we_jobj_get(item, "altitude");
    if (elev_v && elev_v->type == JVAL_NUM && isfinite(elev_v->num)) {
        ent->target_elevation_deg = elev_v->num;
        ent->has_elevation = true;
    }

    const we_jval_t *az_v = we_jobj_get(item, "azimuth");
    if (az_v && az_v->type == JVAL_NUM && isfinite(az_v->num)) {
        ent->target_azimuth_deg = az_v->num;
        ent->has_azimuth = true;
    }

    const char *phase_s = we_jval_str(we_jobj_get(item, "phase"), NULL);
    if (phase_s) {
        waywal_solar_phase_t ph;
        if (!waywal_solar_phase_from_string(phase_s, &ph))
            return false;
        ent->phase = ph;
        ent->has_phase = true;
    }

    const char *time_s = we_jval_str(we_jobj_get(item, "time"), NULL);
    if (!time_s)
        time_s = we_jval_str(we_jobj_get(item, "start"), NULL);
    if (time_s) {
        int32_t mins = -1;
        if (!waywal_parse_hhmm_minutes(time_s, &mins))
            return false;
        ent->time_minutes_of_day = mins;
    }

    const char *tmode_s = we_jval_str(we_jobj_get(item, "theme_mode"), NULL);
    if (!tmode_s)
        tmode_s = we_jval_str(we_jobj_get(item, "mode"), NULL);
    if (tmode_s) {
        if (strcasecmp(tmode_s, "light") == 0) {
            ent->has_theme_mode = true;
            ent->theme_light_mode = true;
        } else if (strcasecmp(tmode_s, "dark") == 0) {
            ent->has_theme_mode = true;
            ent->theme_light_mode = false;
        }
    } else {
        const we_jval_t *light_v = we_jobj_get(item, "light");
        if (!light_v)
            light_v = we_jobj_get(item, "is_light");
        if (light_v && light_v->type == JVAL_BOOL) {
            ent->has_theme_mode = true;
            ent->theme_light_mode = light_v->bval;
        }
    }

    return (ent->has_elevation || ent->has_phase || ent->time_minutes_of_day >= 0);
}

bool waywal_dynwall_parse_json_string(const char *json_text, size_t json_len, const char *base_dir,
                                      waywal_dynwall_manifest_t *out_manifest)
{
    if (!json_text || json_len == 0 || !out_manifest)
        return false;

    memset(out_manifest, 0, sizeof(*out_manifest));
    out_manifest->kind = WAYWAL_DYNWALL_KIND_JSON_SOLAR;

    we_jval_t *root = we_json_parse_root(json_text, json_len);
    if (!root)
        return false;

    const we_jval_t *arr = NULL;
    if (root->type == JVAL_ARR) {
        arr = root;
    } else if (root->type == JVAL_OBJ) {
        const char *arr_keys[] = {"steps",    "images", "frames", "solar",
                                  "schedule", "entries", "times"};
        for (size_t i = 0; i < sizeof(arr_keys) / sizeof(arr_keys[0]); ++i) {
            const we_jval_t *cand = we_jobj_get(root, arr_keys[i]);
            if (cand && cand->type == JVAL_ARR) {
                arr = cand;
                break;
            }
        }
    }

    if (arr && arr->type == JVAL_ARR) {
        if (arr->arr.count == 0 || arr->arr.count > WAYWAL_DYNAMIC_MAX_STEPS) {
            we_jval_free(root);
            return false;
        }
        for (size_t i = 0; i < arr->arr.count; ++i) {
            if (!parse_json_manifest_entry_obj(arr->arr.items[i], base_dir,
                                               &out_manifest->entries[out_manifest->entry_count])) {
                we_jval_free(root);
                return false;
            }
            out_manifest->entry_count++;
        }
        we_jval_free(root);
        return out_manifest->entry_count > 0;
    }

    if (root->type == JVAL_OBJ) {
        const we_jval_t *map_obj = root;
        const we_jval_t *sub_times = we_jobj_get(root, "times");
        if (!sub_times || sub_times->type != JVAL_OBJ)
            sub_times = we_jobj_get(root, "schedule");
        if (!sub_times || sub_times->type != JVAL_OBJ)
            sub_times = we_jobj_get(root, "solar");
        if (sub_times && sub_times->type == JVAL_OBJ)
            map_obj = sub_times;

        if (map_obj->obj.count == 0 || map_obj->obj.count > WAYWAL_DYNAMIC_MAX_STEPS) {
            we_jval_free(root);
            return false;
        }

        for (size_t i = 0; i < map_obj->obj.count; ++i) {
            const char *k = map_obj->obj.entries[i].key;
            const we_jval_t *v = map_obj->obj.entries[i].val;
            if (!k || !v)
                continue;

            if (v->type == JVAL_STR) {
                waywal_solar_phase_t ph = WAYWAL_SOLAR_PHASE_COUNT;
                int32_t mins = -1;
                bool is_ph = waywal_solar_phase_from_string(k, &ph);
                bool is_tm = !is_ph && waywal_parse_hhmm_minutes(k, &mins);
                if (!is_ph && !is_tm)
                    continue;

                if (out_manifest->entry_count >= WAYWAL_DYNAMIC_MAX_STEPS) {
                    we_jval_free(root);
                    return false;
                }
                waywal_dynwall_entry_t *ent = &out_manifest->entries[out_manifest->entry_count];
                memset(ent, 0, sizeof(*ent));
                ent->time_minutes_of_day = mins;
                ent->phase = ph;
                ent->has_phase = is_ph;
                if (!resolve_manifest_frame_path(v->str, base_dir, ent->from_path,
                                                 sizeof(ent->from_path))) {
                    we_jval_free(root);
                    return false;
                }
                if (is_ph) {
                    ent->has_theme_mode = true;
                    ent->theme_light_mode =
                        (ph == WAYWAL_SOLAR_DAY || ph == WAYWAL_SOLAR_MORNING ||
                         ph == WAYWAL_SOLAR_NOON || ph == WAYWAL_SOLAR_AFTERNOON ||
                         ph == WAYWAL_SOLAR_DAWN);
                }
                out_manifest->entry_count++;
            } else if (v->type == JVAL_OBJ) {
                if (out_manifest->entry_count >= WAYWAL_DYNAMIC_MAX_STEPS) {
                    we_jval_free(root);
                    return false;
                }
                waywal_dynwall_entry_t *ent = &out_manifest->entries[out_manifest->entry_count];
                if (!parse_json_manifest_entry_obj(v, base_dir, ent)) {
                    waywal_solar_phase_t ph = WAYWAL_SOLAR_PHASE_COUNT;
                    int32_t mins = -1;
                    bool is_ph = waywal_solar_phase_from_string(k, &ph);
                    bool is_tm = !is_ph && waywal_parse_hhmm_minutes(k, &mins);
                    const char *raw_p = we_jval_str(we_jobj_get(v, "file"), NULL);
                    if (!raw_p)
                        raw_p = we_jval_str(we_jobj_get(v, "path"), NULL);
                    if ((!is_ph && !is_tm) || !raw_p ||
                        !resolve_manifest_frame_path(raw_p, base_dir, ent->from_path,
                                                     sizeof(ent->from_path))) {
                        we_jval_free(root);
                        return false;
                    }
                    ent->phase = ph;
                    ent->has_phase = is_ph;
                    ent->time_minutes_of_day = mins;
                }
                out_manifest->entry_count++;
            }
        }
    }

    we_jval_free(root);
    return out_manifest->entry_count > 0;
}

bool waywal_dynwall_load_file(const char *filepath, waywal_dynwall_manifest_t *out_manifest)
{
    if (!filepath || filepath[0] == '\0' || !out_manifest)
        return false;

    char expanded[4096];
    const char *use_path = filepath;
    if (filepath[0] == '~' || filepath[0] == '$') {
        if (!waywal_expand_path(filepath, expanded, sizeof(expanded)))
            return false;
        use_path = expanded;
    }

    int fd = open(use_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > 4L * 1024L * 1024L) {
        close(fd);
        return false;
    }

    size_t sz = (size_t)st.st_size;
    char *buf = (char *)malloc(sz + 1);
    if (!buf) {
        close(fd);
        return false;
    }

    size_t total = 0;
    while (total < sz) {
        ssize_t r = read(fd, buf + total, sz - total);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return false;
        }
        if (r == 0)
            break;
        total += (size_t)r;
    }
    close(fd);
    buf[total] = '\0';

    char base_dir[4096] = {0};
    const char *slash = strrchr(use_path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - use_path);
        if (dlen == 0)
            dlen = 1;
        if (dlen < sizeof(base_dir)) {
            memcpy(base_dir, use_path, dlen);
            base_dir[dlen] = '\0';
        }
    }

    bool ok = false;
    if (waywal_dynwall_is_xml_path(use_path)) {
        ok = waywal_dynwall_parse_xml_string(buf, total, base_dir, out_manifest);
    } else {
        ok = waywal_dynwall_parse_json_string(buf, total, base_dir, out_manifest);
    }

    free(buf);
    return ok;
}

bool waywal_dynamic_manifest_load(const char *filepath, waywal_dynamic_manifest_t *out_manifest)
{
    return waywal_dynwall_load_file(filepath, out_manifest);
}

static bool is_fine_solar_subphase(waywal_solar_phase_t p)
{
    return (p == WAYWAL_SOLAR_MORNING || p == WAYWAL_SOLAR_NOON || p == WAYWAL_SOLAR_AFTERNOON ||
            p == WAYWAL_SOLAR_SUNSET || p == WAYWAL_SOLAR_EVENING);
}

static bool fine_subphase_matches(waywal_solar_phase_t entry_phase,
                                  const waywal_solar_state_t *solar)
{
    if (!solar || !is_fine_solar_subphase(entry_phase))
        return false;
    if (entry_phase == solar->phase)
        return true;
    if (solar->phase == WAYWAL_SOLAR_DAY) {
        if (entry_phase == WAYWAL_SOLAR_MORNING && solar->hour_angle_deg < -15.0)
            return true;
        if (entry_phase == WAYWAL_SOLAR_NOON && solar->hour_angle_deg >= -15.0 &&
            solar->hour_angle_deg <= 15.0)
            return true;
        if (entry_phase == WAYWAL_SOLAR_AFTERNOON && solar->hour_angle_deg > 15.0)
            return true;
    }
    if (solar->phase == WAYWAL_SOLAR_DUSK) {
        if (entry_phase == WAYWAL_SOLAR_SUNSET && solar->is_daylight)
            return true;
        if (entry_phase == WAYWAL_SOLAR_EVENING && !solar->is_daylight)
            return true;
    }
    return false;
}

bool waywal_dynwall_resolve(const waywal_dynwall_manifest_t *manifest, time_t utc_now,
                            const struct tm *local_tm, const waywal_solar_state_t *solar_opt,
                            waywal_dynwall_resolved_t *out_resolved)
{
    if (!manifest || manifest->entry_count == 0 || !out_resolved)
        return false;

    memset(out_resolved, 0, sizeof(*out_resolved));

    struct tm fallback_tm;
    if (!local_tm) {
        localtime_r(&utc_now, &fallback_tm);
        local_tm = &fallback_tm;
    }

    if (manifest->kind == WAYWAL_DYNWALL_KIND_XML_TIMED) {
        double total = manifest->total_duration_sec;
        if (!isfinite(total) || total <= 0.0)
            return false;

        double now_tod = (double)local_tm->tm_hour * 3600.0 + (double)local_tm->tm_min * 60.0 +
                         (double)local_tm->tm_sec;
        double start_tod = 0.0;
        if (manifest->has_start_tm) {
            start_tod = (double)manifest->start_tm.tm_hour * 3600.0 +
                        (double)manifest->start_tm.tm_min * 60.0 +
                        (double)manifest->start_tm.tm_sec;
        }

        double cycle_pos = fmod(now_tod - start_tod, total);
        if (cycle_pos < 0.0)
            cycle_pos += total;
        if (!isfinite(cycle_pos) || cycle_pos < 0.0 || cycle_pos >= total)
            cycle_pos = 0.0;

        size_t chosen = manifest->entry_count - 1;
        for (size_t i = 0; i < manifest->entry_count; ++i) {
            const waywal_dynwall_entry_t *ent = &manifest->entries[i];
            if (cycle_pos >= ent->start_offset_sec &&
                cycle_pos < ent->start_offset_sec + ent->duration_sec) {
                chosen = i;
                break;
            }
        }

        const waywal_dynwall_entry_t *active = &manifest->entries[chosen];
        double into_seg = cycle_pos - active->start_offset_sec;
        if (into_seg < 0.0)
            into_seg = 0.0;
        double rem_sec = active->duration_sec - into_seg;
        if (active->is_transition && active->to_path[0] != '\0' && active->duration_sec > 0.0 &&
            into_seg < 0.5 * active->duration_sec) {
            rem_sec = (0.5 * active->duration_sec) - into_seg;
        }
        if (!isfinite(rem_sec) || rem_sec < 1.0)
            rem_sec = 1.0;
        if (rem_sec > 315360000.0)
            rem_sec = 315360000.0;

        snprintf(out_resolved->primary_path, sizeof(out_resolved->primary_path), "%s",
                 active->from_path);
        out_resolved->active_index = chosen;
        out_resolved->next_change_sec = (uint64_t)ceil(rem_sec);

        if (active->is_transition && active->to_path[0] != '\0' && active->duration_sec > 0.0) {
            snprintf(out_resolved->secondary_path, sizeof(out_resolved->secondary_path), "%s",
                     active->to_path);
            out_resolved->is_transition = true;
            out_resolved->blend_factor =
                (float)clamp_d(into_seg / active->duration_sec, 0.0, 1.0);
        }
        return true;
    }

    waywal_solar_state_t computed_solar;
    const waywal_solar_state_t *solar = solar_opt;
    if (!solar) {
        waywal_solar_synthesize_local(390, 1110, utc_now, local_tm, &computed_solar);
        solar = &computed_solar;
    }

    /* 1. Solar elevation / azimuth matching */
    bool any_elev = false;
    for (size_t i = 0; i < manifest->entry_count; ++i) {
        if (manifest->entries[i].has_elevation) {
            any_elev = true;
            break;
        }
    }

    if (any_elev) {
        size_t best_idx = 0;
        double best_score = 1e18;
        bool is_rising = (solar->hour_angle_deg < 0.0);

        for (size_t i = 0; i < manifest->entry_count; ++i) {
            const waywal_dynwall_entry_t *ent = &manifest->entries[i];
            if (!ent->has_elevation)
                continue;
            double score = fabs(solar->elevation_deg - ent->target_elevation_deg);
            if (ent->has_azimuth) {
                double az_diff = fabs(solar->azimuth_deg - ent->target_azimuth_deg);
                if (az_diff > 180.0)
                    az_diff = 360.0 - az_diff;
                score += 0.1 * az_diff;
            }
            if (ent->has_phase) {
                bool ent_rising =
                    (ent->phase == WAYWAL_SOLAR_DAWN || ent->phase == WAYWAL_SOLAR_MORNING);
                bool ent_setting =
                    (ent->phase == WAYWAL_SOLAR_DUSK || ent->phase == WAYWAL_SOLAR_SUNSET ||
                     ent->phase == WAYWAL_SOLAR_EVENING);
                if ((ent_rising && !is_rising) || (ent_setting && is_rising)) {
                    score += 25.0;
                }
            }
            if (score < best_score) {
                best_score = score;
                best_idx = i;
            }
        }

        const waywal_dynwall_entry_t *active = &manifest->entries[best_idx];
        snprintf(out_resolved->primary_path, sizeof(out_resolved->primary_path), "%s",
                 active->from_path);
        out_resolved->active_index = best_idx;
        out_resolved->next_change_sec = 60U;
        out_resolved->has_theme_mode = active->has_theme_mode;
        out_resolved->theme_light_mode = active->theme_light_mode;
        return true;
    }

    /* 2. Solar phase matching (two-pass: fine-grained subphases first, coarse phases second) */
    bool any_phase = false;
    for (size_t i = 0; i < manifest->entry_count; ++i) {
        if (manifest->entries[i].has_phase) {
            any_phase = true;
            break;
        }
    }

    if (any_phase) {
        size_t chosen = SIZE_MAX;
        /* Pass 1a: exact fine-grained subphase match (morning / noon / afternoon / sunset / evening) */
        for (size_t i = 0; i < manifest->entry_count; ++i) {
            const waywal_dynwall_entry_t *ent = &manifest->entries[i];
            if (ent->has_phase && fine_subphase_matches(ent->phase, solar)) {
                chosen = i;
                break;
            }
        }
        /* Pass 1b: if in dusk and only one of sunset/evening is defined, match it before coarse dusk */
        if (chosen == SIZE_MAX && solar->phase == WAYWAL_SOLAR_DUSK) {
            for (size_t i = 0; i < manifest->entry_count; ++i) {
                const waywal_dynwall_entry_t *ent = &manifest->entries[i];
                if (ent->has_phase &&
                    (ent->phase == WAYWAL_SOLAR_SUNSET || ent->phase == WAYWAL_SOLAR_EVENING)) {
                    chosen = i;
                    break;
                }
            }
        }
        /* Pass 2: coarse phase match (night / dawn / day / dusk) */
        if (chosen == SIZE_MAX) {
            for (size_t i = 0; i < manifest->entry_count; ++i) {
                const waywal_dynwall_entry_t *ent = &manifest->entries[i];
                if (ent->has_phase && ent->phase == solar->phase) {
                    chosen = i;
                    break;
                }
            }
        }
        /* Pass 3: daylight / night fallback */
        if (chosen == SIZE_MAX) {
            waywal_solar_phase_t fallback =
                solar->is_daylight ? WAYWAL_SOLAR_DAY : WAYWAL_SOLAR_NIGHT;
            for (size_t i = 0; i < manifest->entry_count; ++i) {
                if (manifest->entries[i].has_phase && manifest->entries[i].phase == fallback) {
                    chosen = i;
                    break;
                }
            }
        }
        if (chosen == SIZE_MAX)
            chosen = 0;

        const waywal_dynwall_entry_t *active = &manifest->entries[chosen];
        snprintf(out_resolved->primary_path, sizeof(out_resolved->primary_path), "%s",
                 active->from_path);
        out_resolved->active_index = chosen;
        out_resolved->next_change_sec = 60U;
        out_resolved->has_theme_mode = active->has_theme_mode;
        out_resolved->theme_light_mode = active->theme_light_mode;
        return true;
    }

    /* 3. Wall-clock HH:MM matching (with midnight wraparound) */
    int32_t cur_min = local_tm->tm_hour * 60 + local_tm->tm_min;
    size_t best_le_idx = SIZE_MAX;
    int32_t best_le_min = -1;
    size_t max_idx = 0;
    int32_t max_min = -1;

    for (size_t i = 0; i < manifest->entry_count; ++i) {
        int32_t tm_min = manifest->entries[i].time_minutes_of_day;
        if (tm_min < 0)
            continue;
        if (tm_min > max_min) {
            max_min = tm_min;
            max_idx = i;
        }
        if (tm_min <= cur_min && tm_min >= best_le_min) {
            best_le_min = tm_min;
            best_le_idx = i;
        }
    }

    size_t chosen = (best_le_idx != SIZE_MAX) ? best_le_idx : max_idx;
    const waywal_dynwall_entry_t *active = &manifest->entries[chosen];
    snprintf(out_resolved->primary_path, sizeof(out_resolved->primary_path), "%s",
             active->from_path);
    out_resolved->active_index = chosen;
    out_resolved->next_change_sec = 60U;
    out_resolved->has_theme_mode = active->has_theme_mode;
    out_resolved->theme_light_mode = active->theme_light_mode;
    return true;
}

bool waywal_dynamic_manifest_eval(const waywal_dynamic_manifest_t *manifest, time_t utc_now,
                                  const waywal_solar_state_t *solar_opt,
                                  waywal_dynwall_resolved_t *out_resolved)
{
    struct tm local_tm;
    localtime_r(&utc_now, &local_tm);
    return waywal_dynwall_resolve(manifest, utc_now, &local_tm, solar_opt, out_resolved);
}
