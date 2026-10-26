// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli_parser.h"
#include "waywal/bezier.h"
#include "waywal/ipc_proto.h"

#if __has_include("waywal/toml_parser.h")
#include "waywal/toml_parser.h"
#define HAVE_WAYWAL_TOML_PARSER 1
#endif

#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef WWAL_FUZZ_VERBOSE
static void silence_stdio_once(void)
{
    static bool silenced = false;
    if (silenced)
        return;
    silenced = true;
    /* Redirect stdout/stderr during fuzzing so cli_parse error messages do not bottleneck exec/s.
     */
    int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd >= 0) {
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);
    }
}
#endif

static void fuzz_cli_tokens(const uint8_t *data, size_t size)
{
    if (size == 0 || size > 4096)
        return;

    char buf[4097];
    memcpy(buf, data, size);
    buf[size] = '\0';

    for (size_t i = 0; i < size; ++i) {
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == ' ' || buf[i] == '\t') {
            buf[i] = '\0';
        }
    }

    char *argv[32];
    int argc = 0;
    static char prog_wwal[] = "wwal";
    static char prog_swww[] = "swww";
    argv[argc++] = (data[0] & 1) ? prog_wwal : prog_swww;

    size_t pos = 0;
    while (pos < size && argc < 30) {
        while (pos < size && buf[pos] == '\0')
            pos++;
        if (pos >= size)
            break;
        char *tok = &buf[pos];
        size_t tok_len = strlen(tok);
        if (strcmp(tok, "-h") != 0 && strcmp(tok, "--help") != 0 && strcmp(tok, "help") != 0 &&
            strcmp(tok, "--transition-shader") != 0) {
            argv[argc++] = tok;
        }
        pos += tok_len + 1;
    }
    argv[argc] = NULL;

    cli_options_t opts;
    memset(&opts, 0, sizeof(opts));
    cli_parse(argc, argv, &opts);
    if (opts.custom_shader_src) {
        free(opts.custom_shader_src);
    }
}

static void fuzz_bezier_and_ipc(const uint8_t *data, size_t size)
{
    if (size >= 20) {
        float vals[5];
        memcpy(vals, data, sizeof(vals));
        for (int i = 0; i < 5; ++i) {
            if (!isfinite(vals[i]))
                vals[i] = 0.5f;
        }
        bezier_curve_t curve = bezier_create(vals[0], vals[1], vals[2], vals[3]);
        volatile float y = bezier_eval(&curve, vals[4]);
        (void)y;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data || size > 65536)
        return 0;

#ifndef WWAL_FUZZ_VERBOSE
    silence_stdio_once();
#endif

#ifdef HAVE_WAYWAL_TOML_PARSER
    char *toml_str = (char *)malloc(size + 1);
    if (toml_str) {
        memcpy(toml_str, data, size);
        toml_str[size] = '\0';
        waywal_config_t cfg;
        char err_buf[256];
        waywal_config_init_defaults(&cfg);
        waywal_config_parse_string(toml_str, &cfg, err_buf, sizeof(err_buf));
        free(toml_str);
    }
#endif

    fuzz_cli_tokens(data, size);
    fuzz_bezier_and_ipc(data, size);
    return 0;
}

#ifdef WWAL_FUZZ_STANDALONE
static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            FILE *fp = fopen(argv[i], "rb");
            if (!fp)
                continue;
            fseek(fp, 0, SEEK_END);
            long len = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (len >= 0 && len <= 65536) {
                uint8_t *buf = (uint8_t *)malloc((size_t)len + 1);
                if (buf) {
                    size_t n = fread(buf, 1, (size_t)len, fp);
                    LLVMFuzzerTestOneInput(buf, n);
                    free(buf);
                }
            }
            fclose(fp);
        }
        return 0;
    }

    int saved_stdout = dup(STDOUT_FILENO);

    static const char *corpus[] = {
        "[daemon]\nlayer = \"background\"\nbattery_policy = \"pause-video\"\n10bit = true\n"
        "[theme]\nenable = true\nmode = \"dark\"\ncaelestia_sync = true\n"
        "[transition]\ntype = \"grow\"\nduration = 0.8\nfps = 144\nbezier = [0.25, 0.1, 0.25, "
        "1.0]\n"
        "[output.\"DP-1\"]\npath = \"/tmp/wall.png\"\nscaling = \"fill\"\n"
        "[workspace.\"1\"]\npath = \"/tmp/ws1.png\"\noutput = \"DP-1\"\n",
        "img /tmp/test.png --transition-type grow --transition-pos cursor --transition-fps 144 "
        "--transition-bezier 0.22,1.0,0.36,1.0 --fill-color #1e1e2e --resize fit --10bit",
        "video /tmp/loop.mp4 --loop 0 --speed 1.5 -o DP-1,HDMI-A-1",
        "slideshow start /tmp --interval 120 --shuffle --transition-type wave --transition-wave "
        "30,0.08",
        "clear #ff00ff -o eDP-1",
        "unterminated = \"string literal\n[broken.section\nbezier = [1, 2, 3, 4, 5, 6]",
    };

    for (size_t i = 0; i < sizeof(corpus) / sizeof(corpus[0]); ++i) {
        const uint8_t *ptr = (const uint8_t *)corpus[i];
        size_t len = strlen(corpus[i]);
        LLVMFuzzerTestOneInput(ptr, len);

        uint32_t rng = 0xA5A5F00DU ^ (uint32_t)i;
        uint8_t mutated[1024];
        if (len < sizeof(mutated)) {
            for (int iter = 0; iter < 128; ++iter) {
                memcpy(mutated, ptr, len);
                uint32_t flips = (xorshift32(&rng) % 4U) + 1U;
                for (uint32_t f = 0; f < flips; ++f) {
                    size_t pos = xorshift32(&rng) % len;
                    mutated[pos] ^= (uint8_t)(xorshift32(&rng) & 0x7FU);
                }
                size_t sub_len = (xorshift32(&rng) % len) + 1U;
                LLVMFuzzerTestOneInput(mutated, sub_len);
            }
        }
    }

    if (saved_stdout >= 0) {
        dprintf(saved_stdout, "[FUZZ] fuzz_config_cli standalone corpus smoke test PASSED.\n");
        close(saved_stdout);
    }
    return 0;
}
#endif
