#include "waywal/presentation.h"
#include "waywal/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void presentation_pll_init(waywal_pll_t *pll, uint32_t default_refresh_hz) {
    if (!pll) return;
    memset(pll, 0, sizeof(*pll));
    pll->alpha = 0.15f;
    pll->beta = 0.005f;

    uint32_t hz = (default_refresh_hz > 0) ? default_refresh_hz : 60;
    pll->period_est_ns = 1000000000LL / (int64_t)hz;
    pll->phase_est_ns = 0;
    pll->last_hardware_time = 0;
    pll->frame_seq = 0;
    pll->jitter_ns = 0;
}

void presentation_pll_update(waywal_pll_t *pll, int64_t hw_time_ns, uint32_t refresh_ns, uint32_t flags) {
    if (!pll || hw_time_ns <= 0) return;

    if (pll->phase_est_ns == 0) {
        /* First hardware sample: seed state */
        if (refresh_ns > 0) {
            pll->period_est_ns = (int64_t)refresh_ns;
        }
        pll->phase_est_ns = hw_time_ns + pll->period_est_ns;
        pll->last_hardware_time = hw_time_ns;
        pll->jitter_ns = 0;
        return;
    }

    /* Check if display mode shifted significantly via refresh_ns */
    if (refresh_ns > 0 && llabs((int64_t)refresh_ns - pll->period_est_ns) > (pll->period_est_ns / 8)) {
        pll->period_est_ns = (int64_t)refresh_ns;
        pll->phase_est_ns = hw_time_ns + pll->period_est_ns;
        pll->last_hardware_time = hw_time_ns;
        pll->jitter_ns = 0;
        return;
    }

    /* In VRR (refresh_ns == 0 or variable), detect macro frame rate shift (>25%) */
    if (pll->last_hardware_time > 0) {
        int64_t delta = hw_time_ns - pll->last_hardware_time;
        if (delta >= 1000000LL && delta <= 100000000LL) {
            if (llabs(delta - pll->period_est_ns) > (pll->period_est_ns / 4)) {
                pll->period_est_ns = delta;
                pll->phase_est_ns = hw_time_ns + pll->period_est_ns;
                pll->last_hardware_time = hw_time_ns;
                pll->jitter_ns = 0;
                return;
            }
        }
    }

    /* Innovation (prediction error) */
    int64_t error_ns = hw_time_ns - pll->phase_est_ns;
    pll->jitter_ns = llabs(error_ns);

    /* Update phase with proportional gain alpha */
    pll->phase_est_ns += (int64_t)((float)error_ns * pll->alpha);

    /* Update period with integral/frequency gain beta */
    pll->period_est_ns += (int64_t)((float)error_ns * pll->beta);

    /* Clamp estimated period: 1ms (1000Hz) to 100ms (10Hz) */
    if (pll->period_est_ns < 1000000LL) pll->period_est_ns = 1000000LL;
    if (pll->period_est_ns > 100000000LL) pll->period_est_ns = 100000000LL;

    /* Project next VBlank */
    pll->phase_est_ns += pll->period_est_ns;
    pll->last_hardware_time = hw_time_ns;
    pll->frame_seq++;

    pll->has_hw_clock = (flags & WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK) != 0;
    pll->is_zero_copy = (flags & WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY) != 0;
}

int64_t presentation_pll_predict_next_vblank(const waywal_pll_t *pll) {
    if (!pll) return 0;
    return pll->phase_est_ns;
}

static void feedback_sync_output(void *data, struct wp_presentation_feedback *feedback, struct wl_output *output) {
    (void)data; (void)feedback; (void)output;
}

static void feedback_presented(void *data,
                               struct wp_presentation_feedback *feedback,
                               uint32_t tv_sec_hi,
                               uint32_t tv_sec_lo,
                               uint32_t tv_nsec,
                               uint32_t refresh_ns,
                               uint32_t seq_hi,
                               uint32_t seq_lo,
                               uint32_t flags) {
    (void)seq_hi; (void)seq_lo;
    presentation_sync_t *ps = (presentation_sync_t *)data;
    if (ps) {
        int64_t hw_time_ns = (((int64_t)tv_sec_hi << 32) | (int64_t)tv_sec_lo) * 1000000000LL + (int64_t)tv_nsec;
        presentation_pll_update(&ps->pll, hw_time_ns, refresh_ns, flags);
        ps->pending_feedback = false;
    }
    wp_presentation_feedback_destroy(feedback);
}

static void feedback_discarded(void *data, struct wp_presentation_feedback *feedback) {
    presentation_sync_t *ps = (presentation_sync_t *)data;
    if (ps) {
        ps->pending_feedback = false;
    }
    wp_presentation_feedback_destroy(feedback);
}

static const struct wp_presentation_feedback_listener g_feedback_listener = {
    .sync_output = feedback_sync_output,
    .presented   = feedback_presented,
    .discarded   = feedback_discarded,
};

bool presentation_sync_init(presentation_sync_t *ps, struct wp_presentation *pres, struct wl_surface *surface) {
    if (!ps || !pres || !surface) return false;
    memset(ps, 0, sizeof(*ps));
    ps->wp_pres = pres;
    ps->surface = surface;
    presentation_pll_init(&ps->pll, 60);
    return true;
}

void presentation_sync_request(presentation_sync_t *ps) {
    if (!ps || !ps->wp_pres || !ps->surface || ps->pending_feedback) return;

    ps->feedback = wp_presentation_feedback(ps->wp_pres, ps->surface);
    if (ps->feedback) {
        wp_presentation_feedback_add_listener(ps->feedback, &g_feedback_listener, ps);
        ps->pending_feedback = true;
    }
}

void presentation_sync_destroy(presentation_sync_t *ps) {
    if (!ps) return;
    if (ps->feedback && ps->pending_feedback) {
        wp_presentation_feedback_destroy(ps->feedback);
        ps->feedback = NULL;
        ps->pending_feedback = false;
    }
    ps->wp_pres = NULL;
    ps->surface = NULL;
}
