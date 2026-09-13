#ifndef WAYWAL_PRESENTATION_H
#define WAYWAL_PRESENTATION_H

#include "presentation-time-client-protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* PLL state variables */
    int64_t phase_est_ns;       /* Estimated time of next VBlank in CLOCK_MONOTONIC ns */
    int64_t period_est_ns;      /* Estimated monitor refresh cycle in ns */
    int64_t last_hardware_time; /* Actual timestamp returned by display hardware */
    uint64_t frame_seq;

    /* Alpha-beta filter tuning coefficients */
    float alpha; /* Phase correction gain (default: 0.15) */
    float beta;  /* Frequency/period drift gain (default: 0.005) */

    /* Diagnostics & telemetry */
    bool has_hw_clock;
    bool is_zero_copy;
    int64_t jitter_ns;
} waywal_pll_t;

typedef struct {
    struct wp_presentation *wp_pres;
    struct wp_presentation_feedback *feedback;
    waywal_pll_t pll;
    struct wl_surface *surface;
    bool pending_feedback;
} presentation_sync_t;

void presentation_pll_init(waywal_pll_t *pll, uint32_t default_refresh_hz);
void presentation_pll_update(waywal_pll_t *pll, int64_t hw_time_ns, uint32_t refresh_ns,
                             uint32_t flags);
int64_t presentation_pll_predict_next_vblank(const waywal_pll_t *pll);

bool presentation_sync_init(presentation_sync_t *ps, struct wp_presentation *pres,
                            struct wl_surface *surface);
void presentation_sync_request(presentation_sync_t *ps);
void presentation_sync_destroy(presentation_sync_t *ps);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_PRESENTATION_H */
