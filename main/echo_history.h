/*
 * echo_history.h —— 《回声·下沉》历史档案（回声档案）。
 *
 * 对标 HTML 版 main.js 的 archive：记录解锁过的结局 / 彩蛋 / 碎片，可在主菜单
 * 「历史」里回看完整文案。
 *
 * 固件与网页的关键差异（ESP32-C3 无 PSRAM）：
 *   - 网页版把每条正文都存进 localStorage；固件版**只存解锁位**（3 个 uint16
 *     位图 + 统计，共 12 字节），正文一律从 echo_core 的静态表按需取——
 *     正文是常量，复制一份到 NVS 纯属浪费。
 *   - 彩蛋按楼层分别记录（eggs 位图），与网页版一致：6 个彩蛋各占一格。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "echo_core.h"

/* ==========================================================================
 * 档案（持久化到 NVS）
 * ========================================================================== */
typedef struct {
    uint16_t ends;        /* bit i = 结局 i 已解锁（跳过 END_EGG，它记在 eggs） */
    uint16_t eggs;        /* bit i = 第 i 个彩蛋已解锁（按楼层升序，见 echo_egg_by_index） */
    uint16_t frags;       /* bit i = 第 i 片碎片已解锁 */
    uint16_t runs;        /* 累计游玩局数 */
    int16_t  best_depth;  /* 历史最深 */
    int16_t  best_score;  /* 历史最高分 */
} EchoArchive;

/* 详情页正文区几何（渲染与输入共用，滚动夹取需要） */
#define HIST_DET_TOP  54
#define HIST_DET_BOT  286
#define HIST_DET_LH   18
#define HIST_DET_ROWS ((HIST_DET_BOT - HIST_DET_TOP) / HIST_DET_LH)

/* ==========================================================================
 * 浏览视图（运行时构建，不入存档）
 * ========================================================================== */
typedef enum { HIST_HEAD = 0, HIST_END, HIST_EGG, HIST_FRAG } hist_kind_t;

typedef struct {
    uint8_t  kind;        /* hist_kind_t */
    int8_t   idx;         /* HIST_END: end_t / HIST_EGG: 彩蛋索引 / HIST_FRAG: 碎片索引 */
    bool     unlocked;
    const char *title;    /* 已解锁=真实标题，未解锁=NULL（渲染层画「？？？」） */
} HistItem;

/* 1(head 结局) + (END_COUNT-1) + EGG_COUNT + 1(head 碎片) + FRAGMENT_DEPTH_COUNT */
#define HIST_MAX_ITEMS (1 + (END_COUNT - 1) + EGG_COUNT + 1 + FRAGMENT_DEPTH_COUNT)

typedef struct {
    int count;                    /* items 实际项数 */
    int sel;                      /* 当前选中（0..count-1，自动跳过 head） */
    int scroll;                   /* 列表顶部行偏移 */
    int detail;                   /* -1=列表；否则=正在看详情的 items 下标 */
    int dscroll;                  /* 详情正文滚动行数 */
    int dlines;                   /* 详情正文总行数（由渲染层填，用于滚动夹取） */
    HistItem items[HIST_MAX_ITEMS];
} HistView;

/* ==========================================================================
 * API
 * ========================================================================== */
/* 从 NVS 读档案；无存档/损坏时返回全零（未解锁）档案。 */
void echo_hist_load(EchoArchive *a);
/* 写回 NVS。 */
void echo_hist_save(const EchoArchive *a);

/* 一局结束时调用：按 st 的结局与碎片数登记解锁。 */
void echo_hist_record(EchoArchive *a, const EchoState *st);

/* 结局总数（普通结局 + 各彩蛋），用于「结局 x/13」统计。 */
int echo_hist_total_ends(void);
/* 统计已解锁数量。 */
void echo_hist_count(const EchoArchive *a, int *n_ends, int *n_frags);

/* 构建浏览列表。sel 会落在第一个可选条目上。 */
void echo_hist_build(HistView *v, const EchoArchive *a);
/* 上下移动选中（dir=±1），自动跳过 head 并夹取范围。 */
void echo_hist_move(HistView *v, int dir);
/* 列表显示窗口（行高 ROW_H），返回可视行数；并调整 scroll 使 sel 可见。 */
int echo_hist_visible_rows(int top_y, int bot_y, int row_h);
void echo_hist_ensure_visible(HistView *v, int rows);

/* 详情标题（未解锁返回 NULL）。 */
const char *echo_hist_item_title(const HistItem *it);
/* 详情正文（未解锁返回 NULL）。 */
const char *echo_hist_item_text(const EchoArchive *a, const HistItem *it);
