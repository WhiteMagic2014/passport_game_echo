/*
 * echo_history.c —— 历史档案（回声档案）实现。
 *
 * 存档只存 3 个位图 + 统计（12 字节），正文一律从 echo_core 静态表取，
 * 不重复占用 NVS / flash。
 */
#include <string.h>
#include "echo_history.h"
#include "echo_core.h"

#ifdef ESP_PLATFORM
#include "nvs_flash.h"
#include "nvs.h"
#define NVS_NS          "echo"
#define NVS_KEY_ARCHIVE "archive"
#else
/* 主机测试桩：用静态内存模拟持久化，进程内有效。 */
static EchoArchive g_stub_archive;
static bool g_stub_valid = false;
#endif

int echo_hist_total_ends(void) { return (END_COUNT - 1) + EGG_COUNT; }

void echo_hist_count(const EchoArchive *a, int *n_ends, int *n_frags) {
    int ne = 0, nf = 0;
    for (int i = 0; i < END_COUNT; i++) {
        if (i == END_EGG) continue;
        if (a->ends & (1u << i)) ne++;
    }
    for (int i = 0; i < EGG_COUNT; i++) {
        if (a->eggs & (1u << i)) ne++;
    }
    for (int i = 0; i < FRAGMENT_DEPTH_COUNT; i++) {
        if (a->frags & (1u << i)) nf++;
    }
    if (n_ends) *n_ends = ne;
    if (n_frags) *n_frags = nf;
}

/* ==========================================================================
 * 持久化
 * ========================================================================== */
void echo_hist_load(EchoArchive *a) {
    memset(a, 0, sizeof(*a));
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(*a);
    if (nvs_get_blob(h, NVS_KEY_ARCHIVE, a, &len) != ESP_OK || len != sizeof(*a)) {
        memset(a, 0, sizeof(*a));
    }
    nvs_close(h);
#else
    if (g_stub_valid) *a = g_stub_archive;
#endif
}

void echo_hist_save(const EchoArchive *a) {
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_ARCHIVE, a, sizeof(*a));
    nvs_commit(h);
    nvs_close(h);
#else
    g_stub_archive = *a;
    g_stub_valid = true;
#endif
}

/* ==========================================================================
 * 登记一局结果
 * ========================================================================== */
void echo_hist_record(EchoArchive *a, const EchoState *st) {
    if (!a || !st || st->ended < 0) return;

    a->runs = (a->runs < 0xFFFF) ? (uint16_t)(a->runs + 1) : 0xFFFF;
    if (st->max_depth > a->best_depth) a->best_depth = (int16_t)st->max_depth;
    int score = st->max_depth + st->fragments * 50;
    if (score > a->best_score) a->best_score = (int16_t)score;

    if (st->ended == END_EGG) {
        /* 彩蛋按具体楼层分格记录（与网页版一致） */
        int ei = echo_egg_index_of(st->egg_floor);
        if (ei < 0 && st->egg) ei = echo_egg_index_of(st->egg->depth);
        if (ei >= 0) a->eggs |= (uint16_t)(1u << ei);
    } else if (st->ended < END_COUNT) {
        a->ends |= (uint16_t)(1u << st->ended);
    }

    /* 碎片：本局拿到的片数按 0..n-1 记（叙事顺序固定，与网页版一致） */
    for (int i = 0; i < st->fragments && i < FRAGMENT_DEPTH_COUNT; i++) {
        a->frags |= (uint16_t)(1u << i);
    }
}

/* ==========================================================================
 * 浏览列表
 * ========================================================================== */
void echo_hist_build(HistView *v, const EchoArchive *a) {
    memset(v, 0, sizeof(*v));
    v->detail = -1;
    v->dscroll = 0;
    int n = 0;

    v->items[n].kind = HIST_HEAD; v->items[n].idx = 0; v->items[n].title = "结局"; n++;
    for (int i = 0; i < END_COUNT; i++) {
        if (i == END_EGG) continue;         /* 彩蛋单独列 */
        bool un = (a->ends & (1u << i)) != 0;
        v->items[n].kind = HIST_END;
        v->items[n].idx = (int8_t)i;
        v->items[n].unlocked = un;
        v->items[n].title = un ? echo_end_titles()[i] : NULL;
        n++;
    }
    for (int i = 0; i < EGG_COUNT; i++) {
        bool un = (a->eggs & (1u << i)) != 0;
        const EggDef *e = echo_egg_by_index(i);
        v->items[n].kind = HIST_EGG;
        v->items[n].idx = (int8_t)i;
        v->items[n].unlocked = un;
        v->items[n].title = (un && e) ? e->title : NULL;
        n++;
    }

    v->items[n].kind = HIST_HEAD; v->items[n].idx = 1; v->items[n].title = "碎片"; n++;
    static const char *FRAG_CN[FRAGMENT_DEPTH_COUNT] = { "一", "二", "三", "四", "五", "六" };
    for (int i = 0; i < FRAGMENT_DEPTH_COUNT; i++) {
        bool un = (a->frags & (1u << i)) != 0;
        v->items[n].kind = HIST_FRAG;
        v->items[n].idx = (int8_t)i;
        v->items[n].unlocked = un;
        v->items[n].title = un ? FRAG_CN[i] : NULL;
        n++;
    }

    v->count = n;
    v->sel = 1;                              /* 跳过第一个 head */
    v->scroll = 0;
}

void echo_hist_move(HistView *v, int dir) {
    if (!v || v->count <= 0) return;
    int i = v->sel;
    for (int k = 0; k < v->count; k++) {
        i += dir;
        if (i < 0) i = v->count - 1;
        if (i >= v->count) i = 0;
        if (v->items[i].kind != HIST_HEAD) break;
    }
    v->sel = i;
}

int echo_hist_visible_rows(int top_y, int bot_y, int row_h) {
    if (row_h <= 0) return 0;
    int r = (bot_y - top_y) / row_h;
    return r > 0 ? r : 0;
}

void echo_hist_ensure_visible(HistView *v, int rows) {
    if (!v || rows <= 0) return;
    if (v->sel < v->scroll) v->scroll = v->sel;
    else if (v->sel >= v->scroll + rows) v->scroll = v->sel - rows + 1;
    if (v->scroll < 0) v->scroll = 0;
    int max_s = v->count - rows;
    if (max_s < 0) max_s = 0;
    if (v->scroll > max_s) v->scroll = max_s;
}

/* ==========================================================================
 * 详情
 * ========================================================================== */
const char *echo_hist_item_title(const HistItem *it) {
    if (!it || !it->unlocked) return NULL;
    return it->title;
}

const char *echo_hist_item_text(const EchoArchive *a, const HistItem *it) {
    (void)a;
    if (!it || !it->unlocked) return NULL;
    if (it->kind == HIST_END) return echo_end_text_by_id(it->idx);
    if (it->kind == HIST_EGG) {
        const EggDef *e = echo_egg_by_index(it->idx);
        return e ? e->text : NULL;
    }
    if (it->kind == HIST_FRAG) return echo_fragment_text(it->idx);
    return NULL;
}
