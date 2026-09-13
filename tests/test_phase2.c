#include "waywal/dmabuf.h"
#include "waywal/log.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
__attribute__((visibility("default"))) const char *__lsan_default_suppressions(void)
{
    return "leak:<unknown module>\nleak:gbm_create_device\nleak:libgbm\n";
}
#endif

static void test_dmabuf_context_and_bo(void)
{
    printf("[TEST] Running test_dmabuf_context_and_bo...\n");

    dmabuf_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    bool ok = dmabuf_context_init(&ctx);
    if (!ok) {
        printf("[TEST] DRM node or GBM not available in this environment. Skipping hardware "
               "scanout test.\n");
        return;
    }

    assert(ctx.drm_fd >= 0);
    assert(ctx.gbm != NULL);
    assert(ctx.available);

    /* Test direct GBM buffer allocation with scanout flags */
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t format = DRM_FORMAT_ARGB8888;

    struct gbm_bo *bo =
        gbm_bo_create(ctx.gbm, width, height, format, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!bo) {
        /* Fallback for drivers that don't support simultaneous scanout + render */
        bo = gbm_bo_create(ctx.gbm, width, height, format, GBM_BO_USE_SCANOUT);
    }
    assert(bo != NULL && "gbm_bo_create must succeed on valid DRM/GBM device");

    uint32_t bo_width = gbm_bo_get_width(bo);
    uint32_t bo_height = gbm_bo_get_height(bo);
    assert(bo_width == width);
    assert(bo_height == height);

    uint32_t stride = gbm_bo_get_stride(bo);
    assert(stride >= width * 4);

    uint64_t mod = gbm_bo_get_modifier(bo);
    printf("  Allocated GBM BO: %ux%u (stride: %u, modifier: 0x%016lx)\n", bo_width, bo_height,
           stride, (unsigned long)mod);

    int dma_fd = gbm_bo_get_fd(bo);
    assert(dma_fd >= 0 && "Must be able to export DMA-BUF file descriptor from BO");
    close(dma_fd);

    /* Test CPU write mapping */
    uint32_t map_stride = 0;
    void *map_data = NULL;
    void *map = gbm_bo_map(bo, 0, 0, width, height, GBM_BO_TRANSFER_WRITE, &map_stride, &map_data);
    if (map && map != MAP_FAILED) {
        memset(map, 0xAA, height * map_stride);
        gbm_bo_unmap(bo, map_data);
    }

    gbm_bo_destroy(bo);

    /* Test double-buffering ring */
    dmabuf_ring_t ring;
    bool ring_ok = dmabuf_ring_init(&ctx, &ring, width, height, format, DRM_FORMAT_MOD_INVALID);
    assert(ring_ok && "dmabuf_ring_init must succeed");
    assert(ring.initialized);

    dmabuf_bo_t *buf0 = dmabuf_ring_acquire(&ring);
    assert(buf0 != NULL && buf0->in_use);

    dmabuf_bo_t *buf1 = dmabuf_ring_acquire(&ring);
    assert(buf1 != NULL && buf1->in_use);
    assert(buf0 != buf1 && "Acquiring twice must cycle through distinct double-buffers");

    dmabuf_ring_destroy(&ctx, &ring);
    assert(!ring.initialized);

    dmabuf_context_destroy(&ctx);
    assert(!ctx.available && ctx.gbm == NULL);

    printf("[TEST] test_dmabuf_context_and_bo PASSED.\n");
}

int main(void)
{
    waywal_log_set_level(WAYWAL_LOG_LEVEL_WARN);
    printf("=========================================\n");
    printf("  Executing WayWal Phase 2 Test Suite\n");
    printf("=========================================\n");

    test_dmabuf_context_and_bo();

    printf("\n>>> ALL PHASE 2 UNIT TESTS PASSED SUCCESSFULLY! <<<\n\n");
    return 0;
}
