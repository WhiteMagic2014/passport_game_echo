/*
 * echo_core.c —— 《回声·下沉》纯逻辑状态机实现。
 * 由 prototype/js/core.js 逐行翻译。详见 echo_core.h 顶部说明。
 */
#include "echo_core.h"
#include <string.h>
#include <stdio.h>

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
#define EGG_COUNT (sizeof(EGGS) / sizeof(EGGS[0]))

const EggDef *echo_egg_at(int depth) {
    for (size_t i = 0; i < EGG_COUNT; i++) {
        if (EGGS[i].depth == depth) return &EGGS[i];
    }
    return NULL;
}

static const char *fragment_text(int depth) {
    switch (depth) {
        case 5:  return "一张住户留言：「最近井道里总有回声，物业说是风。」";
        case 12: return "失踪报告：「最后监控拍到她进了电梯。电梯没停过。」";
        case 20: return "维修记录，字迹发抖：「钢缆换过七次。它不让我们修。」";
        case 30: return "一张纸，只写了一行：「第 47 个。」";
        case 42: return "录音笔还有电。按下播放——是你自己刚才呼喊的那三声。";
        case 55: return "一张照片。电梯里的人抬头看着镜头。是你，但衣服不是你的。";
        default: return "又一张纸，什么都没写。";
    }
}

/* ==========================================================================
 * 消息辅助
 * ========================================================================== */
static void set_msg(EchoState *st, const char *s) {
    snprintf(st->msg, sizeof(st->msg), "%s", s);
}

/* ==========================================================================
 * 楼层生成
 * ========================================================================== */
static bool is_frag_depth(int depth) {
    for (int i = 0; i < FRAGMENT_DEPTH_COUNT; i++) {
        if (FRAGMENT_DEPTHS_TABLE[i] == depth) return true;
    }
    return false;
}

static bool floor_flag(const Floor *f, int bit) { return (f->flags & bit) != 0; }
static void floor_set(Floor *f, int bit, bool v) {
    if (v) f->flags = (uint8_t)(f->flags | bit);
    else   f->flags = (uint8_t)(f->flags & ~bit);
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

    bool frag = is_frag_depth(depth);
    f->type = (uint8_t)(frag ? FT_MEMORY : type);
    f->flags = 0;
    floor_set(f, FLOOR_F_FRAGMENT, frag);
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
 * 新建一局
 * ========================================================================== */
void echo_new_game(EchoState *st, int32_t seed, const LastRun *last) {
    memset(st, 0, sizeof(*st));
    st->seed = (seed != 0) ? seed : 1;
    st->depth = 0;
    st->food = 60.0;
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

    st->fear_pos = -25;
    st->fear_mode = 0;
    st->fear_here = false;

    st->phase = PHASE_IDLE;
    st->panel_sel = 0;
    st->pending_unknown = false;
    st->ended = -1;

    st->echo_t = 0.0;
    st->echo_sig = -1;
    st->echo_depth = -99;
    st->echo_fired = false;

    st->door_t = 0.0;
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
    if (st->fear_pos == depth) sig = ECHO_ALIVE;
    else if (f->type == FT_ANOMALY) sig = ECHO_ALIVE;
    else if (f->type == FT_CLUTTER) sig = ECHO_BLOCKED;
    else sig = ECHO_OPEN;

    bool was_seen = floor_flag(f, FLOOR_F_SEEN);
    floor_set(f, FLOOR_F_SEEN, true);

    if (sig == ECHO_ALIVE && (st->fear_pos - depth >= -1 && st->fear_pos - depth <= 1)) {
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
 * 它的移动
 * ========================================================================== */
static void fear_step(EchoState *st) {
    st->fear_mode = (st->danger >= CFG_DANGER_HUNT) ? 1 : 0;
    if (st->fear_mode == 1) {
        if (st->fear_pos < st->depth) st->fear_pos += 1;
        else if (st->fear_pos > st->depth) st->fear_pos -= 1;
    } else {
        double r = rand01(st);
        if (r < 0.40) {
            if (st->fear_pos < st->depth) st->fear_pos += 1;
            else if (st->fear_pos > st->depth) st->fear_pos -= 1;
        } else if (r < 0.75) {
            st->fear_pos += (rand01(st) < 0.5 ? 1 : -1);
        }
    }
    if (st->fear_pos < -2) st->fear_pos = -2;
    if (st->fear_pos == st->depth) {
        if (!st->fear_here) {
            st->fear_here = true;
            st->sanity -= CFG_SANITY_BREATH;
            set_msg(st, "门外的脚步停下了。");
            emit(st, EV_BREATH);
        }
    } else {
        st->fear_here = false;
    }
}

/* ==========================================================================
 * 恐怖手法
 * ========================================================================== */
static void advance_tricks(EchoState *st) {
    int d = st->depth;

    st->trick_silent--;
    if (st->trick_silent <= 0) {
        st->trick_silent = rand_int(st, CFG_TRICK_SILENT_MIN, CFG_TRICK_SILENT_MAX);
        if (d >= CFG_TRICK_DEPTH_SILENT) {
            emit(st, EV_SILENT_ECHO);
            st->danger += CFG_DANGER_SILENT;
            if (st->danger > CFG_DANGER_MAX) st->danger = CFG_DANGER_MAX;
        }
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
            int above = d - st->fear_pos;
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

    fear_step(st);

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

    st->danger += CFG_DANGER_MOVE;
    if (st->danger > CFG_DANGER_MAX) st->danger = CFG_DANGER_MAX;

    set_msg(st, echo_wear_line(st));
    emit(st, dir > 0 ? EV_MOVE_DOWN : EV_MOVE_UP);
    floor_at(st, st->depth);

    end_turn(st);
    return 1;
}

/* 真结局（好结局）：集齐碎片后你终于「听得懂回声」，回声反过来替你指路，
 * 电梯第一次向上，你走出了这栋楼。
 * 控制在 45 字以内：结局区从 y=136 起、每行 15 字，3 行恰好在 y=196 的结算行之前结束。 */
static const char *truth_text(void) {
    return "你呼喊了三声。回声替你指了路——向上。门开了，外面是有风的清晨。你走了出去，没有回头。";
}

static int start_echo(EchoState *st) {
    if (!can_act(st)) return 0;
    /* 集齐全部碎片后，呼喊揭示真相，打破循环 */
    if (st->fragments >= FRAGMENT_DEPTH_COUNT) {
        st->ended = END_TRUTH;
        st->phase = PHASE_OVER;
        st->flash = 1.0;
        set_msg(st, truth_text());
        emit(st, EV_TRUTH);
        return 1;
    }
    st->phase = PHASE_ECHOING;
    st->echo_t = 0.0;
    st->echo_fired = false;
    st->echo_depth = st->depth;
    st->echo_sig = echo_compute_sig(st, st->depth);

    st->food -= CFG_FOOD_ECHO;
    if (st->food < 0) st->food = 0;
    st->danger += CFG_DANGER_ECHO;
    if (st->danger > CFG_DANGER_MAX) st->danger = CFG_DANGER_MAX;

    if (st->run_echo_count < ECHO_MAX_ECHO_DEPTHS) {
        st->run_echoes[st->run_echo_count++] = st->depth;
    }
    st->echoes++;

    emit(st, EV_KNOCK);
    end_turn(st);
    return 1;
}

/* 聆听结果：只分「很远 / 远处 / 很近 / 就在门外」四档，不给出相隔层数。
 * 近距离那一档连上下都不透露——玩家无法靠反复聆听精确定位它。
 * （原先细分为 >8 / >4 / >1 / ==1 / ==0 五档，区间过窄，等于报出了大致层数。） */
static const char *describe_listen(const EchoState *st) {
    int d = st->fear_pos - st->depth;
    int ad = d < 0 ? -d : d;
    if (ad >= 8) return "井道深处很安静。安静得像是有什么在屏息";
    if (ad >= 3) return (d > 0) ? "远处有东西在动，声音贴着井壁往下走"
                                : "远处有东西在动，声音贴着井壁往上走";
    if (ad >= 1) return "很近了。近到你分不清它究竟在上还是在下";
    return "声音就在门外面";
}

static int start_listen(EchoState *st) {
    if (!can_act(st)) return 0;
    st->food -= CFG_FOOD_LISTEN;
    if (st->food < 0) st->food = 0;
    st->danger -= CFG_DANGER_LISTEN;
    if (st->danger < 0) st->danger = 0;
    set_msg(st, describe_listen(st));
    emit(st, EV_LISTEN);
    end_turn(st);
    return 1;
}

static int open_door(EchoState *st) {
    st->phase = PHASE_DOOR;
    st->door_t = 0.0;
    st->doors++;

    st->wear += CFG_WEAR_DOOR;
    if (st->wear > CFG_WEAR_MAX) st->wear = CFG_WEAR_MAX;
    st->food -= CFG_FOOD_DOOR;
    if (st->food < 0) st->food = 0;
    st->danger += CFG_DANGER_DOOR;
    if (st->danger > CFG_DANGER_MAX) st->danger = CFG_DANGER_MAX;

    Floor *f = floor_at(st, st->depth);
    if (!f) { end_turn(st); return 0; }

    floor_type_t ft = (floor_type_t)f->type;
    int got = 0;

    if (st->fear_pos == st->depth || ft == FT_ANOMALY) {
        bool it_here = (st->fear_pos == st->depth);
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
        set_msg(st, st->egg->text);
        got = 1;
        emit(st, EV_EGG);
    } else if (floor_flag(f, FLOOR_F_TOOL)) {
        double eff = CFG_REPAIR_MIN;
        double e = 1.0 - st->repairs * CFG_REPAIR_DECAY;
        if (e > eff) eff = e;
        double fix = CFG_REPAIR_BASE * eff;
        st->wear -= fix;
        if (st->wear < 0) st->wear = 0;
        st->repairs++;
        static const char *rep_fmt[] = {
            "你找到一些工具，尝试加固了电梯（第 %d 次修复，效果 %d%%）",
            "你摸到一些工具。你加固了电梯。第 %d 次修复，效果 %d%%。",
            "你找到一些工具。电梯又稳了一些。第 %d 次。"
        };
        int ri = (int)(rand01(st) * 3) % 3;
        snprintf(st->msg, sizeof(st->msg), rep_fmt[ri],
                 st->repairs, (int)(eff * 100.0 + 0.5));
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
        got = 1;
        emit(st, EV_UNKNOWN_FOOD);
    } else if (floor_flag(f, FLOOR_F_FRAGMENT)) {
        st->fragments++;
        if (st->fragments >= FRAGMENT_DEPTH_COUNT) {
            snprintf(st->msg, sizeof(st->msg), "%s 六张碎片拼齐了。你现在听得懂回声了。", fragment_text(st->depth));
        } else {
            set_msg(st, fragment_text(st->depth));
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
        got = 1;
        emit(st, EV_HOPE);
    } else if (floor_flag(f, FLOOR_F_LOOT)) {
        st->food += 40.0;
        if (st->food > CFG_FOOD_MAX) st->food = CFG_FOOD_MAX;
        static const char *loot_msgs[] = {
            "你摸到几包还没过期的东西",
            "你找到一些吃的。还能吃。",
            "角落里有一份东西。是食物。"
        };
        set_msg(st, loot_msgs[(int)(rand01(st) * 3) % 3]);
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

    st->door_result.type = ft;
    st->door_result.got_count = got;

    end_turn(st);
    return 1;
}

static int eat_unknown(EchoState *st) {
    if (!st->pending_unknown) return 0;
    st->pending_unknown = false;
    st->ate_unknown = true;
    st->food = CFG_FOOD_MAX;
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
    bool truth_ready = st->fragments >= FRAGMENT_DEPTH_COUNT;
    int n = 0;

    out[n++] = (PanelItem){ "echo",   "呼喊", truth_ready ? "打破循环" : "呼喊，听回声" };
    if (st->pending_unknown) {
        out[n++] = (PanelItem){ "eat", "不明食物", "恢复食物失去理智" };
    }
    out[n++] = (PanelItem){ "listen", "聆听", "降危险，听得清" };
    out[n++] = (PanelItem){ "door",   "开门", "不可逆" };
    out[n++] = (PanelItem){ "back",   "返回", "关闭本面板" };
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
        if (st->door_t >= CFG_DOOR_MS) st->phase = PHASE_IDLE;
    }

    if (st->flash > 0) st->flash -= dt_ms / 1000.0;
}

/* ==========================================================================
 * 结局文案 / 结算
 * ========================================================================== */
const char *echo_end_text(const EchoState *st) {
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
