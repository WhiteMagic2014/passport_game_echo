/*
 * echo_render.h —— 《回声·下沉》像素渲染层（C 版）。
 *
 * 由 prototype/js/render.js 逐行翻译而来。逻辑分辨率固定 240x320，
 * 与 ST7789P3 一致，逐像素直接写 RGB565 帧缓冲，不依赖 LVGL。
 *
 * 依赖：echo_core.h（状态）、font_zh.h（点阵字库，tools/gen_font.py 生成）。
 * 本文件不依赖任何 ESP-IDF / BSP 头文件，可在主机上编译做渲染冒烟测试。
 */
#pragma once

#include <stdint.h>
#include "echo_core.h"
#include "echo_history.h"

#define ECHO_W 240
#define ECHO_H 320

/* ==========================================================================
 * 调色板（与 render.js C 一致，RGB565 编码）
 * ========================================================================== */
#define RGB565(r, g, b) \
    ((uint16_t)((((uint8_t)(r) & 0xF8) << 8) | (((uint8_t)(g) & 0xFC) << 3) | ((uint8_t)(b) >> 3)))

#define C_BG      RGB565(0x0a, 0x0d, 0x12)   /* #0a0d12 背景 */
#define C_METAL   RGB565(0x2b, 0x35, 0x42)   /* #2b3542 铁皮 */
#define C_METALHI RGB565(0x3d, 0x4a, 0x5a)   /* #3d4a5a 铁皮高光 */
#define C_METALLO RGB565(0x1a, 0x21, 0x2a)   /* #1a212a 铁皮暗部 */
#define C_INK     RGB565(0x07, 0x0a, 0x0e)   /* #070a0e 近黑 */
#define C_TEXT    RGB565(0xc3, 0xce, 0xd9)   /* #c3ced9 正文 */
#define C_DIM     RGB565(0x5c, 0x69, 0x75)   /* #5c6975 次要 */
#define C_FAINT   RGB565(0x2a, 0x33, 0x3d)   /* #2a333d 弱 */
#define C_AMBER   RGB565(0xd8, 0xa3, 0x41)   /* #d8a341 琥珀 */
#define C_ECHO    RGB565(0x7f, 0xd4, 0xc1)   /* #7fd4c1 回声 */
#define C_DANGER  RGB565(0xc0, 0x39, 0x2b)   /* #c0392b 危险 */
#define C_FOOD    RGB565(0x9a, 0x86, 0x54)   /* #9a8654 食物 */
#define C_SANITY  RGB565(0x5f, 0x9e, 0x6a)   /* #5f9e6a 理智 */
#define C_LIFE    RGB565(0xc0, 0x39, 0x2b)   /* #c0392b 生命 */
#define C_RIVET   RGB565(0x4d, 0x5c, 0x6d)   /* #4d5c6d 铆钉 */

/* ==========================================================================
 * 主菜单（开始 / 继续 / 音量 / 历史）——与 HTML 版四项对齐
 * ========================================================================== */
typedef enum {
    MENU_MAIN = 0, MENU_VOLUME, MENU_HELP, MENU_HISTORY
} menu_screen_t;

typedef struct {
    int screen;        /* menu_screen_t */
    int sel;           /* 0..3，主菜单选中项 */
    uint8_t volume;    /* 0..100 */
    bool has_continue; /* 是否存在可续的存档 */
    int last_depth;    /* 上一局最深深度，<=0 表示无记录 */
    EchoArchive archive;  /* 历史档案（进入「历史」时从 NVS 载入） */
    HistView  hist;       /* 历史浏览视图 */
} MenuState;

/* ==========================================================================
 * 渲染整帧。
 * fb：RGB565 帧缓冲，容量 ECHO_W*ECHO_H。
 * tms：毫秒时间戳（自开机起），仅用于动画（波纹扩散 / 闪烁 / 结局呼吸）。
 * ========================================================================== */
void echo_render(const EchoState *st, uint16_t *fb, uint32_t tms);

/* 渲染主菜单（主屏 / 音量 / 说明）。tms 暂未用于动画，保留签名。 */
void echo_render_menu(const MenuState *m, uint16_t *fb, uint32_t tms);

/* 分带渲染：fb 只需容纳 [ybase, ybase+bh) 这一带（宽仍为 ECHO_W），
 * 供真机省 RAM（无 PSRAM，全帧 150KB 放不下）。主机测试走整帧版本。 */
void echo_render_band(const EchoState *st, uint16_t *fb, uint32_t tms, int ybase, int bh);
void echo_render_menu_band(const MenuState *m, uint16_t *fb, uint32_t tms, int ybase, int bh);
