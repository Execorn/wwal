#ifndef WAYWAL_ANIM_ENGINE_H
#define WAYWAL_ANIM_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAYWAL_ANIM_FORMAT_UNKNOWN = 0,
    WAYWAL_ANIM_FORMAT_GIF     = 1,
    WAYWAL_ANIM_FORMAT_APNG    = 2,
    WAYWAL_ANIM_FORMAT_WEBP    = 3,
} waywal_anim_format_t;

typedef struct waywal_anim_decoder waywal_anim_decoder_t;

bool waywal_anim_probe(const uint8_t *data, size_t size);
waywal_anim_format_t waywal_anim_detect_format(const uint8_t *data, size_t size);

waywal_anim_decoder_t *waywal_anim_create(const uint8_t *data, size_t size);
bool waywal_anim_next_frame(waywal_anim_decoder_t *dec, const uint8_t **out_rgba,
                            uint32_t *out_w, uint32_t *out_h, uint32_t *out_delay_ms);
void waywal_anim_reset(waywal_anim_decoder_t *dec);
uint32_t waywal_anim_frame_count(const waywal_anim_decoder_t *dec);
waywal_anim_format_t waywal_anim_get_format(const waywal_anim_decoder_t *dec);
void waywal_anim_destroy(waywal_anim_decoder_t *dec);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_ANIM_ENGINE_H */
