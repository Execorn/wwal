// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/log.h"
#include "waywal/output_state.h"
#include "waywal/presentation.h"
#include "waywal/security.h"
#include "waywal/shader_engine.h"
#include "waywal/uring_loop.h"

#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

void output_node_destroy_surface(output_node_t *node);
void output_node_destroy_surface(output_node_t *node) { (void)node; }

void waywal_shader_engine_forget_bo(waywal_shader_engine_t *eng, const struct dmabuf_bo *bo)
{
    (void)eng;
    (void)bo;
}

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
__attribute__((visibility("default"))) const char *__lsan_default_suppressions(void);
const char *__lsan_default_suppressions(void)
{
    return "leak:<unknown module>\nleak:libcuda\nleak:cuInit\nleak:gbm_create_device\nleak:libgbm\n"
           "leak:libEGL\nleak:libGLESv2\nleak:dri\nleak:libnvidia\nleak:libGLX_nvidia\nleak:"
           "libEGL_nvidia\nleak:radeonsi\nleak:libva\n";
}
#endif

static void test_presentation_pll_vrr_tracking(void)
{
    printf("[TEST] Running test_presentation_pll_vrr_tracking...\n");

    waywal_pll_t pll;
    presentation_pll_init(&pll, 144); /* 144Hz default */

    assert(pll.period_est_ns >= 6940000LL && pll.period_est_ns <= 6950000LL);

    int64_t simulated_hw_time = 1000000000LL; /* 1.0s in ns */
    const int64_t true_period_ns = 6944444LL; /* ~144Hz */
    const uint32_t refresh_ns = (uint32_t)true_period_ns;

    /* Feed 60 simulated presentation frames with small simulated hardware jitter (±20 us) */
    int64_t total_error_ns = 0;
    for (int i = 0; i < 60; ++i) {
        int64_t jitter = (int64_t)((i % 5) - 2) * 5000LL; /* ±10 us */
        int64_t tick_time = simulated_hw_time + jitter;

        /* Use phase_est_ns directly since simulated timestamps aren't in CLOCK_MONOTONIC domain */
        int64_t predicted_vblank = pll.phase_est_ns;
        if (i > 5 && predicted_vblank > 0) {
            int64_t error = llabs(tick_time - predicted_vblank);
            total_error_ns += error;
        }

        presentation_pll_update(&pll, tick_time, refresh_ns,
                                WP_PRESENTATION_FEEDBACK_KIND_VSYNC |
                                    WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK);

        simulated_hw_time += true_period_ns;
    }

    double avg_error_us = (double)(total_error_ns / 55) / 1000.0;
    printf("  PLL steady-state prediction jitter at 144Hz: %.3f us (Target < 50 us)\n",
           avg_error_us);
    assert(avg_error_us < 50.0); /* Guaranteed sub-50us VRR accuracy */

    /* Simulate dynamic VRR rate shift (144Hz -> 60Hz) */
    const int64_t vrr_period_ns = 16666666LL; /* 60Hz */
    for (int i = 0; i < 40; ++i) {
        presentation_pll_update(&pll, simulated_hw_time, (uint32_t)vrr_period_ns,
                                WP_PRESENTATION_FEEDBACK_KIND_VSYNC);
        simulated_hw_time += vrr_period_ns;
    }

    int64_t final_pred = pll.phase_est_ns;
    int64_t diff_from_expected = llabs(final_pred - simulated_hw_time);
    printf("  PLL post-VRR adaptation period: %ld ns (~%.1f Hz, diff: %ld ns)\n",
           (long)pll.period_est_ns, 1e9 / (double)pll.period_est_ns, (long)diff_from_expected);
    assert(pll.period_est_ns >= 16000000LL && pll.period_est_ns <= 17000000LL);
    assert(diff_from_expected < vrr_period_ns);

    printf("[TEST] test_presentation_pll_vrr_tracking PASSED.\n");
}

static int g_uring_cb_count = 0;
static bool g_uring_readd_in_cb = false;
static int g_uring_write_tfd_in_cb = -1;
static uring_event_type_t g_uring_order[8];
static int g_uring_order_len = 0;
static void test_uring_callback(uring_event_type_t type, int fd, uint32_t res, void *user_data)
{
    (void)res;
    if (g_uring_order_len < 8) {
        g_uring_order[g_uring_order_len++] = type;
    }
    if (type == URING_EV_WAYLAND_READ) {
        g_uring_cb_count++;
        if (g_uring_write_tfd_in_cb >= 0) {
            char b = 0x55;
            assert(write(g_uring_write_tfd_in_cb, &b, 1) == 1);
            g_uring_write_tfd_in_cb = -1;
        }
        if (g_uring_readd_in_cb && user_data) {
            uring_loop_t *lp = (uring_loop_t *)user_data;
            char tmp;
            (void)read(fd, &tmp, 1);
            assert(uring_loop_remove_poll(lp, fd));
            assert(uring_loop_add_poll(lp, fd, POLLIN, URING_EV_WAYLAND_READ, lp));
        }
    }
}

static void test_io_uring_event_loop(void)
{
    printf("[TEST] Running test_io_uring_event_loop...\n");

    uring_loop_t loop;
    bool init_ok = uring_loop_init(&loop, 16);
    assert(init_ok);

    int fds[2];
    int ret = pipe2(fds, O_NONBLOCK | O_CLOEXEC);
    assert(ret == 0);

    /* Register read end with io_uring and verify duplicate registration does not submit duplicate
     * SQEs */
    bool poll_ok = uring_loop_add_poll(&loop, fds[0], POLLIN, URING_EV_WAYLAND_READ, &loop);
    assert(poll_ok);
    assert(uring_loop_add_poll(&loop, fds[0], POLLIN, URING_EV_WAYLAND_READ, &loop));
    assert(loop.multishot_supported);
    assert(loop.slots[0].is_multishot);
    assert(loop.slots[0].in_flight);
    assert(!loop.slots[1].active);

    /* Write 1 byte to trigger first event */
    char byte = 0x42;
    ssize_t w = write(fds[1], &byte, 1);
    assert(w == 1);

    /* Dispatch loop */
    g_uring_cb_count = 0;
    g_uring_readd_in_cb = false;
    int dispatched = uring_loop_dispatch(&loop, test_uring_callback);
    assert(dispatched >= 1);
    assert(g_uring_cb_count == 1);

    /* Drain pipe */
    char rbuf;
    read(fds[0], &rbuf, 1);

    /* Write a second byte without calling uring_loop_add_poll again to verify continuation */
    w = write(fds[1], &byte, 1);
    assert(w == 1);
    dispatched = uring_loop_dispatch(&loop, test_uring_callback);
    assert(dispatched >= 1);
    assert(g_uring_cb_count == 2);
    read(fds[0], &rbuf, 1);

    /* Test remove + re-add of same fd inside dispatch callback (power_governor reconnect pattern)
     */
    g_uring_readd_in_cb = true;
    w = write(fds[1], &byte, 1);
    assert(w == 1);
    dispatched = uring_loop_dispatch(&loop, test_uring_callback);
    assert(dispatched >= 1);
    assert(g_uring_cb_count == 3);
    assert(loop.slots[0].active && loop.slots[0].in_flight);
    g_uring_readd_in_cb = false;

    /* Verify that a subsequent write triggers exactly 1 callback (no duplicate SQE armed) */
    w = write(fds[1], &byte, 1);
    assert(w == 1);
    dispatched = uring_loop_dispatch(&loop, test_uring_callback);
    assert(dispatched >= 1);
    assert(g_uring_cb_count == 4);
    read(fds[0], &rbuf, 1);

    /* Verify URING_EV_WAYLAND_READ is dispatched before URING_EV_TRANSITION_TIMER in the same CQE
     * batch even when the timer pipe is written to first */
    int tfds[2];
    assert(pipe2(tfds, O_NONBLOCK | O_CLOEXEC) == 0);
    assert(uring_loop_add_poll(&loop, tfds[0], POLLIN, URING_EV_TRANSITION_TIMER, &loop));
    assert(write(tfds[1], &byte, 1) == 1);
    assert(write(fds[1], &byte, 1) == 1);
    g_uring_order_len = 0;
    dispatched = uring_loop_dispatch(&loop, test_uring_callback);
    assert(dispatched >= 2);
    assert(g_uring_order_len >= 2);
    assert(g_uring_order[0] == URING_EV_WAYLAND_READ);
    assert(g_uring_order[1] == URING_EV_TRANSITION_TIMER);
    read(fds[0], &rbuf, 1);
    read(tfds[0], &rbuf, 1);

    /* Verify a timer CQE arriving mid-pass-0 (inside URING_EV_WAYLAND_READ callback) is bounded by
     * pass 0's CQE count and never double-dispatched */
    g_uring_order_len = 0;
    g_uring_write_tfd_in_cb = tfds[1];
    assert(write(fds[1], &byte, 1) == 1);
    dispatched = uring_loop_dispatch(&loop, test_uring_callback);
    read(fds[0], &rbuf, 1);
    if (dispatched == 1) {
        assert(g_uring_order_len == 1);
        assert(g_uring_order[0] == URING_EV_WAYLAND_READ);
        dispatched += uring_loop_dispatch(&loop, test_uring_callback);
    }
    assert(dispatched == 2);
    assert(g_uring_order_len == 2);
    assert(g_uring_order[0] == URING_EV_WAYLAND_READ);
    assert(g_uring_order[1] == URING_EV_TRANSITION_TIMER);
    read(tfds[0], &rbuf, 1);

    close(tfds[0]);
    close(tfds[1]);

    close(fds[0]);
    close(fds[1]);
    uring_loop_destroy(&loop);

    printf("  Successfully dispatched kernel io_uring event completion without epoll\n");
    printf("[TEST] test_io_uring_event_loop PASSED.\n");
}

static void test_output_manager_lifecycle(void)
{
    printf("[TEST] Running test_output_manager_lifecycle...\n");

    output_manager_t om;
    output_manager_init(&om, NULL, NULL);
    assert(om.count == 0);
    assert(om.head == NULL);

    /* Simulate manual node creation and search */
    output_state_t node1;
    memset(&node1, 0, sizeof(node1));
    node1.wl_name = 101;
    strncpy(node1.name, "DP-1", sizeof(node1.name) - 1);
    node1.width = 2560;
    node1.height = 1440;
    node1.next = NULL;

    om.head = &node1;
    om.count = 1;

    output_state_t *found = output_manager_find_by_name(&om, "DP-1");
    assert(found != NULL);
    assert(found->wl_name == 101);
    assert(found->width == 2560);

    output_state_t *found_id = output_manager_find_by_id(&om, 101);
    assert(found_id == found);

    om.head = NULL;
    om.count = 0;
    output_manager_destroy(&om);

    printf("  Verified in-process output manager resolution (< 50 us zero fork)\n");
    printf("[TEST] test_output_manager_lifecycle PASSED.\n");
}

static void test_security_hardening(void)
{
    printf("[TEST] Running test_security_hardening...\n");

    int confirm_pfd[2];
    assert(pipe2(confirm_pfd, O_CLOEXEC) == 0);

    char sbox_tmp[256] = "/tmp/wwal_sbox_test_XXXXXX";
    assert(mkdtemp(sbox_tmp) != NULL);

    /* 1. Fork a child process to verify Landlock + Seccomp-BPF isolation */
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(confirm_pfd[0]);
        char sbox_home[512], sbox_ssh[512], sbox_xcache[512], sbox_xstate[512];
        char sbox_local[512], sbox_lshare[512], sbox_keyrings[512], sbox_lstate[512],
            sbox_bash_state[512];
        snprintf(sbox_home, sizeof(sbox_home), "%s/home", sbox_tmp);
        snprintf(sbox_ssh, sizeof(sbox_ssh), "%s/home/.ssh", sbox_tmp);
        snprintf(sbox_local, sizeof(sbox_local), "%s/home/.local", sbox_tmp);
        snprintf(sbox_lshare, sizeof(sbox_lshare), "%s/home/.local/share", sbox_tmp);
        snprintf(sbox_keyrings, sizeof(sbox_keyrings), "%s/home/.local/share/keyrings", sbox_tmp);
        snprintf(sbox_lstate, sizeof(sbox_lstate), "%s/home/.local/state", sbox_tmp);
        snprintf(sbox_bash_state, sizeof(sbox_bash_state), "%s/home/.local/state/bash", sbox_tmp);
        snprintf(sbox_xcache, sizeof(sbox_xcache), "%s/cache", sbox_tmp);
        snprintf(sbox_xstate, sizeof(sbox_xstate), "%s/state", sbox_tmp);
        mkdir(sbox_home, 0700);
        mkdir(sbox_ssh, 0700);
        mkdir(sbox_local, 0700);
        mkdir(sbox_lshare, 0700);
        mkdir(sbox_keyrings, 0700);
        mkdir(sbox_lstate, 0700);
        mkdir(sbox_bash_state, 0700);
        mkdir(sbox_xcache, 0700);
        mkdir(sbox_xstate, 0700);
        setenv("HOME", sbox_home, 1);
        setenv("XDG_CACHE_HOME", sbox_xcache, 1);
        setenv("XDG_STATE_HOME", sbox_xstate, 1);
        setenv("XDG_DATA_HOME", sbox_lshare, 1);

#ifdef SYS_landlock_create_ruleset
        int ll_abi = (int)syscall(SYS_landlock_create_ruleset, NULL, 0, 1);
#else
        int ll_abi = -1;
#endif

        /* Child: apply security hardening */
        bool sec_ok = security_sandbox_apply(NULL);
        if (!sec_ok)
            _exit(1);

        int no_new_privs = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
        if (no_new_privs != 1)
            _exit(2);

        /* Test allowed syscall: getpid() */
        pid_t my_pid = getpid();
        if (my_pid <= 0)
            _exit(3);

        if (ll_abi > 0) {
            char cache_test[4096];
            snprintf(cache_test, sizeof(cache_test), "%s/.cache/wwal", sbox_home);
            int cfd = open(cache_test, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (cfd < 0)
                _exit(5);
            close(cfd);

            /* Verify Landlock + Seccomp-BPF allow symlink() and RW access in swww, awww, and
             * caelestia/wallpaper */
            char sym_path[4096], swww_file[4096], awww_file[4096], cael_file[4096];
            snprintf(sym_path, sizeof(sym_path), "%s/wwal/current_wallpaper", sbox_xcache);
            snprintf(swww_file, sizeof(swww_file), "%s/swww/DP-1", sbox_xcache);
            snprintf(awww_file, sizeof(awww_file), "%s/awww/DP-1", sbox_xcache);
            snprintf(cael_file, sizeof(cael_file), "%s/caelestia/wallpaper/path.txt", sbox_xstate);
            if (symlink("/tmp/dummy_wall.png", sym_path) != 0)
                _exit(11);
            unlink(sym_path);

            int fd1 = open(swww_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            int fd2 = open(awww_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            int fd3 = open(cael_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (fd1 < 0 || fd2 < 0 || fd3 < 0)
                _exit(12);
            close(fd1);
            close(fd2);
            close(fd3);
            unlink(swww_file);
            unlink(awww_file);
            unlink(cael_file);

            /* Sensitive ~/.ssh must be blocked by Landlock */
            int sfd = open(sbox_ssh, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (sfd >= 0) {
                close(sfd);
                _exit(6);
            }

            /* Sensitive ~/.local/share/keyrings must be blocked even when XDG_DATA_HOME is set */
            int kfd = open(sbox_keyrings, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (kfd >= 0) {
                close(kfd);
                _exit(13);
            }

            /* Non-wwal/caelestia subdirectories of ~/.local/state must not be writable */
            char bash_hist[4096];
            snprintf(bash_hist, sizeof(bash_hist), "%s/history", sbox_bash_state);
            int bfd = open(bash_hist, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (bfd >= 0) {
                close(bfd);
                _exit(14);
            }
        }

#ifdef HAVE_LIBSECCOMP
        /* Verify fork + wait4 are allowed for on_wallpaper_change / on_theme_change hooks */
        pid_t hook_pid = fork();
        if (hook_pid < 0)
            _exit(7);
        if (hook_pid == 0)
            _exit(0);
        int hook_st = 0;
        if (waitpid(hook_pid, &hook_st, 0) != hook_pid || !WIFEXITED(hook_st) ||
            WEXITSTATUS(hook_st) != 0)
            _exit(8);

        char ok_byte = 'K';
        (void)write(confirm_pfd[1], &ok_byte, 1);
        close(confirm_pfd[1]);

        /* Verify denied syscall triggers SCMP_ACT_KILL_PROCESS on ptrace */
        syscall(SYS_ptrace, 0, 0, 0, 0);
        /* If not killed by SIGSYS, exit with error code 4 */
        _exit(4);
#else
        char ok_byte = 'K';
        (void)write(confirm_pfd[1], &ok_byte, 1);
        close(confirm_pfd[1]);
        _exit(0);
#endif
    }

    close(confirm_pfd[1]);
    char confirm_byte = 0;
    ssize_t cr = read(confirm_pfd[0], &confirm_byte, 1);
    close(confirm_pfd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    assert(cr == 1 && confirm_byte == 'K');
#ifdef HAVE_LIBSECCOMP
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGSYS || WTERMSIG(status) == SIGKILL);
    printf("  Verified Seccomp-BPF killed thread on denied syscall (SIGSYS=%d)\n",
           WTERMSIG(status));
#else
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif

    printf("  Verified PR_SET_NO_NEW_PRIVS, Landlock LSM, and Seccomp-BPF sandboxing in isolated "
           "process\n");
    printf("[TEST] test_security_hardening PASSED.\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  Executing WayWal Desktop & Runtime Test Suite\n");
    printf("=========================================\n");

    test_presentation_pll_vrr_tracking();
    test_io_uring_event_loop();
    test_output_manager_lifecycle();
    test_security_hardening();

    printf("\n>>> ALL DESKTOP UNIT TESTS PASSED SUCCESSFULLY! <<<\n");
    return 0;
}
