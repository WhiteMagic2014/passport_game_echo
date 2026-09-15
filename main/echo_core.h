/*
 * echo_core.h —— 《回声·下沉》纯逻辑状态机（C 版）。
 *
 * 由 prototype/js/core.js 逐行翻译而来。与 JS 版保持一致的关键点：
 *   - 回合驱动：只有「改变世界的动作」才推进回合（endTurn），演出（tick）只驱动画面。
 *   - 伪随机（hash32 / rand01）用 uint32_t 全程运算，与 JS 的 Math.imul / ToInt32 语义
 *     逐位一致，保证楼层生成、彩蛋触发等随机分布与主机版可复现对齐。
 *   - 数值用 double，与 JS number 一致（0.3 / 1.1 / 2.3 等小数不丢精度）。
 *
 * 本文件不依赖任何 ESP-IDF / BSP 头文件，仅依赖 stdint / stdbool / math，
 * 可在主机上用 gcc/clang 编译运行测试。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 配置（与 core.js CFG 一一对应）
 * ========================================================================== */
#define CFG_WEAR_MAX          100.0
#define CFG_SANITY_MAX        100.0
#define CFG_FOOD_MAX          40.0    /* 携带上限（背包容量，不可无限囤积） */
#define CFG_FOOD_START        28.0    /* 初始携带（约 18 次移动的余量，逼你早开门） */
#define CFG_FOOD_CAP          40.0    /* 拾取食物的上限（与 FOOD_MAX 同义） */
#define CFG_FOOD_LOOT         26.0    /* 一次搜刮的补给量（低于上限，囤不满） */
#define CFG_DANGER_MAX        100.0   /* 危险读数上限（由怪物距离推导，不再手动累加） */

#define CFG_WEAR_DOWN         2.3
#define CFG_WEAR_UP           1.0
#define CFG_WEAR_DOOR         1.0

#define CFG_FOOD_MOVE         1.5
#define CFG_FOOD_ECHO         1.0
#define CFG_FOOD_DOOR         2.5
#define CFG_FOOD_LISTEN       1.0

/* 怪物 / 危险 */
#define CFG_MONSTER_COUNT         1       /* 同局怪物数量 */
#define CFG_MONSTER_SPAWN_MIN     12      /* 出生楼层下限（不会一开局就贴脸） */
#define CFG_MONSTER_SPAWN_MAX     95      /* 出生楼层上限 */
#define CFG_HOMING_MIN            1       /* 被标记后，每回合朝信标移动的最小层数 */
#define CFG_HOMING_MAX            3       /* 最大层数（速度随机 1~3，HTML 推荐甜点） */
#define CFG_WANDER_STEP           0.45    /* 游荡时移动（随机 ±1）的概率，其余不动 */
#define CFG_DANGER_RADIUS         20      /* 危险读数归零的距离（层）；越近危险越高 */

#define CFG_SANITY_FED        0.3
#define CFG_SANITY_HUNGRY     1.1
#define CFG_SANITY_BREATH     4.0
#define CFG_SANITY_AHEAD      3.0
#define CFG_SANITY_UNSEEN     40.0
#define CFG_SANITY_HOPE       26.0

#define CFG_HEALTH_MAX        100.0
#define CFG_HEALTH_HIT_IT_MIN 22
#define CFG_HEALTH_HIT_IT_MAX 32
#define CFG_HEALTH_HIT_ANOMALY_MIN 12
#define CFG_HEALTH_HIT_ANOMALY_MAX 20
#define CFG_HEALTH_HUNGRY     0.3
#define CFG_HEALTH_LOW        30.0
#define CFG_SANITY_LOW_MULT   2.0

/* LIE_BIAS 在 JS 版中定义但未被引用，此处保留以对齐配置表（勿删，便于对照）。 */
#define CFG_LIE_BIAS          0.72

#define CFG_SANITY_LIE        55.0
#define CFG_SANITY_LIE_HARD   25.0

/* 演出时长（毫秒）——只驱动画面/音效，不驱动逻辑 */
#define CFG_ECHO_MS           3400.0
#define CFG_SILENT_MS         1000.0
#define CFG_DOOR_MS           2600.0
#define CFG_PANEL_LONG_MS     450

/* 修理：每次修复按几何递减（保底 12 点磨损 = 12% 耐久度）。
 *   第 1..n 次 = 45 × 0.72^(n-1)：45 → 32.4 → 23.3 → 16.8 → 12.1 → 到底 12。
 *   触底后固定回 12%，并在文案追加「再修也只能这样了」。 */
#define CFG_REPAIR_BASE       45.0
#define CFG_REPAIR_DECAY      0.72
#define CFG_REPAIR_FLOOR      12.0

#define CFG_EGG_CHANCE        0.08

/* 恐怖手法的回合冷却 [min, max] */
#define CFG_TRICK_SILENT_MIN  5
#define CFG_TRICK_SILENT_MAX  10
#define CFG_TRICK_CUT_MIN     7
#define CFG_TRICK_CUT_MAX     13
#define CFG_TRICK_AHEAD_MIN   8
#define CFG_TRICK_AHEAD_MAX   15
#define CFG_TRICK_DEPTH_SILENT 5
#define CFG_TRICK_DEPTH_CUT   8
#define CFG_TRICK_DEPTH_AHEAD 6

#define FRAGMENT_DEPTH_COUNT  6
/* 碎片层固定基准：仅作旧存档兜底，实际使用 st.fragment_depths（每局纯随机）。 */
#define FRAGMENT_DEPTHS       { 5, 12, 20, 30, 42, 55 }
#define FRAGMENT_MIN_DEPTH    10   /* 碎片层随机区间（含） */
#define FRAGMENT_MAX_DEPTH    100  /* 碎片层随机区间（含） */

/* 状态结构规模上限 */
#define ECHO_MAX_EVENTS       16     /* 单回合事件槽 */
#define ECHO_MAX_ECHO_DEPTHS  256    /* 本局呼喊足迹上限 */
#define ECHO_MSG_LEN          192    /* 消息缓冲（UTF-8 字节） */
#define ECHO_FLOOR_OFFSET     128    /* depth + OFFSET = 数组下标 */
#define ECHO_FLOOR_SLOTS      512    /* 覆盖 depth ∈ [-128, 383] */
#define ECHO_MAX_PANEL_ITEMS  8
#define ECHO_MAX_MONSTERS     4      /* 同局怪物数量上限（当前 CFG_MONSTER_COUNT=1） */

/* ==========================================================================
 * 枚举
 * ========================================================================== */
/* 楼层类型。已移除 FT_STAIRS：本作只能靠电梯上下，不设楼梯。
 * 注意：枚举值变更会导致旧存档的楼层类型错位，故 ECHO_SAVE_VERSION 已升至 3。 */
typedef enum {
    FT_EMPTY = 0, FT_CLUTTER, FT_POWER, FT_MEMORY, FT_ANOMALY
} floor_type_t;

typedef enum {
    ECHO_OPEN = 0, ECHO_BLOCKED, ECHO_ALIVE, ECHO_ANSWERED
} echo_sig_t;

typedef enum {
    PHASE_IDLE = 0, PHASE_PANEL, PHASE_ECHOING, PHASE_DOOR, PHASE_OVER
} phase_t;

typedef enum {
    END_STALLED = 0, END_MADNESS, END_TAKEN, END_BECAME, END_EGG, END_TRUTH, END_STARVED,
    END_STRANDED      /* 困层：停摆后开门，这一层没有能修它的东西 */
} end_t;
#define END_COUNT 8

/* 事件（对应 JS emit 的字符串）。带音效的由音频层消费，其余保留供调试/扩展。 */
typedef enum {
    EV_NONE = 0,
    EV_KNOCK, EV_ECHO, EV_MOVE_UP, EV_MOVE_DOWN, EV_PANEL, EV_BLIP,
    EV_ENCOUNTER, EV_BREATH, EV_LISTEN, EV_SILENT_ECHO, EV_AMBIENCE_CUT,
    EV_AHEAD, EV_DOOR, EV_EGG, EV_TRUTH, EV_REPAIR,
    EV_MEMORY_LIE, EV_HOPE, EV_LOOT, EV_FRAGMENT, EV_UNKNOWN_FOOD, EV_ATE,
    EV_EMPTY, EV_STRANDED
} event_t;

/* 按键：0=上 1=下 2=确认（与 BSP_BTN_UP/DOWN/OK 对齐） */
typedef enum {
    BTN_UP = 0, BTN_DOWN, BTN_OK
} btn_t;

/* 按键事件：0=按下 1=长按（输入层仅需这两档，单击/双击由 BSP 层决定语义） */
typedef enum {
    BTN_PRESS = 0, BTN_LONG
} btn_ev_t;

/* ==========================================================================
 * 数据结构
 * ========================================================================== */
/* 楼层：惰性生成后缓存。flags 用位标志省内存（ESP32-C3 无 PSRAM）。 */
typedef struct {
    uint8_t type;    /* floor_type_t */
    uint16_t flags;  /* 见下方 FLOOR_F_*（bit8 claimed 需要 16 位） */
} Floor;

enum {
    FLOOR_F_HOPE = 1 << 0,
    FLOOR_F_LOOT = 1 << 1,
    FLOOR_F_TOOL = 1 << 2,
    FLOOR_F_UNKNOWN_FOOD = 1 << 3,
    FLOOR_F_FRAGMENT = 1 << 4,
    FLOOR_F_SEEN = 1 << 5,
    FLOOR_F_GENERATED = 1 << 6,
    FLOOR_F_FRAGMENT_TAKEN = 1 << 7,  /* 碎片是否已被取走（用于聆听线索消失） */
    FLOOR_F_CLAIMED = 1 << 8          /* 本层奖励已被取过一次（再开只有风） */
};

/* 彩蛋结局（静态表，按 depth 查询） */
typedef struct {
    int depth;
    const char *title;
    const char *text;
} EggDef;

/* 上局足迹（跨局持久化，NVS 存档） */
typedef struct {
    int max_depth;
    int echo_depths[ECHO_MAX_ECHO_DEPTHS];
    int echo_count;
} LastRun;

/* 面板项 */
typedef struct {
    const char *id;      /* "echo" / "listen" / "door" / "eat" / "back" */
    const char *label;
    const char *hint;
} PanelItem;

/* 开门结果（供平衡探针读取 type / 是否有物品；核心逻辑用 pending_unknown） */
typedef struct {
    floor_type_t type;
    int got_count;
} DoorResult;

/* 核心状态 */
typedef struct {
    int32_t seed;
    int depth;
    double food, sanity, health, wear, danger;

    int repairs, fragments, max_depth;
    int enc, doors, echoes, turn;
    bool ate_unknown, pending_unknown;

    /* 怪物（「它」）：默认 1 只，开局刷新在随机楼层 */
    int monsters[ECHO_MAX_MONSTERS];
    int monster_count;
    bool marked;          /* 是否被呼喊标记过信标 */
    int mark_floor;       /* 信标所在层（= 某次呼喊时的玩家层） */
    bool fear_here;       /* 最近怪物是否就在此层 */

    /* 本局碎片层（每局纯随机，升序） */
    int fragment_depths[FRAGMENT_DEPTH_COUNT];

    int phase;           /* phase_t */
    int panel_sel;
    int ended;           /* end_t，-1 表示未结束 */
    const EggDef *egg;   /* 触发的彩蛋（END_EGG 时指向静态 EggDef） */
    int egg_floor;       /* 触发彩蛋时的楼层（历史档案按楼层分别记录） */

    /* 回声演出 */
    double echo_t;
    int echo_sig;
    int echo_depth;
    bool echo_fired;

    /* 开门演出 */
    double door_t;
    DoorResult door_result;
    bool door_truth;     /* 本次开门触发了真结局（tick 收尾时转入 OVER） */

    /* 恐怖手法回合冷却 */
    int trick_silent, trick_cut, trick_ahead;

    /* 本局呼喊足迹 */
    int run_echoes[ECHO_MAX_ECHO_DEPTHS];
    int run_echo_count;

    /* 上局足迹（只读输入，ghostWeight 用） */
    LastRun last_run;
    bool has_last_run;

    /* 消息文本（UTF-8，含格式化后的结果） */
    char msg[ECHO_MSG_LEN];

    /* 事件队列 */
    event_t events[ECHO_MAX_EVENTS];
    int ev_count;

    /* 楼层缓存 */
    Floor floors[ECHO_FLOOR_SLOTS];

    /* 伪随机内部状态 */
    uint32_t rng;

    /* 演出闪烁（>0 时渲染做一次高亮） */
    double flash;
} EchoState;

/* ==========================================================================
 * 公开 API（对应 JS 导出）
 * ========================================================================== */
void echo_new_game(EchoState *st, int32_t seed, const LastRun *last);

/* 演出推进：只驱动画面/音效，不碰逻辑状态。dtMs 为毫秒。 */
void echo_tick(EchoState *st, double dt_ms);

/* 输入：btn 0=up 1=down 2=ok；ev 0=press 1=long。返回 1=已处理 0=忽略。 */
int echo_input(EchoState *st, int btn, int ev);

/* 面板项列表，返回项数，写入 out（容量 ECHO_MAX_PANEL_ITEMS）。 */
int echo_panel_items(const EchoState *st, PanelItem *out);

/* 取某层（惰性生成）。depth 越界返回 NULL。 */
const Floor *echo_floor_at(EchoState *st, int depth);

/* 回声签名（会写入 seen / 触发失真）。 */
int echo_compute_sig(EchoState *st, int depth);

const char *echo_wear_line(const EchoState *st);
bool echo_is_stalled(const EchoState *st);
const char *echo_end_text(const EchoState *st);
void echo_summary(const EchoState *st, LastRun *out);
double echo_lie_chance(const EchoState *st);
double echo_ghost_weight(const EchoState *st, int depth);
uint32_t echo_hash32(uint32_t a, uint32_t b);

/* 怪物查询（供主循环触发脚步音效等集成使用） */
int echo_nearest_pos(const EchoState *st);
int echo_nearest_dist(const EchoState *st);
bool echo_monster_here(const EchoState *st);

/* 彩蛋查询：返回指向静态 EggDef 的指针，无则 NULL。 */
const EggDef *echo_egg_at(int depth);

/* 彩蛋表（历史档案用）：按楼层升序，索引 0..EGG_COUNT-1。越界返回 NULL。 */
#define EGG_COUNT 6
const EggDef *echo_egg_by_index(int i);
/* 彩蛋楼层 → 索引，非彩蛋楼层返回 -1。 */
int echo_egg_index_of(int depth);
/* 结局标题表（按 end_t 下标，END_COUNT 条），供历史档案列出未解锁条目名。 */
const char *const *echo_end_titles(void);
/* 第 idx 片碎片的正文（0..FRAGMENT_DEPTH_COUNT-1），历史档案详情用。 */
const char *echo_fragment_text(int idx);
/* 困层结局正文（含当前楼层，写入内部静态缓冲，返回后即用） */
const char *echo_stranded_text(const EchoState *st);
/* 按结局编号取正文（END_COUNT 条，历史档案用；END_EGG 返回 ""，
 * 困层为不含楼层的通用版——档案跨局，具体楼层没有意义）。 */
const char *echo_end_text_by_id(int end_id);

/* ==========================================================================
 * 续局存档：全量序列化 EchoState（处理 egg 指针）。
 * 仅用于「继续上一局」，与 LastRun（跨局足迹）是两套不同的存档。
 * ========================================================================== */
#define ECHO_SAVE_MAGIC    0x4543484Fu   /* "ECHO" */
/* v4：Floor.flags 扩到 16 位（新增 claimed 位）、新增 END_STRANDED / egg_floor、
 *     食物数值改为 START 28 / CAP 40 / LOOT 26。v3 及以下存档一律拒绝。 */
#define ECHO_SAVE_VERSION  4

typedef struct {
    uint32_t magic;
    uint32_t version;
    int egg_depth;         /* 触发彩蛋时存 depth，否则 -1 */
    EchoState state;
} EchoSave;

#define ECHO_SAVE_SIZE ((size_t)sizeof(EchoSave))

/* 序列化到 buf（容量 cap），返回写入字节数；空间不足返回 0。 */
size_t echo_save_state(const EchoState *st, uint8_t *buf, size_t cap);

/* 从 buf 反序列化，成功返回 true。 */
bool echo_load_state(EchoState *st, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
