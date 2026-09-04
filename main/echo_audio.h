/*
 * echo_audio.h —— 《回声·下沉》音效合成层（C 版）。
 *
 * 由 prototype/js/audio.js 逐行翻译而来。全部运行时合成 PCM，不加载任何音频文件，
 * 与 JS 端一致。输出 16kHz / 16bit / mono，直接喂给 bsp_audio_write()。
 *
 * 本文件不依赖任何 ESP-IDF / BSP 头文件，可在主机上编译测试（tests/test_audio.c）。
 * 合成在调用线程内一次性完成（阻塞）；真机上应由独立音频任务调用，避免卡主循环。
 */
#pragma once

#include <stdint.h>

#define ECHO_SR 16000                 /* 采样率（与 BSP 一致） */
#define ECHO_AUDIO_MAX_SAMPLES 52000   /* 覆盖最长音效(echo 3.2s=51200)+余量，省24KB RAM */

/* 音效类型。与 main.js 的 event→sound 分发对齐（含 footstep 距离变量）。 */
typedef enum {
    SFX_KNOCK = 0,        /* 玩家呼喊 */
    SFX_KNOCK_SOFT,       /* 静默里飘来的那一声（silentEcho，更轻） */
    SFX_ECHO_OPEN,        /* 回声：开放 */
    SFX_ECHO_BLOCKED,     /* 回声：堵死 */
    SFX_ECHO_ALIVE,       /* 回声：活物（混呼吸） */
    SFX_ECHO_ANSWERED,    /* 回声：它回应（呼吸 + 多一次回音） */
    SFX_FOOTSTEP,         /* 它的脚步（距离变量，用 echo_audio_footstep_render） */
    SFX_ELEV_UP,          /* 电梯上行 */
    SFX_ELEV_DOWN,        /* 电梯下行 */
    SFX_DOOR,             /* 开门 */
    SFX_BLIP,             /* 面板/确认短音 */
    SFX_BREATH,           /* 呼吸 */
    SFX_STINGER,          /* 遭遇刺音 */
    SFX_TRUTH             /* 真结局泛音 */
} sfx_t;

/* 某音效的总采样数（16kHz mono）。 */
int echo_audio_samples(sfx_t sfx);

/* 合成整段音效到 out（int16 mono）。out 容量需 >= echo_audio_samples(sfx)。
 * 返回写入的采样数。out 会被覆盖（先清零再叠加），可安全复用缓冲。 */
int echo_audio_render(sfx_t sfx, int16_t *out, int cap);

/* 脚步声：dist 为它与你之间的层数差，决定音量与远近感。 */
int echo_audio_footstep_render(int dist, int16_t *out, int cap);

/* 环境底噪（4 秒循环，可无缝拼接自循环）。 */
int echo_audio_ambience_render(int16_t *out, int cap);

/* 环境底噪流式生成：写入 out[0..count)，对应全局采样下标 [start, start+count)。
 * 无状态（正弦相位由 start 决定，噪声为白噪声），供音频任务分块合成、免去大循环缓冲。 */
void echo_audio_ambience_fill(int16_t *out, int64_t start, int count);

/* 主音量 0..1（默认 0.85，与 main.js 一致）。 */
void echo_audio_set_volume(double v);
double echo_audio_volume(void);

/* 噪声种子（默认固定；真机可传 esp_random() 增加随机感）。 */
void echo_audio_seed(uint32_t s);
