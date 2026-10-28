// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_SOLAR_SCHEDULE_H
#define WAYWAL_SOLAR_SCHEDULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAYWAL_DYNAMIC_MAX_STEPS    64U
#define WAYWAL_DYNWALL_MAX_ENTRIES  WAYWAL_DYNAMIC_MAX_STEPS
#define WAYWAL_MANIFEST_MAX_SEGMENTS WAYWAL_DYNAMIC_MAX_STEPS

typedef enum {
    WAYWAL_SOLAR_NIGHT = 0,     /* Solar elevation < -6.0 deg (after dusk / before dawn) */
    WAYWAL_SOLAR_DAWN = 1,      /* Civil dawn (-6.0 deg <= elev < +6.0 deg, rising)      */
    WAYWAL_SOLAR_DAY = 2,       /* Daylight (elev >= +6.0 deg, or polar summer day)      */
    WAYWAL_SOLAR_DUSK = 3,      /* Civil dusk (-6.0 deg <= elev < +6.0 deg, setting)     */
    WAYWAL_SOLAR_MORNING = 4,   /* Fine-grained morning alias                            */
    WAYWAL_SOLAR_NOON = 5,      /* Fine-grained solar noon alias                         */
    WAYWAL_SOLAR_AFTERNOON = 6, /* Fine-grained afternoon / golden hour alias            */
    WAYWAL_SOLAR_SUNSET = 7,    /* Fine-grained sunset alias                             */
    WAYWAL_SOLAR_EVENING = 8,   /* Fine-grained evening alias                            */
    WAYWAL_SOLAR_PHASE_COUNT = 9,

    /* Descriptive & legacy aliases */
    WAYWAL_SOLAR_SUNRISE = WAYWAL_SOLAR_DAWN,
    WAYWAL_SOLAR_GOLDEN_HOUR = WAYWAL_SOLAR_AFTERNOON,

    WAYWAL_SOLAR_PHASE_NIGHT = WAYWAL_SOLAR_NIGHT,
    WAYWAL_SOLAR_PHASE_DAWN = WAYWAL_SOLAR_DAWN,
    WAYWAL_SOLAR_PHASE_DAY = WAYWAL_SOLAR_DAY,
    WAYWAL_SOLAR_PHASE_DUSK = WAYWAL_SOLAR_DUSK,
    WAYWAL_SOLAR_PHASE_MORNING = WAYWAL_SOLAR_MORNING,
    WAYWAL_SOLAR_PHASE_SUNRISE = WAYWAL_SOLAR_SUNRISE,
    WAYWAL_SOLAR_PHASE_NOON = WAYWAL_SOLAR_NOON,
    WAYWAL_SOLAR_PHASE_AFTERNOON = WAYWAL_SOLAR_AFTERNOON,
    WAYWAL_SOLAR_PHASE_GOLDEN_HOUR = WAYWAL_SOLAR_GOLDEN_HOUR,
    WAYWAL_SOLAR_PHASE_SUNSET = WAYWAL_SOLAR_SUNSET,
    WAYWAL_SOLAR_PHASE_EVENING = WAYWAL_SOLAR_EVENING,
} waywal_solar_phase_t;

typedef struct {
    double julian_day;         /* Julian Day (JD) from UTC timestamp                    */
    double julian_century;     /* Julian Century T = (JD - 2451545.0) / 36525.0         */
    double geom_mean_lon_deg;  /* Geometric Mean Longitude of Sun L0 [0..360)           */
    double geom_mean_anom_deg; /* Geometric Mean Anomaly of Sun M (deg)                 */
    double eq_of_center_deg;   /* Sun Equation of Center C (deg)                        */
    double apparent_lon_deg;   /* Apparent solar longitude lambda (deg)                 */
    double obliquity_deg;      /* Obliquity of the ecliptic epsilon (deg)               */
    union {
        double declination_deg;
        double declination;
    };
    union {
        double eq_of_time_min;
        double eq_time_min;
        double equation_of_time;
    };
    double true_solar_time_min;
    double hour_angle_deg;
    union {
        double elevation_deg;
        double elevation;
        double altitude_deg;
        double altitude;
    };
    union {
        double azimuth_deg;
        double azimuth;
    };
    double solar_noon_min; /* Minutes from midnight [0..1440)                       */
    double sunrise_min;    /* Minutes from midnight (-0.833 deg)                    */
    double sunset_min;     /* Minutes from midnight (-0.833 deg)                    */
    double civil_dawn_min; /* Minutes from midnight (-6.0 deg)                      */
    double civil_dusk_min; /* Minutes from midnight (-6.0 deg)                      */
    union {
        time_t sunrise_utc;
        time_t sunrise_epoch;
        time_t sunrise_time;
    };
    union {
        time_t sunset_utc;
        time_t sunset_epoch;
        time_t sunset_time;
    };
    union {
        time_t solar_noon_utc;
        time_t solar_noon_epoch;
        time_t noon_time;
    };
    union {
        time_t dawn_utc;
        time_t dawn_epoch;
    };
    union {
        time_t dusk_utc;
        time_t dusk_epoch;
    };
    bool has_sunrise_sunset; /* false during polar day or polar night                 */
    bool is_polar_day;       /* true when sun never drops below -0.833 deg            */
    bool is_polar_night;     /* true when sun never rises above -0.833 deg            */
    union {
        bool is_daylight;
        bool is_daytime;
    };
    waywal_solar_phase_t phase;
} waywal_solar_state_t;

typedef waywal_solar_state_t waywal_solar_pos_t;

typedef enum {
    WAYWAL_DYNWALL_KIND_XML_TIMED = 0, /* GNOME <background><starttime> + <static>/<transition> */
    WAYWAL_DYNWALL_KIND_JSON_SOLAR,    /* JSON solar elevation / phase / time-of-day manifest   */
} waywal_dynwall_kind_t;

typedef struct {
    union {
        char from_path[4096];
        char path[4096];
        char file_path[4096];
    };
    char to_path[4096];          /* Non-empty only for <transition> segments             */
    double duration_sec;         /* Duration in seconds (for XML segments)               */
    double start_offset_sec;     /* Cumulative seconds from starttime                    */
    double target_elevation_deg; /* For solar elevation manifests                        */
    double target_azimuth_deg;
    int32_t time_minutes_of_day; /* [0..1439] for HH:MM entries, or -1 if unused         */
    waywal_solar_phase_t phase;  /* Phase tag if phase-based, or WAYWAL_SOLAR_PHASE_COUNT */
    bool is_transition;
    bool has_elevation;
    bool has_azimuth;
    bool has_phase;
    bool has_theme_mode;
    bool theme_light_mode;
} waywal_dynwall_entry_t;

typedef waywal_dynwall_entry_t waywal_dynamic_step_t;

typedef struct {
    waywal_dynwall_kind_t kind;
    struct tm start_tm; /* Parsed from <starttime> (year, mon, mday, hour, min, sec) */
    bool has_start_tm;
    double total_duration_sec; /* Total cycle duration (usually 86400.0s)              */
    union {
        waywal_dynwall_entry_t entries[WAYWAL_DYNAMIC_MAX_STEPS];
        waywal_dynwall_entry_t steps[WAYWAL_DYNAMIC_MAX_STEPS];
    };
    union {
        size_t entry_count;
        size_t step_count;
    };
} waywal_dynwall_manifest_t;

typedef waywal_dynwall_manifest_t waywal_dynamic_manifest_t;

typedef struct {
    union {
        char primary_path[4096];
        char path[4096];
        char from_path[4096];
    };
    union {
        char secondary_path[4096];
        char to_path[4096];
    };
    union {
        float blend_factor; /* [0.0..1.0] progress from primary to secondary        */
        float progress;
    };
    uint64_t next_change_sec; /* Seconds until next segment boundary (>= 1)           */
    size_t active_index;
    bool is_transition;
    bool has_theme_mode;
    bool theme_light_mode;
} waywal_dynwall_resolved_t;

typedef waywal_dynwall_resolved_t waywal_dynamic_eval_t;

/* 1. NOAA Solar Position Calculator */
waywal_solar_state_t waywal_solar_calculate(double lat_deg, double lon_deg, time_t utc_now);

void waywal_solar_compute(double latitude_deg, double longitude_deg, time_t utc_now,
                          const struct tm *local_tm, waywal_solar_state_t *out_state);

bool waywal_solar_is_daytime_at(double latitude_deg, double longitude_deg, bool has_coordinates,
                                time_t utc_now, const struct tm *local_tm,
                                waywal_solar_phase_t *out_phase);

bool waywal_solar_is_daytime_at_ex(double latitude_deg, double longitude_deg, bool has_coordinates,
                                   int32_t sunrise_fallback_min, int32_t sunset_fallback_min,
                                   time_t utc_now, const struct tm *local_tm,
                                   waywal_solar_phase_t *out_phase);

void waywal_solar_synthesize_local(int32_t sunrise_fallback_min, int32_t sunset_fallback_min,
                                   time_t utc_now, const struct tm *local_tm,
                                   waywal_solar_state_t *out_state);

const char *waywal_solar_phase_to_string(waywal_solar_phase_t phase);
bool waywal_solar_phase_from_string(const char *str, waywal_solar_phase_t *out_phase);
bool waywal_parse_hhmm_minutes(const char *hhmm, int32_t *out_minutes_of_day);

/* 2. Zero-Dependency Dynamic Wallpaper Manifest Parsers (.xml and .json) */
bool waywal_dynwall_is_xml_path(const char *path);
bool waywal_dynwall_is_manifest_path(const char *path);
bool waywal_dynamic_manifest_is_path(const char *path);

bool waywal_dynwall_parse_xml_string(const char *xml_text, size_t xml_len, const char *base_dir,
                                     waywal_dynwall_manifest_t *out_manifest);
bool waywal_dynwall_parse_json_string(const char *json_text, size_t json_len, const char *base_dir,
                                      waywal_dynwall_manifest_t *out_manifest);
bool waywal_dynwall_load_file(const char *filepath, waywal_dynwall_manifest_t *out_manifest);
bool waywal_dynamic_manifest_load(const char *filepath, waywal_dynamic_manifest_t *out_manifest);

/* 3. Frame Resolution at Current Time / Solar State */
bool waywal_dynwall_resolve(const waywal_dynwall_manifest_t *manifest, time_t utc_now,
                            const struct tm *local_tm, const waywal_solar_state_t *solar_opt,
                            waywal_dynwall_resolved_t *out_resolved);

bool waywal_dynamic_manifest_eval(const waywal_dynamic_manifest_t *manifest, time_t utc_now,
                                  const waywal_solar_state_t *solar_opt,
                                  waywal_dynwall_resolved_t *out_resolved);

static inline const char *waywal_dynwall_effective_path(const waywal_dynwall_resolved_t *res)
{
    if (!res)
        return "";
    if (res->is_transition && res->blend_factor >= 0.5f && res->to_path[0] != '\0')
        return res->to_path;
    return res->primary_path;
}

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_SOLAR_SCHEDULE_H */
