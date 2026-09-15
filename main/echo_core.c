/*
 * echo_core.c —— 《回声·下沉》纯逻辑状态机实现。
 * 由 prototype/js/core.js 逐行翻译。详见 echo_core.h 顶部说明。
 */
#include "echo_core.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* ==========================================================================
 * 伪随机：uint32 全程运算，与 JS Math.imul / ToInt32 逐位一致
 * ========================================================================== */
uint32_t echo_hash32(uint32_t a, uint32_t b) {
    uint32_t h = (a ^ 0x9E3779B9u) + (b * 0x85EBCA6Bu);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

static uint32_t rng_u32(EchoState *st) {
    st->rng ^= st->rng >> 13;
    st->rng = st->rng * 0x19660Du;
    return st->rng;
}

static double rand01(EchoState *st) {
    return (double)rng_u32(st) / 4294967296.0;
}

static int rand_int(EchoState *st, int min, int max) {
    return min + (int)(rand01(st) * (double)(max - min + 1));
}

/* 一次性事件槽 */
static void emit(EchoState *st, event_t ev) {
    if (st->ev_count < ECHO_MAX_EVENTS) {
        st->events[st->ev_count++] = ev;
    }
}

/* ==========================================================================
 * 静态表
 * ========================================================================== */
static const int FRAGMENT_DEPTHS_TABLE[FRAGMENT_DEPTH_COUNT] = FRAGMENT_DEPTHS;

static const EggDef EGGS[] = {
    { 4,  "死",         "电梯停在 B4。你盯着那个数字，忽然觉得它读作另一个字。" },
    { 13, "缺席的楼层", "这栋楼的按钮面板上没有 13。可你正停在这里，数字是灭的。" },
    { 18, "如坠地狱",   "虽然不是地狱，但这宛如十八层地狱。" },
    { 44, "双重的死",   "B44。两个四并排。门外的黑暗里，也站着两个你。" },
    { 66, "大顺",       "六六大顺。可电梯偏偏在这一层卡住了，像在提醒你：这趟从没顺过。" },
    { 88, "再见",       "B88。88，再见。你想跟谁道个别，却怎么也想不起那个名字。" }
};
/* EGG_COUNT 定义在 echo_core.h（历史档案需要它做静态数组维度）。
 * 增删彩蛋时必须同步修改 echo_core.h 的 EGG_COUNT，且表要按楼层升序。 */

const EggDef *echo_egg_at(int depth) {
    for (size_t i = 0; i < EGG_COUNT; i++) {
        if (EGGS[i].depth == depth) return &EGGS[i];
    }
    return NULL;
}

const EggDef *echo_egg_by_index(int i) {
    if (i < 0 || i >= EGG_COUNT) return NULL;
    return &EGGS[i];
}

int echo_egg_index_of(int depth) {
    for (int i = 0; i < EGG_COUNT; i++) {
        if (EGGS[i].depth == depth) return i;
    }
    return -1;
}

/* 结局标题（按 end_t 下标），历史档案列未解锁条目时用。
 * END_EGG 那格只是占位，实际显示具体彩蛋的 title。 */
static const char *const END_TITLES[END_COUNT] = {
    "电梯停了",        /* 0 STALLED  */
    "理智归零",        /* 1 MADNESS  */
    "它带走了你",      /* 2 TAKEN    */
    "你成了下一个它",  /* 3 BECAME   */
    "彩蛋楼层",        /* 4 EGG（占位） */
    "六六大顺",        /* 5 TRUTH    */
    "你耗尽了",        /* 6 STARVED  */
    "困层"             /* 7 STRANDED */
};
const char *const *echo_end_titles(void) { return END_TITLES; }

/* 六张碎片：同一件事的六个侧面，沿「第三声」这条暗线推进。
 * 按「第几片」(0..5) 取文案：碎片层每局随机，但叙事顺序必须固定——
 * 玩家总是先捡到第一片（风/井/第三声），最后捡到第六片（照片里的人是你）。 */
const char *echo_fragment_text(int idx) {
    static const char *T[6] = {
        "一张住户留言，字迹工整：「最近井道里总有回声，物业说是风。可风不会数数：第一声是风，第二声是井，第三声会比你晚一点回来。别让第三声听见你的名字。」",
        "一张寻人启事，边角被撕过：「她最后出现在一层电梯。监控拍到她对着门喊了三声。门开了，她笑了，像听见有人在叫她的名字。电梯没停过。」",
        "维修记录，字迹发抖：「钢缆换过七次。每换一次，井道就比图纸深一层。第七次工单的签名栏是空的——可那笔迹我认得，是我的。」",
        "一张纸，只写了一行：「第 47 个。他已经数到第三声了。别叫他的名字——叫了，他就会回答。」",
        "录音笔还有电。按下播放：三声呼喊。第三声之后还有第四声，那个声音在数数，一直数到六。你还没有开口。",
        "一张照片。电梯里的人抬头看着镜头。是你，但衣服不是你的——那是十二层寻人启事上，她失踪那天穿的。"
    };
    if (idx < 0 || idx > 5) return "又一张纸，什么都没写。";
    return T[idx];
}

/* 聆听对「怪物」的感知：模糊、不给方向/精确层数。
 * 理智越低越失真：轻度误判一档（远近混淆），重度直接变成幻觉（无信息量）。 */
static const char *LISTEN_TIERS[] = {
    "井道深处很安静。安静得不真实。",
    "远处有动静。你听不出多远，也听不出在哪。",
    "很近了。近得让你不想开门。",
    "近得不正常。它就在这一带。"
};
static const char *LISTEN_HALLUC[] = {
    "你听见了什么。可又好像什么都没有。",
    "回声在你耳朵里打转，分不清远近。",
    "有什么在听你。这一点你很确定。"
};

/* 碎片层每局随机，需要给玩家一条能定位的线索，否则只能逐层盲开。
 * 线索只分两档、且不说上下。写法上统一落在「回声」上，不去写听见纸的响动——
 * 纸不是声源，它是吸声的东西：门缝被薄薄地垫住，回声到那里就哑了、钝了、软了。 */
static const char *FRAG_HINT_HERE[] = {
    "回声撞在门缝上就散了。底下垫着东西，很薄，把声音吸了进去。",
    "你数着回声。第三声没回来——门缝底下压着什么，很薄，把它接住了。",
    "门缝那儿是哑的。回声走到那里就没了，像被一张纸捂住。"
};
static const char *FRAG_HINT_NEAR[] = {
    "井道里有一层是哑的。回声经过时不响——那层的门缝，被垫住了。",
    "回声从隔壁那层回来时是钝的。有什么薄薄的东西，压在它的门缝上。",
    "隔壁那层的安静不一样。回声到那儿会软一下，像碰到了纸。"
};

/* ==========================================================================
 * 消息辅助
 * ========================================================================== */
static void set_msg(EchoState *st, const char *s) {
    snprintf(st->msg, sizeof(st->msg), "%s", s);
}

/* ==========================================================================
 * 楼层生成
 * ========================================================================== */
static bool is_frag_depth(const EchoState *st, int depth) {
    for (int i = 0; i < FRAGMENT_DEPTH_COUNT; i++) {
        if (st->fragment_depths[i] == depth) return true;
    }
    for (int i = 0; i < FRAGMENT_DEPTH_COUNT; i++) {
        if (FRAGMENT_DEPTHS_TABLE[i] == depth) return true;  /* 旧存档兜底 */
    }
    return false;
}

static bool floor_flag(const Floor *f, int bit) { return (f->flags & bit) != 0; }
static void floor_set(Floor *f, int bit, bool v) {
    /* 注意：flags 是 uint16_t（bit8 claimed），不要截断成 uint8_t */
    if (v) f->flags = (uint16_t)(f->flags | bit);
    else   f->flags = (uint16_t)(f->flags & ~bit);
}

static Floor *floor_at(EchoState *st, int depth) {
    int idx = depth + ECHO_FLOOR_OFFSET;
    if (idx < 0 || idx >= ECHO_FLOOR_SLOTS) return NULL;
    Floor *f = &st->floors[idx];
    if (floor_flag(f, FLOOR_F_GENERATED)) {
        return f;
    }

    double r  = (double)echo_hash32((uint32_t)st->seed, (uint32_t)(int32_t)depth) / 4294967296.0;
    double r2 = (double)echo_hash32((uint32_t)st->seed ^ 0x5BF03635u, (uint32_t)(int32_t)depth) / 4294967296.0;
    double r3 = (double)echo_hash32((uint32_t)st->seed ^ 0x1B873593u, (uint32_t)(int32_t)depth) / 4294967296.0;

    /* 已移除楼梯层：原 10% 配额按原比例摊回其余类型（总和重新归一到 1.0） */
    int type;
    if (r < 0.33) type = FT_EMPTY;
    else if (r < 0.53) type = FT_CLUTTER;
    else if (r < 0.71) type = FT_POWER;
    else if (r < 0.87) type = FT_MEMORY;
    else type = FT_ANOMALY;

    bool frag = is_frag_depth(st, depth);
    f->type = (uint8_t)(frag ? FT_MEMORY : type);
    f->flags = 0;
    floor_set(f, FLOOR_F_FRAGMENT, frag);
    floor_set(f, FLOOR_F_FRAGMENT_TAKEN, false);
    /* 碎片层清空一切会顶掉碎片的掉落 */
    floor_set(f, FLOOR_F_HOPE,         !frag && (r3 < 0.20));
    floor_set(f, FLOOR_F_LOOT,         !frag && (r3 >= 0.20 && r3 < 0.60));
    floor_set(f, FLOOR_F_TOOL,         !frag && ((type == FT_POWER && r2 < 0.95) || (type == FT_CLUTTER && r2 < 0.40)));
    floor_set(f, FLOOR_F_UNKNOWN_FOOD, !frag && (depth >= 14 && r2 > 0.88));
    floor_set(f, FLOOR_F_GENERATED, true);
    return f;
}

const Floor *echo_floor_at(EchoState *st, int depth) {
    return floor_at(st, depth);
}

/* ==========================================================================
 * 怪物（「它」）—— 默认 1 只，开局刷新在随机楼层
 * ========================================================================== */
static int nearest_dist(const EchoState *st);
static int nearest_pos(const EchoState *st);
static bool monster_at(const EchoState *st, int depth);
static bool monster_here(const EchoState *st);
static void monster_step(EchoState *st);
static void derive_danger(EchoState *st);
static void spawn_monsters(EchoState *st);
static int fragment_index_at(const EchoState *st, int depth);

static int nearest_dist(const EchoState *st) {
    int best = 9999;
    for (int i = 0; i < st->monster_count; i++) {
        int d = st->monsters[i] - st->depth;
        if (d < 0) d = -d;
        if (d < best) best = d;
    }
    return best;
}
static int nearest_pos(const EchoState *st) {
    int best = 9999, pos = st->depth;
    for (int i = 0; i < st->monster_count; i++) {
        int d = st->monsters[i] - st->depth;
        if (d < 0) d = -d;
        if (d < best) { best = d; pos = st->monsters[i]; }
    }
    return pos;
}
static bool monster_at(const EchoState *st, int depth) {
    for (int i = 0; i < st->monster_count; i++) {
        if (st->monsters[i] == depth) return true;
    }
    return false;
}
static bool monster_here(const EchoState *st) {
    return monster_at(st, st->depth);
}

/* 每回合推进怪物：被标记 → 朝信标 1~3 层；否则在附近游荡（±1 / 不动） */
static void monster_step(EchoState *st) {
    for (int i = 0; i < st->monster_count; i++) {
        int *p = &st->monsters[i];
        if (st->marked) {
            int step = rand_int(st, CFG_HOMING_MIN, CFG_HOMING_MAX);
            if (*p < st->mark_floor) *p = (*p + step > st->mark_floor) ? st->mark_floor : (*p + step);
            else if (*p > st->mark_floor) *p = (*p - step < st->mark_floor) ? st->mark_floor : (*p - step);
        } else {
            /* 未被标记：在附近随机游荡（随机上下 1 层，或不动）—— 不朝玩家偏移 */
            if (rand01(st) < CFG_WANDER_STEP) {
                *p += (rand01(st) < 0.5 ? 1 : -1);
            }
        }
        if (*p < 0) *p = 0;
        if (*p > 100) *p = 100;
    }

    bool here = monster_here(st);
    if (here && !st->fear_here) {
        st->fear_here = true;
        st->sanity -= CFG_SANITY_BREATH;
        set_msg(st, "门外的脚步停下了。");
        emit(st, EV_BREATH);
    } else if (!here) {
        st->fear_here = false;
    }
}

/* 危险由「最近怪物距离」实时推导：越近越高，半径外归零。 */
static void derive_danger(EchoState *st) {
    int d = nearest_dist(st);
    double r = CFG_DANGER_RADIUS;
    double v = (d >= r) ? 0.0 : (1.0 - (double)d / r);
    double val = CFG_DANGER_MAX * pow(v, 1.1);
    st->danger = (double)((int)(val + 0.5));
    if (st->danger < 0) st->danger = 0;
    if (st->danger > CFG_DANGER_MAX) st->danger = CFG_DANGER_MAX;
}

static void spawn_monsters(EchoState *st) {
    int n = CFG_MONSTER_COUNT;
    if (n > ECHO_MAX_MONSTERS) n = ECHO_MAX_MONSTERS;
    if (n < 1) n = 1;
    int count = 0, guard = 0;
    while (count < n && guard < 200) {
        guard++;
        int fl = CFG_MONSTER_SPAWN_MIN +
                 (int)(rand01(st) * (double)(CFG_MONSTER_SPAWN_MAX - CFG_MONSTER_SPAWN_MIN + 1));
        if (fl < CFG_MONSTER_SPAWN_MIN) fl = CFG_MONSTER_SPAWN_MIN;
        if (fl > CFG_MONSTER_SPAWN_MAX) fl = CFG_MONSTER_SPAWN_MAX;
        bool dup = false;
        for (int k = 0; k < count; k++) if (st->monsters[k] == fl) dup = true;
        if (dup) continue;
        st->monsters[count++] = fl;
    }
    st->monster_count = count;
}

/* 公开查询（供主循环触发脚步音效等集成使用） */
int echo_nearest_pos(const EchoState *st) { return nearest_pos(st); }
int echo_nearest_dist(const EchoState *st) { return nearest_dist(st); }
bool echo_monster_here(const EchoState *st) { return monster_here(st); }

/* ==========================================================================
 * 新建一局
 * ========================================================================== */
/* 碎片层：每局在 [MIN_DEPTH, MAX_DEPTH] 内纯随机抽 6 个不重复楼层（升序）。
 * 玩家可以下去再上来，所以不做「必须早于 66 层」之类的约束。 */
static void roll_fragment_depths(EchoState *st) {
    const int lo = FRAGMENT_MIN_DEPTH, hi = FRAGMENT_MAX_DEPTH, n = FRAGMENT_DEPTH_COUNT;
    bool picked[FRAGMENT_MAX_DEPTH + 2];
    for (int i = 0; i <= hi + 1; i++) picked[i] = false;
    int arr[FRAGMENT_DEPTH_COUNT];
    int count = 0, guard = 0;
    while (count < n && guard < 4000) {
        guard++;
        /* 用 hash32 而非 rand01：与 seed 强相关、分布均匀，且不消耗本局主随机流 */
        uint32_t r = echo_hash32((uint32_t)st->seed ^ 0x5A17C0DEu, (uint32_t)(guard * 13 + 7));
        int v = lo + (int)(r % (uint32_t)(hi - lo + 1));
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        if (picked[v]) continue;
        picked[v] = true;
        arr[count++] = v;
    }
    for (int v2 = lo; count < n && v2 <= hi; v2++) {
        if (!picked[v2]) { picked[v2] = true; arr[count++] = v2; }
    }
    /* 升序（插入排序，n 很小） */
    for (int i = 1; i < count; i++) {
        int key = arr[i], j = i - 1;
        while (j >= 0 && arr[j] > key) { arr[j + 1] = arr[j]; j--; }
        arr[j + 1] = key;
    }
    for (int i = 0; i < FRAGMENT_DEPTH_COUNT; i++) {
        st->fragment_depths[i] = (i < count) ? arr[i] : (lo + i);
    }
}

void echo_new_game(EchoState *st, int32_t seed, const LastRun *last) {
    memset(st, 0, sizeof(*st));
    st->seed = (seed != 0) ? seed : 1;
    st->depth = 0;
    st->food = CFG_FOOD_START;
    st->sanity = 100.0;
    st->health = 100.0;
    st->wear = 0.0;
    st->danger = 0.0;

    st->repairs = 0;
    st->fragments = 0;
    st->max_depth = 0;
    st->ate_unknown = false;
    st->enc = 0;
    st->doors = 0;
    st->echoes = 0;

    st->monster_count = 0;
    st->marked = false;
    st->mark_floor = -99;
    st->fear_here = false;

    st->phase = PHASE_IDLE;
    st->panel_sel = 0;
    st->pending_unknown = false;
    st->ended = -1;
    st->egg = NULL;
    st->egg_floor = -1;

    st->echo_t = 0.0;
    st->echo_sig = -1;
    st->echo_depth = -99;
    st->echo_fired = false;

    st->door_t = 0.0;
    st->door_truth = false;
    st->door_result.type = FT_EMPTY;
    st->door_result.got_count = 0;

    st->trick_silent = 5;
    st->trick_cut = 8;
    st->trick_ahead = 10;

    st->run_echo_count = 0;
    if (last) {
        st->last_run = *last;
        st->has_last_run = true;
    } else {
        st->has_last_run = false;
    }

    st->ev_count = 0;
    st->flash = 0.0;
    st->turn = 0;

    st->rng = (uint32_t)st->seed ^ 0x1234567u;

    roll_fragment_depths(st);
    spawn_monsters(st);

    set_msg(st, echo_wear_line(st));
}

/* ==========================================================================
 * 磨损文字 / 停摆
 * ========================================================================== */
const char *echo_wear_line(const EchoState *st) {
    double w = st->wear / CFG_WEAR_MAX;
    if (w < 0.25) return "电机运转平稳，只有轻微的机械声";
    if (w < 0.50) return "运行时轿厢有些晃动";
    if (w < 0.75) return "钢缆摩擦声变得刺耳，轿厢在井道里摇晃";
    if (w < 1.00) return "有什么金属部件正在疲劳断裂";
    return "电梯拒绝响应。门还能开。";
}

bool echo_is_stalled(const EchoState *st) {
    return st->wear >= CFG_WEAR_MAX;
}

/* ==========================================================================
 * 失真 / 上局足迹
 * ========================================================================== */
double echo_lie_chance(const EchoState *st) {
    if (st->sanity >= CFG_SANITY_LIE) return 0.0;
    if (st->sanity >= CFG_SANITY_LIE_HARD) {
        return 0.25 * (1.0 - (st->sanity - CFG_SANITY_LIE_HARD) / (CFG_SANITY_LIE - CFG_SANITY_LIE_HARD));
    }
    return 0.25 + 0.45 * (1.0 - st->sanity / CFG_SANITY_LIE_HARD);
}

double echo_ghost_weight(const EchoState *st, int depth) {
    if (!st->has_last_run) return 0.0;
    int n = 0;
    for (int i = 0; i < st->last_run.echo_count; i++) {
        if (st->last_run.echo_depths[i] == depth) n++;
    }
    return n > 3 ? 0.35 : n * 0.09;
}

/* ==========================================================================
 * 回声签名
 * ========================================================================== */
int echo_compute_sig(EchoState *st, int depth) {
    Floor *f = floor_at(st, depth);
    if (!f) return ECHO_OPEN;

    int sig;
    if (monster_at(st, depth)) sig = ECHO_ALIVE;
    else if (f->type == FT_ANOMALY) sig = ECHO_ALIVE;
    else if (f->type == FT_CLUTTER) sig = ECHO_BLOCKED;
    else sig = ECHO_OPEN;

    bool was_seen = floor_flag(f, FLOOR_F_SEEN);
    floor_set(f, FLOOR_F_SEEN, true);

    if (sig == ECHO_ALIVE && abs(nearest_pos(st) - depth) <= 1) {
        if (rand01(st) < 0.35 + echo_ghost_weight(st, depth)) sig = ECHO_ANSWERED;
    }

    if (was_seen && rand01(st) < 0.08) {
        sig = (sig == ECHO_OPEN) ? ECHO_BLOCKED : ECHO_OPEN;
        emit(st, EV_MEMORY_LIE);
    }

    double lie = echo_lie_chance(st) + echo_ghost_weight(st, depth);
    if (rand01(st) < lie) {
        if (sig == ECHO_ALIVE || sig == ECHO_ANSWERED) sig = ECHO_OPEN;
        else if (rand01(st) < 0.5) sig = ECHO_ALIVE;
    }
    return sig;
}

/* ==========================================================================
 * 恐怖手法
 * ========================================================================== */
static void advance_tricks(EchoState *st) {
    int d = st->depth;

    st->trick_silent--;
    if (st->trick_silent <= 0) {
        st->trick_silent = rand_int(st, CFG_TRICK_SILENT_MIN, CFG_TRICK_SILENT_MAX);
        if (d >= CFG_TRICK_DEPTH_SILENT) emit(st, EV_SILENT_ECHO);
    }

    st->trick_cut--;
    if (st->trick_cut <= 0) {
        st->trick_cut = rand_int(st, CFG_TRICK_CUT_MIN, CFG_TRICK_CUT_MAX);
        if (d >= CFG_TRICK_DEPTH_CUT) emit(st, EV_AMBIENCE_CUT);
    }

    st->trick_ahead--;
    if (st->trick_ahead <= 0) {
        st->trick_ahead = rand_int(st, CFG_TRICK_AHEAD_MIN, CFG_TRICK_AHEAD_MAX);
        if (d >= CFG_TRICK_DEPTH_AHEAD) {
            int above = d - nearest_pos(st);
            if (above > 0 && above <= 4) {
                emit(st, EV_AHEAD);
                st->sanity -= CFG_SANITY_AHEAD;
                set_msg(st, "上面传来刮擦声。三下，和你呼喊的节奏一样。");
            }
        }
    }
}

/* ==========================================================================
 * 结局判定
 * ========================================================================== */
static void end_check(EchoState *st) {
    if (st->health <= 0) {
        st->health = 0;
        st->ended = END_STARVED;
        st->phase = PHASE_OVER;
    } else if (st->sanity <= 0) {
        st->sanity = 0;
        st->ended = st->ate_unknown ? END_BECAME : END_MADNESS;
        st->phase = PHASE_OVER;
    } else if (echo_is_stalled(st) && st->food <= 0) {
        st->ended = END_STALLED;
        st->phase = PHASE_OVER;
    }
}

/* ==========================================================================
 * 回合推进
 * ========================================================================== */
static void end_turn(EchoState *st) {
    if (st->ended != -1) return;
    st->turn++;

    monster_step(st);
    derive_danger(st);

    double s_drain = (st->food > 0 ? CFG_SANITY_FED : CFG_SANITY_HUNGRY);
    if (st->health <= CFG_HEALTH_LOW) s_drain *= CFG_SANITY_LOW_MULT;
    st->sanity -= s_drain;

    if (st->food <= 0) {
        st->health -= CFG_HEALTH_HUNGRY;
        if (st->health < 0) st->health = 0;
    }

    advance_tricks(st);
    end_check(st);
}

/* ==========================================================================
 * 动作
 * ========================================================================== */
static bool can_act(const EchoState *st) {
    return st->ended == -1 && (st->phase == PHASE_IDLE || st->phase == PHASE_PANEL);
}

static int move(EchoState *st, int dir) {
    if (st->phase != PHASE_IDLE || st->ended != -1) return 0;
    if (st->depth <= 0 && dir < 0) return 0;   /* 地面层不能向上 */
    if (echo_is_stalled(st)) {
        set_msg(st, echo_wear_line(st));
        st->food -= 1.0;
        if (st->food < 0) st->food = 0;
        emit(st, EV_BLIP);
        end_turn(st);
        return 1;
    }

    st->depth += dir;
    if (st->depth > st->max_depth) st->max_depth = st->depth;

    st->wear += dir > 0 ? CFG_WEAR_DOWN : CFG_WEAR_UP;
    if (st->wear > CFG_WEAR_MAX) st->wear = CFG_WEAR_MAX;

    st->food -= CFG_FOOD_MOVE;
    if (st->food < 0) st->food = 0;

    set_msg(st, echo_wear_line(st));
    emit(st, dir > 0 ? EV_MOVE_DOWN : EV_MOVE_UP);
    floor_at(st, st->depth);

    end_turn(st);
    return 1;
}

/* 真结局（好结局）：集齐 6 张碎片后，在 66 层开门，回声反过来替你指路，
 * 电梯第一次向上，你走出了这栋楼（六六大顺）。控制在合理长度内。 */
static const char *truth_text(void) {
    return "六张碎片拼齐，停在第六十六层。六个六，凑成了顺。门开了，外面是纯白的光，白得没有影子，白得看不见地面。你记得老人们说，顺是吉兆；也记得，数到第三个六，是另一个名字。光把你吞了进去。你没有回头。";
}

static int start_echo(EchoState *st) {
    if (!can_act(st)) return 0;
    st->phase = PHASE_ECHOING;
    st->echo_t = 0.0;
    st->echo_fired = false;
    st->echo_depth = st->depth;
    st->echo_sig = echo_compute_sig(st, st->depth);

    st->food -= CFG_FOOD_ECHO;
    if (st->food < 0) st->food = 0;

    /* 标记信标：每次呼喊都把「你所在的层」设为信标（再次呐喊 = 刷新被标记的楼层）。
     * 怪物被标记后朝信标移动；停止呼喊，信标滞留旧层，怪物抵达后转为游荡——
     * 你靠不呼喊来甩脱。 */
    st->marked = true;
    st->mark_floor = st->depth;

    if (st->run_echo_count < ECHO_MAX_ECHO_DEPTHS) {
        st->run_echoes[st->run_echo_count++] = st->depth;
    }
    st->echoes++;

    emit(st, EV_KNOCK);
    end_turn(st);
    return 1;
}

/* 聆听结果：只分四档，不给出相隔层数，近距离连上下都不透露。
 * 贴脸（同层）与相邻（1 层）合并为同一档「它就在这一带」。 */
static const char *describe_listen(EchoState *st) {
    int ad = nearest_dist(st);
    int tier = ad >= 12 ? 0 : ad >= 6 ? 1 : ad >= 2 ? 2 : 3;
    const char *txt = LISTEN_TIERS[tier];
    double l = echo_lie_chance(st);   /* 0（理智高）~ 约 0.7（理智极低） */
    if (st->sanity < CFG_SANITY_LIE_HARD) {
        if (rand01(st) < l * 0.6) txt = LISTEN_HALLUC[rand_int(st, 0, 2)];
    } else if (l > 0) {
        if (rand01(st) < l) {
            int t2 = tier + (rand01(st) < 0.5 ? 1 : -1);
            if (t2 < 0) t2 = 0;
            if (t2 > 3) t2 = 3;
            txt = LISTEN_TIERS[t2];
        }
    }
    return txt;
}

/* 碎片线索：本层有碎片 → 回声被门缝吸住；隔壁有碎片 → 井道里有一层是哑的。 */
static int fragment_index_at(const EchoState *st, int depth) {
    for (int i = 0; i < FRAGMENT_DEPTH_COUNT; i++) {
        if (st->fragment_depths[i] == depth) return i;
    }
    return -1;
}

static const char *frag_hint(EchoState *st) {
    Floor *f = floor_at(st, st->depth);
    if (f && floor_flag(f, FLOOR_F_FRAGMENT) && !floor_flag(f, FLOOR_F_FRAGMENT_TAKEN)) {
        return FRAG_HINT_HERE[rand_int(st, 0, 2)];
    }
    for (int i = 0; i < FRAGMENT_DEPTH_COUNT; i++) {
        int d = st->fragment_depths[i];
        if (d < 0) continue;
        if (abs(d - st->depth) == 1) {
            Floor *nf = floor_at(st, d);
            if (nf && floor_flag(nf, FLOOR_F_FRAGMENT) && !floor_flag(nf, FLOOR_F_FRAGMENT_TAKEN)) {
                return FRAG_HINT_NEAR[rand_int(st, 0, 2)];
            }
        }
    }
    return NULL;
}

static int start_listen(EchoState *st) {
    if (!can_act(st)) return 0;
    st->food -= CFG_FOOD_LISTEN;
    if (st->food < 0) st->food = 0;

    const char *dl = describe_listen(st);
    const char *fh = frag_hint(st);
    if (fh) {
        snprintf(st->msg, sizeof(st->msg), "%s　%s", dl, fh);
    } else {
        set_msg(st, dl);
    }

    emit(st, EV_LISTEN);
    end_turn(st);
    return 1;
}

static int open_door(EchoState *st) {
    st->phase = PHASE_DOOR;
    st->door_t = 0.0;
    st->door_truth = false;
    st->doors++;
    emit(st, EV_DOOR);

    st->wear += CFG_WEAR_DOOR;
    if (st->wear > CFG_WEAR_MAX) st->wear = CFG_WEAR_MAX;
    st->food -= CFG_FOOD_DOOR;
    if (st->food < 0) st->food = 0;

    Floor *f = floor_at(st, st->depth);
    if (!f) { end_turn(st); return 0; }

    floor_type_t ft = (floor_type_t)f->type;
    int got = 0;

    /* 真结局：集满 6 张碎片，且在 66 层开门——六六大顺（优先于一切门后结果） */
    if (st->fragments >= FRAGMENT_DEPTH_COUNT && st->depth == 66) {
        st->door_truth = true;
        st->door_result.type = ft;
        st->door_result.got_count = 0;
        set_msg(st, "六十六层。门，开了。");
        return 1;   /* 不 endTurn：等开门演出结束，由 tick 收尾转入结局 */
    }

    if (monster_at(st, st->depth) || ft == FT_ANOMALY) {
        bool it_here = monster_at(st, st->depth);
        int lo = it_here ? CFG_HEALTH_HIT_IT_MIN : CFG_HEALTH_HIT_ANOMALY_MIN;
        int hi = it_here ? CFG_HEALTH_HIT_IT_MAX : CFG_HEALTH_HIT_ANOMALY_MAX;
        st->health -= rand_int(st, lo, hi);
        st->enc++;
        st->flash = 1.0;
        emit(st, EV_ENCOUNTER);
        if (st->health <= 0) {
            st->health = 0;
            st->ended = END_TAKEN;
            st->phase = PHASE_OVER;
            set_msg(st, "它已经站在门后。");
        } else {
            static const char *enc_msgs[] = {
                "门开的瞬间，它就在那里。你摔回电梯，死命按住关门键。门合上的前一秒，它伸进了手。",
                "门开了。它就在那里。你摔回电梯，死命按关门键。门关上了。",
                "门开的一瞬，你看见了它。你摔回去，死命关门。门合上时，它伸进了手。"
            };
            set_msg(st, enc_msgs[(int)(rand01(st) * 3) % 3]);
        }
    } else if (echo_egg_at(st->depth) && rand01(st) < CFG_EGG_CHANCE) {
        st->ended = END_EGG;
        st->phase = PHASE_OVER;
        st->flash = 1.0;
        st->egg = echo_egg_at(st->depth);
        st->egg_floor = st->depth;      /* 档案按楼层分别记录每个彩蛋 */
        set_msg(st, st->egg->text);
        got = 1;
        emit(st, EV_EGG);
    } else if (floor_flag(f, FLOOR_F_CLAIMED)) {
        /* 这一层的东西已经被取过一次了：再开只有风 */
        static const char *again_msgs[] = {
            "你已经把这层翻遍了。剩下的只有风。",
            "门又开了。这层已经被你掏空了，什么都没剩下。",
            "你再次推开这扇门。该拿的早拿走了，剩下的只有黑暗。"
        };
        set_msg(st, again_msgs[(int)(rand01(st) * 3) % 3]);
        emit(st, EV_EMPTY);
    } else if (floor_flag(f, FLOOR_F_TOOL)) {
        /* 递减：45 → 32.4 → 23.3 → 16.8 → 12.1 → 到底 12。保底每次仍回 12% 耐久度。 */
        double raw = CFG_REPAIR_BASE * pow(CFG_REPAIR_DECAY, (double)st->repairs);
        bool floored = raw <= CFG_REPAIR_FLOOR;
        double fix = floored ? CFG_REPAIR_FLOOR : raw;
        st->wear -= fix;
        if (st->wear < 0) st->wear = 0;
        st->repairs++;
        /* 不报数字：只描述「稳了多少」的手感，让玩家自己感觉修复在变弱。
         * 档位：big ≥30 / mid ≥18 / small <18（保底 12）。 */
        static const char *rep_big[] = {
            "你找到一套工具。把电梯松掉的地方都拧紧了。",
            "你摸到工具。钢缆重新绷紧，运行声低了下去。",
            "你找到些能用的东西。电梯稳了，像刚修好不久。"
        };
        static const char *rep_mid[] = {
            "你找到几件工具。你加固了能加固的地方。",
            "你摸到一些工具。轿厢还是晃，但轻了些。",
            "你找到工具。电梯没那么响了。"
        };
        static const char *rep_small[] = {
            "你找到一点能用的材料。你把它塞进电梯缝隙里，聊胜于无。",
            "你摸到几件工具。多数已经锈死，你只拧紧了两处。",
            "你找到些损坏的工具。勉强加固了电梯，你知道撑不了太久。"
        };
        const char **pool = fix >= 30.0 ? rep_big : (fix >= 18.0 ? rep_mid : rep_small);
        set_msg(st, pool[(int)(rand01(st) * 3) % 3]);
        if (floored) {
            strncat(st->msg, " 再修也只能这样了。", sizeof(st->msg) - strlen(st->msg) - 1);
        }
        floor_set(f, FLOOR_F_CLAIMED, true);
        got = 1;
        emit(st, EV_REPAIR);
    } else if (floor_flag(f, FLOOR_F_UNKNOWN_FOOD)) {
        static const char *food_msgs[] = {
            "一份还算新鲜的东西。你不想追问它是哪来的。",
            "一份东西。闻起来还行。你不想追问。",
            "角落里有一份东西。你不想追问它是哪来的。"
        };
        set_msg(st, food_msgs[(int)(rand01(st) * 3) % 3]);
        st->pending_unknown = true;
        floor_set(f, FLOOR_F_CLAIMED, true);
        got = 1;
        emit(st, EV_UNKNOWN_FOOD);
    } else if (floor_flag(f, FLOOR_F_FRAGMENT) && !floor_flag(f, FLOOR_F_FRAGMENT_TAKEN)) {
        st->fragments++;
        floor_set(f, FLOOR_F_FRAGMENT_TAKEN, true);
        floor_set(f, FLOOR_F_CLAIMED, true);
        int idx = fragment_index_at(st, st->depth);
        const char *txt = echo_fragment_text(idx);
        if (st->fragments >= FRAGMENT_DEPTH_COUNT) {
            snprintf(st->msg, sizeof(st->msg), "%s 六张碎片拼齐了。该去凑最后一道顺了。", txt);
        } else {
            set_msg(st, txt);
        }
        got = 1;
        emit(st, EV_FRAGMENT);
    } else if (floor_flag(f, FLOOR_F_HOPE)) {
        st->sanity += CFG_SANITY_HOPE;
        if (st->sanity > CFG_SANITY_MAX) st->sanity = CFG_SANITY_MAX;
        static const char *hope_msgs[] = {
            "你找到一件还认得的东西。握着它，你想起自己是谁，手不再抖了。",
            "你摸到一件旧东西。握着它，你想起了什么。手不再抖了。",
            "角落里有一件旧东西。你想起自己是谁。手不再抖了。"
        };
        set_msg(st, hope_msgs[(int)(rand01(st) * 3) % 3]);
        floor_set(f, FLOOR_F_CLAIMED, true);
        got = 1;
        emit(st, EV_HOPE);
    } else if (floor_flag(f, FLOOR_F_LOOT)) {
        double before = st->food;
        st->food += CFG_FOOD_LOOT;
        if (st->food > CFG_FOOD_CAP) st->food = CFG_FOOD_CAP;
        static const char *loot_msgs[] = {
            "你摸到几包还没过期的东西",
            "你找到一些吃的。还能吃。",
            "角落里有一份东西。是食物。"
        };
        const char *lm = loot_msgs[(int)(rand01(st) * 3) % 3];
        if (st->food - before < CFG_FOOD_LOOT) lm = "你找到一些吃的。你只拿得动这些了。";
        set_msg(st, lm);
        floor_set(f, FLOOR_F_CLAIMED, true);
        got = 1;
        emit(st, EV_LOOT);
    } else {
        static const char *empty_msgs[] = {
            "门外什么都没有。风从破窗里灌进来。",
            "门外什么都没有。黑暗里只有风。",
            "空的。什么都没有。安静得不正常。",
            "门外什么都没有。黑暗里什么在动。是风。"
        };
        set_msg(st, empty_msgs[(int)(rand01(st) * 4) % 4]);
        emit(st, EV_EMPTY);
    }

    /* 停摆后开门 = 最后一张牌。这一层若没有能修它的东西，就再没有下一层了：
     * 井道是这栋楼唯一的通道（没有楼梯），电梯不动 = 永远停在这一层。
     * 与 END_STALLED 互斥：STALLED 在 end_check 里（停摆 + 食物耗尽 + 没开门），
     * STRANDED 在这里（停摆 + 开了门 + 这层没工具）。 */
    if (st->ended == -1 && echo_is_stalled(st)) {
        st->ended = END_STRANDED;
        st->phase = PHASE_OVER;
        set_msg(st, "门外没有能修的东西。你走遍了这一层——没有楼梯，也没有向上的路。");
        emit(st, EV_STRANDED);
    }

    st->door_result.type = ft;
    st->door_result.got_count = got;

    end_turn(st);
    return 1;
}

static int eat_unknown(EchoState *st) {
    if (!st->pending_unknown) return 0;
    st->pending_unknown = false;
    st->ate_unknown = true;
    st->food = CFG_FOOD_CAP;
    st->sanity -= CFG_SANITY_UNSEEN;
    set_msg(st, "你吃了。味道说不上来。");
    emit(st, EV_ATE);
    end_turn(st);
    return 1;
}

/* 对应 JS declineUnknown：当前未被面板引用（JS 版同为死代码，保留对齐便于日后对照）。 */
__attribute__((unused))
static int decline_unknown(EchoState *st) {
    if (!st->pending_unknown) return 0;
    st->pending_unknown = false;
    set_msg(st, "你把它推回门外的黑暗里。");
    emit(st, EV_BLIP);
    end_turn(st);
    return 1;
}

/* ==========================================================================
 * 面板
 * ========================================================================== */
int echo_panel_items(const EchoState *st, PanelItem *out) {
    int n = 0;
    out[n++] = (PanelItem){ "echo",   "呼喊", "回声探测本层" };
    if (st->pending_unknown) {
        out[n++] = (PanelItem){ "eat", "不明食物", "恢复食物失去理智" };
    }
    out[n++] = (PanelItem){ "listen", "聆听", "模糊感知" };
    out[n++] = (PanelItem){ "door",   "开门", "面对未知" };
    out[n++] = (PanelItem){ "back",   "返回", "" };
    return n;
}

/* 注意：调用方 echo_input 已把 panel_sel 置为该项索引，此处不再重设，
 * 否则在「不明食物」项存在时（列表索引整体后移）光标会跳到错误位置。 */
static int panel_exec(EchoState *st, const char *id) {
    if (strcmp(id, "echo") == 0)   return start_echo(st);
    if (strcmp(id, "listen") == 0) return start_listen(st);
    if (strcmp(id, "door") == 0)   return open_door(st);
    if (strcmp(id, "eat") == 0)    return eat_unknown(st);
    return 0;
}

/* ==========================================================================
 * 输入
 * ========================================================================== */
int echo_input(EchoState *st, int btn, int ev) {
    if (st->ended != -1) return 0;

    if (st->phase == PHASE_PANEL) {
        PanelItem items[ECHO_MAX_PANEL_ITEMS];
        int n = echo_panel_items(st, items);
        if (ev == BTN_LONG && btn == BTN_OK) { st->phase = PHASE_IDLE; return 1; }
        if (ev == BTN_PRESS && btn == BTN_UP)   { st->panel_sel = (st->panel_sel + n - 1) % n; emit(st, EV_BLIP); return 1; }
        if (ev == BTN_PRESS && btn == BTN_DOWN) { st->panel_sel = (st->panel_sel + 1) % n; emit(st, EV_BLIP); return 1; }
        if (ev == BTN_PRESS && btn == BTN_OK) {
            int idx = st->panel_sel < n ? st->panel_sel : n - 1;
            PanelItem it = items[idx];
            st->panel_sel = idx;
            if (strcmp(it.id, "back") == 0) { st->phase = PHASE_IDLE; return 1; }
            int ok = panel_exec(st, it.id);
            if (st->phase == PHASE_PANEL) st->phase = PHASE_IDLE;
            return ok;
        }
        return 0;
    }

    if (st->phase == PHASE_IDLE) {
        if (ev == BTN_PRESS && btn == BTN_UP)   return move(st, -1);
        if (ev == BTN_PRESS && btn == BTN_DOWN) return move(st, 1);
        if (ev == BTN_PRESS && btn == BTN_OK)   { st->phase = PHASE_PANEL; emit(st, EV_PANEL); return 1; }
    }
    return 0;
}

/* ==========================================================================
 * 演出推进
 * ========================================================================== */
void echo_tick(EchoState *st, double dt_ms) {
    if (st->phase == PHASE_ECHOING) {
        st->echo_t += dt_ms;
        if (!st->echo_fired && st->echo_t >= CFG_SILENT_MS) {
            st->echo_fired = true;
            emit(st, EV_ECHO);
        }
        if (st->echo_t >= CFG_ECHO_MS) st->phase = PHASE_IDLE;
    }

    if (st->phase == PHASE_DOOR) {
        st->door_t += dt_ms;
        if (st->door_t >= CFG_DOOR_MS) {
            if (st->door_truth) {
                st->ended = END_TRUTH;
                st->phase = PHASE_OVER;
                emit(st, EV_TRUTH);
            } else {
                st->phase = PHASE_IDLE;
            }
        }
    }

    if (st->flash > 0) st->flash -= dt_ms / 1000.0;
}

/* ==========================================================================
 * 结局文案 / 结算
 * ========================================================================== */
/* 困层：停摆后开门，这一层没有能修电梯的东西，而井道之外没有别的路。
 * 含当前楼层数字，故写入静态缓冲（调用方即用，不可跨次持有）。 */
const char *echo_stranded_text(const EchoState *st) {
    static char buf[128];
    int d = st->depth < 0 ? -st->depth : st->depth;
    snprintf(buf, sizeof(buf),"门开了，外面只有黑暗。你回到轿厢，门合上。数字还亮着：B%d。但它不会再变了。", d);
    return buf;
}

/* 历史档案版：按结局编号取正文。困层不写楼层（档案跨局，具体楼层没意义）。 */
const char *echo_end_text_by_id(int end_id) {
    switch (end_id) {
        case END_TRUTH:   return truth_text();
        case END_BECAME:  return "你不再确定门外有没有东西。于是你呼喊了一声，等一个回应。";
        case END_MADNESS: return "你开始对着门说话。门外的声音也在说话，节奏和你一样。";
        case END_TAKEN:   return "门开了。你的回声从外面传回来，比你的手早了半秒。";
        case END_STARVED: return "你没有输给它，只是先一步耗尽了。电梯停在原地，门外的回声还在等你回应，可你已经没有力气再呼喊。";
        case END_STALLED: return "电梯停了。你还有力气开门，但已经没有力气走出去了。";
        case END_STRANDED: return "门开了，外面只有黑暗。你回到轿厢，门合上。数字还亮着。但它不会再变了。";
        default:          return "";
    }
}

const char *echo_end_text(const EchoState *st) {
    if (st->ended == END_STRANDED) return echo_stranded_text(st);
    if (st->ended == END_TRUTH)   return truth_text();
    if (st->ended == END_EGG)     return st->egg ? st->egg->text : "";
    if (st->ended == END_BECAME)  return "你不再确定门外有没有东西。于是你呼喊了一声，等一个回应。";
    if (st->ended == END_MADNESS) return "你开始对着门说话。门外的声音也在说话，节奏和你一样。";
    if (st->ended == END_TAKEN)   return "门开了。你的回声从外面传回来，比你的手早了半秒。";
    if (st->ended == END_STARVED) return "你没有输给它，只是先一步耗尽了。电梯停在原地，门外的回声还在等你回应，可你已经没有力气再呼喊。";
    if (st->ended == END_STALLED) return "电梯停了。你还有力气开门，但已经没有力气走出去了。";
    return "";
}

void echo_summary(const EchoState *st, LastRun *out) {
    memset(out, 0, sizeof(*out));
    out->max_depth = st->max_depth;
    out->echo_count = st->run_echo_count < ECHO_MAX_ECHO_DEPTHS ? st->run_echo_count : ECHO_MAX_ECHO_DEPTHS;
    memcpy(out->echo_depths, st->run_echoes, (size_t)out->echo_count * sizeof(int));
}

/* ==========================================================================
 * 续局存档：全量序列化 EchoState（处理 egg 指针）。
 * EchoState 除 egg 指针外均为值类型/内联数组，可直接 memcpy；
 * egg 指向静态 EggDef，故只存 depth，读档时用 echo_egg_at 还原。
 * ========================================================================== */
size_t echo_save_state(const EchoState *st, uint8_t *buf, size_t cap) {
    if (cap < ECHO_SAVE_SIZE) return 0;
    EchoSave *s = (EchoSave *)buf;
    s->magic = ECHO_SAVE_MAGIC;
    s->version = ECHO_SAVE_VERSION;
    s->egg_depth = st->egg ? st->egg->depth : -1;
    memcpy(&s->state, st, sizeof(EchoState));
    return ECHO_SAVE_SIZE;
}

bool echo_load_state(EchoState *st, const uint8_t *buf, size_t len) {
    if (len < ECHO_SAVE_SIZE) return false;
    const EchoSave *s = (const EchoSave *)buf;
    if (s->magic != ECHO_SAVE_MAGIC || s->version != ECHO_SAVE_VERSION) return false;
    memcpy(st, &s->state, sizeof(EchoState));
    st->egg = (s->egg_depth >= 0) ? echo_egg_at(s->egg_depth) : NULL;
    st->ev_count = 0;   /* 丢弃残留事件，避免读档即播放旧音效 */
    return true;
}
