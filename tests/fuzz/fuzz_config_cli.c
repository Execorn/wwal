// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli_parser.h"
#include "fuzz_harness.h"
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
static void silence_stderr_once(void)
{
    static bool silenced = false;
    if (silenced) {
        return;
    }
    silenced = true;
    /* Redirect stderr while fuzzing so cli_parse error messages do not stall exec/s. */
    int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd >= 0) {
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);
    }
}
#endif

static void fuzz_cli_tokens(const uint8_t *data, size_t size)
{
    if (size == 0u) {
        return;
    }

    size_t tok_bytes = size <= 4096u ? size : 4096u;
    char buf[4097];
    memcpy(buf, data, tok_bytes);
    buf[tok_bytes] = '\0';

    for (size_t i = 0u; i < tok_bytes; ++i) {
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == ' ' || buf[i] == '\t') {
            buf[i] = '\0';
        }
    }

    char *argv[64];
    int argc = 0;
    static char prog_wwal[] = "wwal";
    static char prog_swww[] = "swww";
    argv[argc++] = (data[0] & 1u) ? prog_wwal : prog_swww;

    size_t pos = 0u;
    while (pos < tok_bytes && argc < 62) {
        while (pos < tok_bytes && buf[pos] == '\0') {
            pos++;
        }
        if (pos >= tok_bytes) {
            break;
        }
        char *tok = &buf[pos];
        size_t tok_len = strlen(tok);
        argv[argc++] = tok;
        pos += tok_len + 1u;
    }
    argv[argc] = NULL;

    cli_options_t opts;
    memset(&opts, 0, sizeof(opts));
    (void)cli_parse(argc, argv, &opts);
    free(opts.custom_shader_src);
}

static void fuzz_bezier_and_helpers(const uint8_t *data, size_t size)
{
    if (size >= 20u) {
        float vals[5];
        memcpy(vals, data, sizeof(vals));
        for (int i = 0; i < 5; ++i) {
            if (!isfinite(vals[i])) {
                vals[i] = 0.5f;
            }
        }
        bezier_curve_t curve = bezier_create(vals[0], vals[1], vals[2], vals[3]);
        volatile float y = bezier_eval(&curve, vals[4]);
        (void)y;
    }

#ifdef HAVE_WAYWAL_TOML_PARSER
    size_t slice_len = size < 255u ? size : 255u;
    char small[256];
    memcpy(small, data, slice_len);
    small[slice_len] = '\0';
    float gx = 0.0f;
    float gy = 0.0f;
    (void)waywal_parse_crop_gravity(small, &gx, &gy);
    uint32_t dur_sec = 0u;
    (void)waywal_parse_duration_seconds(small, &dur_sec);
    waywal_sort_mode_t sort_m = WAYWAL_SORT_RANDOM;
    (void)waywal_parse_sort_mode(small, &sort_m);
#endif
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data) {
        return 0;
    }

#ifndef WWAL_FUZZ_VERBOSE
    silence_stderr_once();
#endif

#ifdef HAVE_WAYWAL_TOML_PARSER
    size_t toml_len = size <= (256u * 1024u) ? size : (256u * 1024u);
    char *toml_str = (char *)malloc(toml_len + 1u);
    waywal_config_t *cfg = (waywal_config_t *)calloc(1u, sizeof(*cfg));
    if (toml_str && cfg) {
        memcpy(toml_str, data, toml_len);
        toml_str[toml_len] = '\0';
        char err_buf[256];
        waywal_config_init_defaults(cfg);
        if (waywal_config_parse_string(toml_str, cfg, err_buf, sizeof(err_buf))) {
            (void)waywal_config_match_output(cfg, "DP-1");
            (void)waywal_config_match_output(cfg, "");
        }
        uint32_t dur_sec = 0u;
        (void)waywal_parse_duration_seconds(toml_str, &dur_sec);
        waywal_sort_mode_t sort_m = WAYWAL_SORT_RANDOM;
        (void)waywal_parse_sort_mode(toml_str, &sort_m);
        float gx = 0.0f;
        float gy = 0.0f;
        (void)waywal_parse_crop_gravity(toml_str, &gx, &gy);
    }
    free(cfg);
    free(toml_str);
#endif

    fuzz_cli_tokens(data, size);
    fuzz_bezier_and_helpers(data, size);
    return 0;
}

#ifdef WWAL_FUZZ_STANDALONE
static void run_builtin_config_cli_corpus(void)
{
    static const char *const corpus[] = {
        "[daemon]\nlayer = \"background\"\nbattery_policy = \"pause-video\"\n10bit = true\n"
        "[theme]\nenable = true\nmode = \"dark\"\ncaelestia_sync = true\n"
        "[transition]\ntype = \"grow\"\nduration = 0.8\nfps = 144\nbezier = [0.25, 0.1, 0.25, "
        "1.0]\n"
        "[output.\"DP-1\"]\npath = \"/tmp/wall.png\"\nscaling = \"fill\"\n"
        "[workspace.\"1\"]\npath = \"/tmp/ws1.png\"\noutput = \"DP-1\"\n",
        "[default]\npath = \"/tmp\"\nduration = \"15m\"\nmode = \"fit-border-color\"\n"
        "sorting = \"mtime\"\ncolor = \"#112233\"\n"
        "[any]\nduration = 1h30m\n"
        "[DP-1]\npath = \"/tmp/shader.frag\"\nmode = \"center\"\n"
        "[workspace.\"2\"]\ncolor = \"#336699\"\ntransition = \"slide\"\n"
        "transition_duration = \"400ms\"\ndirectional = false\n",
        "img /tmp/test.png --transition-type grow --transition-pos cursor --transition-fps 144 "
        "--transition-bezier 0.22,1.0,0.36,1.0 --fill-color #1e1e2e --resize fit --10bit "
        "--crop-gravity top-left",
        "img /tmp/test.png --transition-shader /nonexistent/fuzz.comp --crop-gravity 0.25,0.75",
        "video /tmp/loop.mp4 --loop 0 --speed 1.5 -o DP-1,HDMI-A-1",
        ("slideshow start /tmp --interval 120 --shuffle --transition-type wave --transition-wave "
         "30,0.08"),
        "slideshow --duration 1h30m --sort descending /tmp -o DP-1",
        "--help",
        "-h",
        "help",
        "next -o DP-1",
        "prev --output HDMI-A-1",
        "clear #ff00ff -o eDP-1",
        "color #1e1e2e -o DP-1",
        "resume -o HDMI-A-1",
        "unpause",
        "[transition]\nstagger_delay = 120\n",
        "[transition]\nstagger_delay_ms = 250\n",
        "unterminated = \"string literal\n[broken.section\nbezier = [1, 2, 3, 4, 5, 6]",
    };

    const size_t n = sizeof(corpus) / sizeof(corpus[0]);
    wwal_fuzz_sample_t seeds[sizeof(corpus) / sizeof(corpus[0])];
    for (size_t i = 0u; i < n; ++i) {
        seeds[i].data = (const uint8_t *)corpus[i];
        seeds[i].size = strlen(corpus[i]);
    }

    wwal_fuzz_run_mutations(seeds, n, 0xA5A5F00Du, 2048u, 1024u);

#ifdef HAVE_WAYWAL_TOML_PARSER
    {
        const char *t1 = "[transition]\nstagger_delay = 120\n";
        waywal_config_t *cfg1 = (waywal_config_t *)calloc(1u, sizeof(*cfg1));
        if (!cfg1 || !waywal_config_parse_string(t1, cfg1, NULL, 0u) ||
            cfg1->transition.stagger_delay_ms != 120u) {
            free(cfg1);
            exit(1);
        }
        const char *t2 = "[transition]\nstagger_delay_ms = 250\n";
        waywal_config_t *cfg2 = (waywal_config_t *)calloc(1u, sizeof(*cfg2));
        if (!cfg2 || !waywal_config_parse_string(t2, cfg2, NULL, 0u) ||
            cfg2->transition.stagger_delay_ms != 250u) {
            free(cfg1);
            free(cfg2);
            exit(1);
        }
        free(cfg1);
        free(cfg2);
    }
#endif
    {
        cli_options_t o;
        char *argv_c[] = {"wwal", "color", "#112233"};
        memset(&o, 0, sizeof(o));
        if (!cli_parse(3, argv_c, &o) || o.cmd != CLI_CMD_COLOR) {
            exit(1);
        }
        free(o.custom_shader_src);

        char *argv_r[] = {"wwal", "resume", "-o", "DP-1"};
        memset(&o, 0, sizeof(o));
        if (!cli_parse(4, argv_r, &o) || o.cmd != CLI_CMD_UNPAUSE) {
            exit(1);
        }
        free(o.custom_shader_src);

        char *argv_h[] = {"wwal", "--help"};
        memset(&o, 0, sizeof(o));
        if (!cli_parse(2, argv_h, &o) || o.cmd != CLI_CMD_HELP) {
            exit(1);
        }
        free(o.custom_shader_src);
    }
}

WWAL_FUZZ_HARNESS_MAIN("fuzz_config_cli", NULL, run_builtin_config_cli_corpus, NULL)
#endif
