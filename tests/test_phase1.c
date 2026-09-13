#include "waywal/arena.h"
#include "waywal/ipc_proto.h"
#include "waywal/log.h"
#include "waywal/os_compat.h"
#include "waywal/path.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

static void test_arena(void)
{
    printf("[TEST] Running test_arena...\n");

    arena_t arena;
    bool ok = arena_init(&arena, 4 * 1024 * 1024); /* 4 MB */
    assert(ok && "arena_init failed");
    assert(arena.capacity >= 4 * 1024 * 1024);
    assert(arena.offset == 0);

    /* Test 10,000 allocations of varying sizes and alignments */
    uint8_t *ptrs[10000];
    size_t sizes[10000];

    for (size_t i = 0; i < 10000; ++i) {
        size_t sz = (i % 64) + 1;
        size_t align = 1 << (i % 6); /* 1, 2, 4, 8, 16, 32 */
        sizes[i] = sz;

        ptrs[i] = (uint8_t *)arena_alloc(&arena, sz, align);
        assert(ptrs[i] != NULL && "allocation must succeed within capacity");
        assert(((uintptr_t)ptrs[i] & (align - 1)) == 0 && "allocation must be aligned");

        /* Fill memory to ensure writeability and test for overlapping */
        memset(ptrs[i], (int)(i & 0xFF), sz);
    }

    /* Verify no overlap */
    for (size_t i = 0; i < 10000; ++i) {
        for (size_t b = 0; b < sizes[i]; ++b) {
            assert(ptrs[i][b] == (uint8_t)(i & 0xFF) && "data corrupted by subsequent allocations");
        }
    }

    /* Test zero allocation */
    uint32_t *zeros = (uint32_t *)arena_alloc_zero(&arena, 256 * sizeof(uint32_t), 64);
    assert(zeros != NULL);
    for (size_t i = 0; i < 256; ++i) {
        assert(zeros[i] == 0 && "arena_alloc_zero must initialize all memory to 0");
    }

    /* Test scoped scratchpad */
    size_t before_temp_offset = arena.offset;
    arena_temp_t temp = arena_temp_begin(&arena);
    void *temp_alloc1 = arena_alloc(&arena, 1024, 16);
    void *temp_alloc2 = arena_alloc(&arena, 2048, 16);
    assert(temp_alloc1 != NULL && temp_alloc2 != NULL);
    assert(arena.offset > before_temp_offset);
    arena_temp_end(temp);
    assert(arena.offset == before_temp_offset && "arena_temp_end must rewind offset");

    /* Test arena reset */
    arena_reset(&arena);
    assert(arena.offset == 0 && "arena_reset must set offset to 0");

    /* Reallocate after reset */
    void *post_reset_ptr = arena_alloc(&arena, 128, 16);
    assert(post_reset_ptr != NULL);
    assert(arena.offset >= 128);

    arena_destroy(&arena);
    assert(arena.buffer == NULL && arena.capacity == 0);
    printf("[TEST] test_arena PASSED.\n");
}

static void test_path(void)
{
    printf("[TEST] Running test_path...\n");

    path_buf_t pb;
    path_buf_init(&pb);
    assert(pb.len == 0);
    assert(strcmp(path_buf_cstr(&pb), "") == 0);

    /* Test path_buf_from_str */
    path_buf_from_str(&pb, "/tmp/waywal_test");
    assert(pb.len == strlen("/tmp/waywal_test"));
    assert(strcmp(path_buf_cstr(&pb), "/tmp/waywal_test") == 0);

    /* Test path_buf_push */
    bool pushed = path_buf_push(&pb, "subdir");
    assert(pushed);
    assert(strcmp(path_buf_cstr(&pb), "/tmp/waywal_test/subdir") == 0);

    /* Test push without double slash */
    pushed = path_buf_push(&pb, "/file.sock");
    assert(pushed);
    assert(strcmp(path_buf_cstr(&pb), "/tmp/waywal_test/subdir/file.sock") == 0);

    /* Test append suffix */
    bool appended = path_buf_append(&pb, ".lock");
    assert(appended);
    assert(strcmp(path_buf_cstr(&pb), "/tmp/waywal_test/subdir/file.sock.lock") == 0);

    /* Test path_buf_parent */
    bool parent_ok = path_buf_parent(&pb);
    assert(parent_ok);
    assert(strcmp(path_buf_cstr(&pb), "/tmp/waywal_test/subdir") == 0);

    parent_ok = path_buf_parent(&pb);
    assert(parent_ok);
    assert(strcmp(path_buf_cstr(&pb), "/tmp/waywal_test") == 0);

    parent_ok = path_buf_parent(&pb);
    assert(parent_ok);
    assert(strcmp(path_buf_cstr(&pb), "/tmp") == 0);

    parent_ok = path_buf_parent(&pb);
    assert(parent_ok);
    assert(strcmp(path_buf_cstr(&pb), "/") == 0);

    /* Root "/" parent should return false */
    parent_ok = path_buf_parent(&pb);
    assert(!parent_ok && "Root / has no parent");

    /* Test relative path parent without underflow */
    path_buf_from_str(&pb, "relative_dir");
    parent_ok = path_buf_parent(&pb);
    assert(parent_ok);
    assert(pb.len == 0 && "Parent of relative single item is empty string");

    /* Empty path parent test (must not underflow) */
    parent_ok = path_buf_parent(&pb);
    assert(!parent_ok && "Empty path parent returns false without underflow");

    /* Trailing slashes test */
    path_buf_from_str(&pb, "/home/user/dir///");
    parent_ok = path_buf_parent(&pb);
    assert(parent_ok);
    assert(strcmp(path_buf_cstr(&pb), "/home/user") == 0);

    /* Overflow protection test */
    char huge[WAYWAL_PATH_MAX + 10];
    memset(huge, 'a', sizeof(huge));
    huge[sizeof(huge) - 1] = '\0';
    path_buf_from_str(&pb, "/base");
    bool overflow = path_buf_push(&pb, huge);
    assert(!overflow && "Oversized component must be rejected");

    printf("[TEST] test_path PASSED.\n");
}

static void test_ipc_roundtrip(void)
{
    printf("[TEST] Running test_ipc_roundtrip...\n");

    /* Verify fixed 16-byte header size */
    static_assert(sizeof(waywal_ipc_hdr_t) == 16, "waywal_ipc_hdr_t MUST be exactly 16 bytes");
    assert(sizeof(waywal_ipc_hdr_t) == 16);

    int sv[2];
    int res = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv);
    assert(res == 0 && "socketpair failed");

    /* Create test memfd */
    int memfd = waywal_create_memfd("test-memfd-ipc", 4096, 0);
    assert(memfd >= 0 && "waywal_create_memfd failed");

    uint32_t test_val = 0xDEADBEEF;
    ssize_t w = write(memfd, &test_val, sizeof(test_val));
    assert(w == sizeof(test_val));

    /* Send IPC message with SCM_RIGHTS */
    waywal_ipc_hdr_t send_hdr = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_REQ_SET_IMAGE,
        .payload_size = 4096,
    };

    bool sent = waywal_ipc_send(sv[0], &send_hdr, memfd);
    assert(sent && "waywal_ipc_send failed");

    /* Receive IPC message */
    waywal_ipc_hdr_t recv_hdr;
    int received_fd = -1;
    bool recvd = waywal_ipc_recv(sv[1], &recv_hdr, &received_fd);
    assert(recvd && "waywal_ipc_recv failed");

    assert(recv_hdr.magic == WAYWAL_IPC_MAGIC);
    assert(recv_hdr.version == WAYWAL_IPC_VERSION);
    assert(recv_hdr.opcode == WAYWAL_REQ_SET_IMAGE);
    assert(recv_hdr.payload_size == 4096);
    assert(received_fd >= 0 && "Must receive attached file descriptor");

    /* Read back content from transferred descriptor */
    lseek(received_fd, 0, SEEK_SET);
    uint32_t read_back = 0;
    ssize_t r = read(received_fd, &read_back, sizeof(read_back));
    assert(r == sizeof(read_back));
    assert(read_back == 0xDEADBEEF && "Payload contents in transferred descriptor must match");

    close(memfd);
    close(received_fd);
    close(sv[0]);
    close(sv[1]);

    printf("[TEST] test_ipc_roundtrip PASSED.\n");
}

int main(void)
{
    waywal_log_set_level(WAYWAL_LOG_LEVEL_WARN);
    printf("=========================================\n");
    printf("  Executing WayWal Phase 1 Test Suite\n");
    printf("=========================================\n");

    test_arena();
    test_path();
    test_ipc_roundtrip();

    printf("\n>>> ALL PHASE 1 UNIT TESTS PASSED SUCCESSFULLY! <<<\n\n");
    return 0;
}
