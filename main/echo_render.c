/*
 * echo_render.c —— 《回声·下沉》像素渲染层实现。
 *
 * 布局（逻辑 240x320，与 render.js 对齐，点阵字比画布字略高故微调纵向）：
 *   - 状态栏 0..34
 *   - 声呐 34..238（中心 120,136 半径 92）
 *   - 消息区 238..288（3 行）
 *   - 底部 288..320（磨损行 + 按键提示）
 *
 * 字库：ASCII 8x16，中文 16x16，混排（UTF-8 解码）。
 */
#include "echo_render.h"
#include "font_zh.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * 布局常量
 * ------------------------------------------------------------------- */
#define STATUSBAR_H 34
#define SONAR_TOP   STATUSBAR_H
#define MSG_H       82
#define SONAR_H     (ECHO_H - STATUSBAR_H - MSG_H)   /* 204 */
#define MSG_TOP     (SONAR_TOP + SONAR_H)

#define SONAR_CX 120
#define SONAR_CY (SONAR_TOP + SONAR_H / 2)   /* 136 */
#define SONAR_R  92

#define PANEL_CY 160

enum { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };

/* ---------------------------------------------------------------------
 * 分带渲染上下文：把整帧按行分段画进小缓冲，省 RAM（ESP32-C3 无 PSRAM，
 * 全帧 240x320 RGB565 = 150KB 放不下）。g_y0/g_y1 为当前带的
 * [含上界, 不含下界)；默认覆盖整帧（主机测试直接画全帧）。
 * ------------------------------------------------------------------- */
static int g_y0 = 0;
static int g_y1 = ECHO_H;

/* ---------------------------------------------------------------------
 * 低层像素原语
 * ------------------------------------------------------------------- */
static inline uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {
    if (a == 0) return bg;
    if (a >= 255) return fg;
    int fr = (fg >> 11) & 0x1F, fg_ = (fg >> 5) & 0x3F, fb = fg & 0x1F;
    int br = (bg >> 11) & 0x1F, bg_ = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    int r = (fr * a + br * (255 - a)) / 255;
    int g = (fg_ * a + bg_ * (255 - a)) / 255;
    int b = (fb * a + bb * (255 - a)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline void set_px(uint16_t *fb, int x, int y, uint16_t c) {
    if (x < 0 || x >= ECHO_W) return;
    if (y < g_y0 || y >= g_y1) return;
    fb[(y - g_y0) * ECHO_W + x] = c;
}

static inline void set_px_a(uint16_t *fb, int x, int y, uint16_t c, uint8_t a) {
    if (x < 0 || x >= ECHO_W) return;
    if (y < g_y0 || y >= g_y1) return;
    if (a == 0) return;
    uint16_t *p = &fb[(y - g_y0) * ECHO_W + x];
    if (a >= 255) { *p = c; return; }
    *p = blend565(c, *p, a);
}

static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > ECHO_W) x1 = ECHO_W;
    int y1 = y + h; if (y1 > ECHO_H) y1 = ECHO_H;
    if (y0 < g_y0) y0 = g_y0;
    if (y1 > g_y1) y1 = g_y1;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++)
            fb[(j - g_y0) * ECHO_W + i] = c;
}

static void fill_rect_a(uint16_t *fb, int x, int y, int w, int h, uint16_t c, uint8_t a) {
    if (a == 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > ECHO_W) x1 = ECHO_W;
    int y1 = y + h; if (y1 > ECHO_H) y1 = ECHO_H;
    if (a >= 255) { fill_rect(fb, x, y, w, h, c); return; }
    if (y0 < g_y0) y0 = g_y0;
    if (y1 > g_y1) y1 = g_y1;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++)
            fb[(j - g_y0) * ECHO_W + i] = blend565(c, fb[(j - g_y0) * ECHO_W + i], a);
}

static void rect_outline(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < w; i++) { set_px(fb, x + i, y, c); set_px(fb, x + i, y + h - 1, c); }
    for (int j = 0; j < h; j++) { set_px(fb, x, y + j, c); set_px(fb, x + w - 1, y + j, c); }
}

static void circle_outline_a(uint16_t *fb, int cx, int cy, int r, uint16_t c, uint8_t a) {
    if (r < 0) return;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        set_px_a(fb, cx + x, cy + y, c, a);
        set_px_a(fb, cx + y, cy + x, c, a);
        set_px_a(fb, cx - y, cy + x, c, a);
        set_px_a(fb, cx - x, cy + y, c, a);
        set_px_a(fb, cx - x, cy - y, c, a);
        set_px_a(fb, cx - y, cy - x, c, a);
        set_px_a(fb, cx + y, cy - x, c, a);
        set_px_a(fb, cx + x, cy - y, c, a);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

static void circle_outline(uint16_t *fb, int cx, int cy, int r, uint16_t c) {
    circle_outline_a(fb, cx, cy, r, c, 255);
}

/* 虚线圆（render.js 用 setLineDash 画空楼层井道） */
static void circle_dashed_a(uint16_t *fb, int cx, int cy, int r, uint16_t c, uint8_t a) {
    int steps = (int)(2 * M_PI * r / 2);
    if (steps < 24) steps = 24;
    for (int i = 0; i < steps; i++) {
        if ((i / 4) & 1) continue;   /* 间隙 */
        double th = 2 * M_PI * i / steps;
        set_px_a(fb, cx + (int)(r * cos(th)), cy + (int)(r * sin(th)), c, a);
    }
}

/* ---------------------------------------------------------------------
 * 电梯厢体边框 + 螺丝装饰
 * ------------------------------------------------------------------- */
static void draw_screw(uint16_t *fb, int cx, int cy) {
    /* 8x8 螺丝帽 */
    fill_rect(fb, cx - 4, cy - 4, 8, 8, C_RIVET);
    fill_rect(fb, cx - 3, cy - 3, 6, 6, C_METALHI);
    /* 十字槽 */
    fill_rect(fb, cx - 3, cy, 6, 1, C_INK);
    fill_rect(fb, cx, cy - 3, 1, 6, C_INK);
}

static void draw_frame_border(uint16_t *fb) {
    /* 5px 厚边框：外高光 → 铁皮 → 阴影 → 暗缝 */
    rect_outline(fb, 0, 0, ECHO_W, ECHO_H, C_METALHI);
    rect_outline(fb, 1, 1, ECHO_W - 2, ECHO_H - 2, C_METAL);
    rect_outline(fb, 2, 2, ECHO_W - 4, ECHO_H - 4, C_METAL);
    rect_outline(fb, 3, 3, ECHO_W - 6, ECHO_H - 6, C_METALLO);
    rect_outline(fb, 4, 4, ECHO_W - 8, ECHO_H - 8, C_INK);
    /* 四角螺丝（移入厚框内） */
    draw_screw(fb, 10, 10);
    draw_screw(fb, ECHO_W - 11, 10);
    draw_screw(fb, 10, ECHO_H - 11);
    draw_screw(fb, ECHO_W - 11, ECHO_H - 11);
}

/* ---------------------------------------------------------------------
 * 字库：UTF-8 解码 + 点阵绘制
 * ------------------------------------------------------------------- */
static uint32_t utf8_decode(const char *s, int *adv) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *adv = 1; return c; }
    if ((c & 0xE0) == 0xC0) { *adv = 2; return ((uint32_t)(c & 0x1F) << 6) | (s[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0) {
        *adv = 3;
        return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0) {
        *adv = 4;
        return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
               ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    *adv = 1; return c;   /* 非法字节，原样跳过 */
}

static int zh_glyph_index(uint32_t cp) {
    int lo = 0, hi = (int)FONT_ZH_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t v = FONT_ZH_CODEPOINTS[mid];
        if (v == cp) return mid;
        if (v < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static void draw_glyph(uint16_t *fb, int x, int y, uint16_t c, uint8_t a,
                       const uint8_t *bmp, int w, int h) {
    int row_bytes = (w + 7) / 8;
    for (int r = 0; r < h; r++) {
        for (int col = 0; col < w; col++) {
            int byte = r * row_bytes + (col / 8);
            if (bmp[byte] & (1 << (7 - (col % 8))))
                set_px_a(fb, x + col, y + r, c, a);
        }
    }
}

static int text_measure_n(const char *s, int nbytes) {
    int w = 0;
    for (int i = 0; i < nbytes && s[i]; ) {
        int adv; uint32_t cp = utf8_decode(s + i, &adv);
        if (i + adv > nbytes) break;
        i += adv;
        w += (cp < 0x80) ? FONT_ASCII_W : FONT_ZH_W;
    }
    return w;
}

/* 带字间距文本测量/绘制（用于标题等需要拉开字距的场景） */
static int text_measure_spaced(const char *s, int spacing) {
    int w = 0, count = 0;
    for (int i = 0; s[i]; ) {
        int adv; uint32_t cp = utf8_decode(s + i, &adv);
        i += adv;
        w += (cp < 0x80) ? FONT_ASCII_W : FONT_ZH_W;
        count++;
    }
    return w + spacing * (count - 1);
}

static void draw_text_spaced(uint16_t *fb, const char *s,
                             int x, int y, uint16_t c, int align, int spacing) {
    int w = text_measure_spaced(s, spacing);
    if (align == ALIGN_CENTER) x -= w / 2;
    else if (align == ALIGN_RIGHT) x -= w;

    int cx = x, count = 0;
    for (int i = 0; s[i]; ) {
        int adv; uint32_t cp = utf8_decode(s + i, &adv);
        i += adv;
        if (count > 0) cx += spacing;
        if (cp == 0x20) { cx += FONT_ASCII_W; count++; continue; }
        if (cp < 0x80) {
            int idx = (int)cp - FONT_ASCII_BASE;
            if (idx >= 0 && idx < 95)
                draw_glyph(fb, cx, y, c, 255, FONT_ASCII[idx], FONT_ASCII_W, FONT_ASCII_H);
            cx += FONT_ASCII_W;
        } else {
            int idx = zh_glyph_index(cp);
            if (idx >= 0)
                draw_glyph(fb, cx, y, c, 255, FONT_ZH_BITMAP[idx], FONT_ZH_W, FONT_ZH_H);
            cx += FONT_ZH_W;
        }
        count++;
    }
}

static void draw_text_n(uint16_t *fb, const char *s, int nbytes,
                        int x, int y, uint16_t c, int align) {
    if (align == ALIGN_CENTER) x -= text_measure_n(s, nbytes) / 2;
    else if (align == ALIGN_RIGHT) x -= text_measure_n(s, nbytes);

    int cx = x;
    for (int i = 0; i < nbytes && s[i]; ) {
        int adv; uint32_t cp = utf8_decode(s + i, &adv);
        if (i + adv > nbytes) break;
        i += adv;
        if (cp == 0x20) { cx += FONT_ASCII_W; continue; }
        if (cp < 0x80) {
            int idx = (int)cp - FONT_ASCII_BASE;
            if (idx >= 0 && idx < 95)
                draw_glyph(fb, cx, y, c, 255, FONT_ASCII[idx], FONT_ASCII_W, FONT_ASCII_H);
            cx += FONT_ASCII_W;
        } else {
            int idx = zh_glyph_index(cp);
            if (idx >= 0)
                draw_glyph(fb, cx, y, c, 255, FONT_ZH_BITMAP[idx], FONT_ZH_W, FONT_ZH_H);
            cx += FONT_ZH_W;
        }
    }
}

static void draw_text(uint16_t *fb, const char *s, int x, int y, uint16_t c, int align) {
    draw_text_n(fb, s, (int)strlen(s), x, y, c, align);
}

/* ---------------------------------------------------------------------
 * 状态栏
 * ------------------------------------------------------------------- */
static void draw_bar(uint16_t *fb, int x, int y, int w, int h, double val, uint16_t c) {
    fill_rect(fb, x, y, w, h, C_FAINT);
    int fw = (int)(w * val / 100.0 + 0.5);
    if (fw > w) fw = w;
    if (fw < 0) fw = 0;
    fill_rect(fb, x, y, fw, h, c);
}

static void draw_status(uint16_t *fb, const EchoState *st) {
    fill_rect(fb, 0, 0, ECHO_W, STATUSBAR_H, C_METALLO);
    fill_rect(fb, 0, STATUSBAR_H - 1, ECHO_W, 1, C_FAINT);

    /* 内容右移4px、下移2px，避开5px厚框 */
    int sx = 8, sy = 11;

    char buf[16];
    snprintf(buf, sizeof(buf), "B%d", st->depth < 0 ? -st->depth : st->depth);
    draw_text(fb, buf, sx, sy, C_TEXT, ALIGN_LEFT);

    draw_text(fb, "食", sx + 30, sy, C_DIM, ALIGN_LEFT);
    draw_bar(fb, sx + 48, sy + 4, 34, 8, st->food, C_FOOD);

    draw_text(fb, "命", sx + 86, sy, C_DIM, ALIGN_LEFT);
    draw_bar(fb, sx + 104, sy + 4, 34, 8, st->health, C_LIFE);

    draw_text(fb, "智", sx + 142, sy, C_DIM, ALIGN_LEFT);
    draw_bar(fb, sx + 160, sy + 4, 34, 8, st->sanity, C_SANITY);

    draw_text(fb, "碎", sx + 198, sy, C_DIM, ALIGN_LEFT);
    snprintf(buf, sizeof(buf), "%d", st->fragments);
    draw_text(fb, buf, sx + 216, sy, C_DIM, ALIGN_LEFT);
}

/* ---------------------------------------------------------------------
 * 声呐主视图
 * ------------------------------------------------------------------- */
static void draw_floor_shape(uint16_t *fb, const EchoState *st, int sig, double waveR, uint8_t a);

static void draw_sonar(uint16_t *fb, const EchoState *st, uint32_t tms) {
    fill_rect(fb, 0, SONAR_TOP, ECHO_W, SONAR_H, C_BG);

    int active = (st->phase == PHASE_ECHOING && st->echo_depth == st->depth);
    double waveR = 0.0, shown = 0.0;
    if (active) {
        waveR = (st->echo_t / 4000.0) * (SONAR_R + 18);
        shown = st->echo_fired ? fmax(0.0, 1.0 - (st->echo_t - 1200.0) / 2800.0) : 0.0;
    }

    /* 井道参考圈 */
    for (int r = 24; r <= SONAR_R; r += 24)
        circle_outline(fb, SONAR_CX, SONAR_CY, r, C_FAINT);

    /* 扩散波纹 */
    if (active && waveR > 0 && waveR < SONAR_R + 18) {
        double al = fmax(0.0, 1.0 - waveR / (SONAR_R + 18));
        circle_outline_a(fb, SONAR_CX, SONAR_CY, (int)waveR, C_ECHO, (uint8_t)(al * 255));
    }

    /* 声波扫过后显现的门外轮廓 */
    if (shown > 0 && st->echo_sig >= 0)
        draw_floor_shape(fb, st, st->echo_sig, waveR, (uint8_t)(shown * 255));

    /* 电梯（你） */
    int sh = 0;
    if (st->flash > 0) sh = (int)(sin(tms / 40.0) * 2.0);
    fill_rect(fb, SONAR_CX - 9 + sh, SONAR_CY - 11, 18, 22, C_METAL);
    rect_outline(fb, SONAR_CX - 9 + sh, SONAR_CY - 11, 18, 22, C_METALHI);
    fill_rect(fb, SONAR_CX - 1 + sh, SONAR_CY - 11, 2, 22, C_INK);   /* 门缝 */

    /* 遭遇一闪 */
    if (st->flash > 0) {
        uint8_t al = (uint8_t)(fmin(1.0, st->flash) * 0.5 * 255);
        fill_rect_a(fb, 0, SONAR_TOP, ECHO_W, SONAR_H, C_DANGER, al);
    }
}

static void draw_floor_shape(uint16_t *fb, const EchoState *st, int sig, double waveR, uint8_t a) {
    int cx = SONAR_CX, cy = SONAR_CY;
    const Floor *f = echo_floor_at((EchoState *)st, st->depth);

    if (sig == ECHO_ALIVE || sig == ECHO_ANSWERED || (f && f->type == FT_ANOMALY)) {
        /* 活物：人形轮廓 */
        int hx = cx + 46, hy = cy - 24;
        if (waveR > 52) {
            fill_rect_a(fb, hx - 4, hy, 8, 8, C_DANGER, a);
            fill_rect_a(fb, hx - 6, hy + 9, 12, 22, C_DANGER, a);
            fill_rect_a(fb, hx - 8, hy + 31, 4, 12, C_DANGER, a);
            fill_rect_a(fb, hx + 4, hy + 31, 4, 12, C_DANGER, a);
        }
        if (sig == ECHO_ANSWERED && waveR > 80) {
            circle_outline_a(fb, cx, cy, 68, C_DANGER, a);
            circle_outline_a(fb, cx, cy, 82, C_DANGER, a);
        }
        return;
    }
    if (!f) return;

    if (f->type == FT_CLUTTER && waveR > 40) {
        fill_rect_a(fb, cx + 30, cy - 26, 14, 20, C_METAL, a);
        fill_rect_a(fb, cx + 48, cy - 4, 12, 26, C_METAL, a);
        fill_rect_a(fb, cx + 26, cy + 16, 22, 12, C_METAL, a);
        fill_rect_a(fb, cx - 52, cy - 18, 16, 34, C_METAL, a);
        return;
    }

    if (f->type == FT_POWER && waveR > 48) {
        fill_rect_a(fb, cx + 34, cy - 22, 26, 34, C_METAL, a);
        /* 配电室边框 + 指示灯 */
        rect_outline(fb, cx + 34, cy - 22, 26, 34, C_AMBER);
        fill_rect_a(fb, cx + 45, cy - 14, 4, 18, C_AMBER, a);
        return;
    }

    if (f->type == FT_MEMORY && waveR > 44) {
        fill_rect_a(fb, cx + 36, cy - 14, 18, 22, C_TEXT, a);
        fill_rect_a(fb, cx + 40, cy - 8, 10, 1, C_METALLO, a);
        fill_rect_a(fb, cx + 40, cy - 4, 10, 1, C_METALLO, a);
        fill_rect_a(fb, cx + 40, cy, 8, 1, C_METALLO, a);
        return;
    }

    /* 空楼层 */
    if (waveR > 70)
        circle_dashed_a(fb, cx, cy, SONAR_R - 4, C_METAL, a);
}

/* ---------------------------------------------------------------------
 * 消息区 / 底部
 * ------------------------------------------------------------------- */
static void draw_message(uint16_t *fb, const EchoState *st) {
    fill_rect(fb, 0, MSG_TOP, ECHO_W, MSG_H, C_METALLO);
    fill_rect(fb, 0, MSG_TOP, ECHO_W, 1, C_FAINT);

    const char *s = st->msg;
    int len = (int)strlen(s);
    int line = 0, start = 0, cnt = 0;
    for (int i = 0; i < len && line < 5; ) {
        int adv; utf8_decode(s + i, &adv);
        i += adv; cnt++;
        if (cnt >= 14) {
            draw_text_n(fb, s + start, i - start, 6, MSG_TOP + 2 + line * 16,
                        line == 0 ? C_TEXT : C_DIM, ALIGN_LEFT);
            line++; start = i; cnt = 0;
        }
    }
    if (line < 5 && start < len)
        draw_text_n(fb, s + start, len - start, 6, MSG_TOP + 2 + line * 16,
                    line == 0 ? C_TEXT : C_DIM, ALIGN_LEFT);
}

/* ---------------------------------------------------------------------
 * 控制面板
 * ------------------------------------------------------------------- */
static void draw_panel(uint16_t *fb, const EchoState *st) {
    /* 半透明压暗：覆盖声呐 + 消息区 */
    fill_rect_a(fb, 0, SONAR_TOP, ECHO_W, (MSG_TOP + MSG_H) - SONAR_TOP, C_INK, 184);

    PanelItem items[ECHO_MAX_PANEL_ITEMS];
    int n = echo_panel_items(st, items);

    int ph = 24 + n * 26;
    int py = PANEL_CY - ph / 2;
    /* 面板宽度须容纳最长的一对「标签 + 右对齐提示」：
     *   「不明食物」4 字 64px + 「恢复食物失去理智」8 字 128px = 192px
     *   标签起点 x+16、提示右对齐到 x+w-16 ⇒ 可用宽 = w-32，需 ≥192 并留出 8px 间隙
     *   ⇒ w = 232（原 172 只有 140px，提示会压到标签上） */
    int x = 4, w = 232;

    fill_rect(fb, x, py, w, ph, C_METAL);
    rect_outline(fb, x, py, w, ph, C_METALHI);
    fill_rect(fb, x, py, w, 1, C_METALHI);

    /* 铆钉 */
    int riv[4][2] = {
        { x + 5, py + 5 }, { x + w - 9, py + 5 },
        { x + 5, py + ph - 9 }, { x + w - 9, py + ph - 9 }
    };
    for (int i = 0; i < 4; i++) {
        fill_rect(fb, riv[i][0], riv[i][1], 4, 4, C_RIVET);
        fill_rect(fb, riv[i][0], riv[i][1], 2, 2, C_METALHI);
    }

    draw_text(fb, "控 制 面 板", x + w / 2, py + 7, C_DIM, ALIGN_CENTER);

    for (int k = 0; k < n; k++) {
        int iy = py + 22 + k * 26;
        int sel = (k == st->panel_sel);
        fill_rect(fb, x + 8, iy, w - 16, 22, sel ? C_METALHI : C_METALLO);
        if (sel) rect_outline(fb, x + 8, iy, w - 16, 22, C_AMBER);
        draw_text(fb, items[k].label, x + 16, iy + 3, sel ? C_AMBER : C_TEXT, ALIGN_LEFT);
        draw_text(fb, items[k].hint, x + w - 16, iy + 4, C_DIM, ALIGN_RIGHT);
    }
}

/* ---------------------------------------------------------------------
 * 结局
 * ------------------------------------------------------------------- */
static void draw_end(uint16_t *fb, const EchoState *st, uint32_t tms) {
    fill_rect(fb, 0, 0, ECHO_W, ECHO_H, C_INK);

    const char *title;
    switch (st->ended) {
        case END_TRUTH:   title = "回声引路"; break;   /* 真结局＝好结局：回声指路，你走了出去 */
        case END_STARVED: title = "你耗尽了"; break;
        case END_EGG:     title = st->egg ? st->egg->title : "彩蛋"; break;
        case END_BECAME:  title = "你成了下一个它"; break;
        case END_MADNESS: title = "理智归零"; break;
        case END_TAKEN:   title = "它带走了你"; break;
        default:          title = "电梯停了"; break;
    }
    draw_text(fb, title, ECHO_W / 2, 96, C_AMBER, ALIGN_CENTER);

    /* 结局文案，17 字折行居中 */
    const char *et = echo_end_text(st);
    int len = (int)strlen(et);
    int line = 0, start = 0, cnt = 0;
    for (int i = 0; i < len; ) {
        int adv; utf8_decode(et + i, &adv);
        i += adv; cnt++;
        /* 每行 15 字居中：15 × 16 = 240px 正好等于屏宽。
         * 原为 17 字（272px），居中后 x 起点为 -16，左右各被裁掉 1 字。 */
        if (cnt >= 15) {
            draw_text_n(fb, et + start, i - start, ECHO_W / 2, 136 + line * 16, C_TEXT, ALIGN_CENTER);
            line++; start = i; cnt = 0;
        }
    }
    if (start < len)
        draw_text_n(fb, et + start, len - start, ECHO_W / 2, 136 + line * 16, C_TEXT, ALIGN_CENTER);

    char buf[40];
    snprintf(buf, sizeof(buf), "最深 B%d　碎片 %d", st->max_depth, st->fragments);
    draw_text(fb, buf, ECHO_W / 2, 196, C_DIM, ALIGN_CENTER);
    snprintf(buf, sizeof(buf), "得分 %d", st->max_depth + st->fragments * 50);
    draw_text(fb, buf, ECHO_W / 2, 216, C_AMBER, ALIGN_CENTER);
    if (((tms / 700) & 1) == 0)
        draw_text(fb, "OK 重新开始", ECHO_W / 2, 262, C_DIM, ALIGN_CENTER);
}

/* ---------------------------------------------------------------------
 * 主菜单：开始 / 继续 / 音量 / 说明
 * ------------------------------------------------------------------- */
static void draw_menu_main(uint16_t *fb, const MenuState *m) {
    /* 标题：电梯楼层显示屏风格 */
    int dx = 24, dy = 14, dw = ECHO_W - 48, dh = 48;
    /* 金属外框 */
    fill_rect(fb, dx - 3, dy - 3, dw + 6, dh + 6, C_METAL);
    rect_outline(fb, dx - 3, dy - 3, dw + 6, dh + 6, C_METALHI);
    /* 暗底屏幕 */
    fill_rect(fb, dx, dy, dw, dh, C_INK);
    rect_outline(fb, dx, dy, dw, dh, C_FAINT);
    /* 屏幕角指示灯 */
    fill_rect(fb, dx + 3, dy + 3, 2, 2, C_AMBER);
    fill_rect(fb, dx + dw - 5, dy + 3, 2, 2, C_AMBER);
    fill_rect(fb, dx + 3, dy + dh - 5, 2, 2, C_AMBER);
    fill_rect(fb, dx + dw - 5, dy + dh - 5, 2, 2, C_AMBER);
    /* 琥珀色LED文字（拉开字距） */
    draw_text_spaced(fb, "回声 ECHO", ECHO_W / 2, dy + 16, C_AMBER, ALIGN_CENTER, 3);

    /* 分隔线 */
    fill_rect(fb, 40, 70, ECHO_W - 80, 1, C_METALHI);

    /* 菜单面板背景 */
    int px = 12, py = 96, pw = ECHO_W - 24, ph = 184;
    fill_rect(fb, px, py, pw, ph, C_METALLO);
    rect_outline(fb, px, py, pw, ph, C_METALHI);
    fill_rect(fb, px, py, pw, 1, C_METALHI);

    /* 面板四角铆钉 */
    int riv[4][2] = {
        { px + 5, py + 5 }, { px + pw - 9, py + 5 },
        { px + 5, py + ph - 9 }, { px + pw - 9, py + ph - 9 }
    };
    for (int i = 0; i < 4; i++) {
        fill_rect(fb, riv[i][0], riv[i][1], 4, 4, C_RIVET);
        fill_rect(fb, riv[i][0], riv[i][1], 2, 2, C_METALHI);
    }

    /* 每项严格对齐 BAND_H=40 边界，避免横跨带 */
    /* k=0: y=120  k=1: y=160  k=2: y=200  k=3: y=240 */
    int bar_x = 20, bar_w = ECHO_W - 40, bar_h = 28;

    /* 项 0：开始 */
    {
        int iy = 120, sel = (m->sel == 0);
        if (sel) { fill_rect(fb, bar_x, iy, bar_w, bar_h, C_METAL);
                    rect_outline(fb, bar_x, iy, bar_w, bar_h, C_AMBER); }
        draw_text(fb, sel ? "> 开始" : "  开始", 40, iy + 6,
                  sel ? C_AMBER : C_TEXT, ALIGN_LEFT);
    }
    /* 项 1：继续 */
    {
        int iy = 160, sel = (m->sel == 1);
        bool en = m->has_continue;
        if (sel) { fill_rect(fb, bar_x, iy, bar_w, bar_h, C_METAL);
                    rect_outline(fb, bar_x, iy, bar_w, bar_h, C_AMBER); }
        draw_text(fb, sel ? "> 继续" : "  继续", 40, iy + 6,
                  sel ? C_AMBER : (en ? C_TEXT : C_DIM), ALIGN_LEFT);
    }
    /* 项 2：音量 */
    {
        int iy = 200, sel = (m->sel == 2);
        if (sel) { fill_rect(fb, bar_x, iy, bar_w, bar_h, C_METAL);
                    rect_outline(fb, bar_x, iy, bar_w, bar_h, C_AMBER); }
        draw_text(fb, sel ? "> 音量" : "  音量", 40, iy + 6,
                  sel ? C_AMBER : C_TEXT, ALIGN_LEFT);
    }
    /* 项 3：说明 */
    {
        int iy = 240, sel = (m->sel == 3);
        if (sel) { fill_rect(fb, bar_x, iy, bar_w, bar_h, C_METAL);
                    rect_outline(fb, bar_x, iy, bar_w, bar_h, C_AMBER); }
        draw_text(fb, sel ? "> 说明" : "  说明", 40, iy + 6,
                  sel ? C_AMBER : C_TEXT, ALIGN_LEFT);
    }

    /* 底部分隔线 */
    fill_rect(fb, 40, 290, ECHO_W - 80, 1, C_METALHI);

    /* 底部状态条 */
    char buf[24];
    if (m->last_depth > 0)
        snprintf(buf, sizeof(buf), "最深 B%d", m->last_depth);
    else
        snprintf(buf, sizeof(buf), "尚无记录");
    draw_text(fb, buf, ECHO_W / 2, 298, C_FAINT, ALIGN_CENTER);
}

static void draw_menu_volume(uint16_t *fb, const MenuState *m) {
    draw_text(fb, "音量", ECHO_W / 2, 60, C_AMBER, ALIGN_CENTER);

    int bx = 40, bw = 160, by = 120, bh = 14;
    fill_rect(fb, bx, by, bw, bh, C_FAINT);
    fill_rect(fb, bx, by, bw * (int)m->volume / 100, bh, C_AMBER);
    rect_outline(fb, bx, by, bw, bh, C_METALHI);

    char b[8];
    snprintf(b, sizeof(b), "%d", m->volume);
    draw_text(fb, b, ECHO_W / 2, 146, C_TEXT, ALIGN_CENTER);

    draw_text(fb, "↑↓ 调整 · OK 返回", ECHO_W / 2, 210, C_FAINT, ALIGN_CENTER);
}

static void draw_menu_help(uint16_t *fb, const MenuState *m) {
    (void)m;
    draw_text(fb, "说明", ECHO_W / 2, 40, C_AMBER, ALIGN_CENTER);

    static const char *lines[] = {
        "↑↓  升降一层",
        "OK  打开控制面板",
        "长按OK  关闭面板",
        "面板内可呼喊、聆听、开门"
    };
    for (int i = 0; i < 4; i++)
        draw_text(fb, lines[i], 20, 100 + i * 28, C_TEXT, ALIGN_LEFT);

    draw_text(fb, "OK 返回", ECHO_W / 2, 296, C_FAINT, ALIGN_CENTER);
}

void echo_render_menu(const MenuState *m, uint16_t *fb, uint32_t tms) {
    (void)tms;
    fill_rect(fb, 0, 0, ECHO_W, ECHO_H, C_BG);
    draw_frame_border(fb);
    if (m->screen == MENU_VOLUME) { draw_menu_volume(fb, m); return; }
    if (m->screen == MENU_HELP)   { draw_menu_help(fb, m); return; }
    draw_menu_main(fb, m);
}

/* 分带渲染：fb 只需容纳 [ybase, ybase+bh) 这一带（宽仍为 ECHO_W）。
 * 用于真机省 RAM；主机测试仍走 echo_render_menu（画整帧）。 */
void echo_render_menu_band(const MenuState *m, uint16_t *fb, uint32_t tms, int ybase, int bh) {
    g_y0 = ybase;
    g_y1 = ybase + bh;
    echo_render_menu(m, fb, tms);
    g_y0 = 0;
    g_y1 = ECHO_H;
}

/* ---------------------------------------------------------------------
 * 总入口
 * ------------------------------------------------------------------- */
void echo_render(const EchoState *st, uint16_t *fb, uint32_t tms) {
    fill_rect(fb, 0, 0, ECHO_W, ECHO_H, C_BG);
    if (st->ended >= 0) { draw_end(fb, st, tms); draw_frame_border(fb); return; }
    draw_status(fb, st);
    draw_sonar(fb, st, tms);
    draw_message(fb, st);
    if (st->phase == PHASE_PANEL) draw_panel(fb, st);
    draw_frame_border(fb);
}

void echo_render_band(const EchoState *st, uint16_t *fb, uint32_t tms, int ybase, int bh) {
    g_y0 = ybase;
    g_y1 = ybase + bh;
    echo_render(st, fb, tms);
    g_y0 = 0;
    g_y1 = ECHO_H;
}
