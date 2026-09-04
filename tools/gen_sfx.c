/*
 * gen_sfx.c —— PC 端音效预生成工具。
 * 复用 echo_audio.c 的合成代码，把所有音效导出为 raw PCM 文件。
 *
 * 编译：cc -O2 -o gen_sfx gen_sfx.c ../main/echo_audio.c -lm
 * 运行：./gen_sfx  (在 output/ 目录生成 .raw 文件)
 *
 * 每个文件是 16kHz / 16bit / mono 的裸 PCM，可直接 EMBED_FILES 嵌入 flash。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "echo_audio.h"

#define OUT_DIR "sfx_data"

static void write_raw(const char *name, const int16_t *buf, int n) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.raw", OUT_DIR, name);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "无法写入 %s\n", path); exit(1); }
    fwrite(buf, 2, n, f);
    fclose(f);
    printf("  %-20s %6d samples (%5.1f KB)\n", name, n, n * 2 / 1024.0);
}

int main(void) {
    /* 创建输出目录 */
    system("mkdir -p " OUT_DIR);

    int cap = 64000;  /* 足够覆盖 ambience 4秒=64000 采样 */
    int16_t *buf = (int16_t *)malloc(cap * 2);
    if (!buf) { fprintf(stderr, "malloc 失败\n"); return 1; }

    printf("生成音效 (16kHz/16bit/mono) → %s/\n", OUT_DIR);

    /* 固定音效 */
    struct { sfx_t sfx; const char *name; } table[] = {
        { SFX_KNOCK,        "knock" },
        { SFX_KNOCK_SOFT,   "knock_soft" },
        { SFX_ECHO_OPEN,    "echo_open" },
        { SFX_ECHO_BLOCKED, "echo_blocked" },
        { SFX_ECHO_ALIVE,   "echo_alive" },
        { SFX_ECHO_ANSWERED,"echo_answered" },
        { SFX_ELEV_UP,      "elev_up" },
        { SFX_ELEV_DOWN,    "elev_down" },
        { SFX_DOOR,         "door" },
        { SFX_BLIP,         "blip" },
        { SFX_BREATH,       "breath" },
        { SFX_STINGER,      "stinger" },
        { SFX_TRUTH,        "truth" },
    };

    int total_samples = 0;
    for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
        int n = echo_audio_render(table[i].sfx, buf, cap);
        if (n <= 0) { fprintf(stderr, "合成失败: %s\n", table[i].name); continue; }
        write_raw(table[i].name, buf, n);
        total_samples += n;
    }

    /* footstep：预生成 dist=0 的基础波形，运行时按距离调音量 */
    {
        int n = echo_audio_footstep_render(0, buf, cap);
        if (n > 0) {
            write_raw("footstep", buf, n);
            total_samples += n;
        }
    }

    /* 环境底噪：4 秒循环 */
    {
        int n = echo_audio_ambience_render(buf, cap);
        if (n > 0) {
            write_raw("ambience", buf, n);
            total_samples += n;
        }
    }

    free(buf);
    printf("\n总计: %d samples (%.1f KB)\n", total_samples, total_samples * 2 / 1024.0);
    printf("Flash 占用约 %.0f KB\n", total_samples * 2 / 1024.0);
    return 0;
}
