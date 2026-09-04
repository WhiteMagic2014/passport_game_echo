/*
 * echo_core.h —— 《回声·下沉》纯逻辑状态机（C 版）。
 *
 * 由 prototype/js/core.js 逐行翻译而来。与 JS 版保持一致的关键点：
 *   - 回合驱动：只有「改变世界的动作」才推进回合（endTurn），演出（tick）只驱动画面。
 *   - 伪随机（hash32 / rand01）用 uint32_t 全程运算，与 JS 的 Math.imul / ToInt32 语义
 *     逐位一致，保证楼层生成、彩蛋触发等随机分布与主机版可复现对齐。
 *   - 数值用 double，与 JS number 一致（0.3 / 1.1 / 2.3 等小数不丢精度）。
 *
 * 本文件不依赖任何 ESP-IDF / BSP 头文件，仅依赖 stdint / stdbool，
 * 可在主机上用 gcc/clang 编译运行测试（tests/test_core.c）。
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
#define CFG_FOOD_MAX          100.0
#define CFG_DANGER_MAX        100.0
#define CFG_DANGER_HUNT       50.0

#define CFG_WEAR_DOWN         2.3
#define CFG_WEAR_UP           1.0
#define CFG_WEAR_DOOR         1.0

#define CFG_FOOD_MOVE         1.5
#define CFG_FOOD_ECHO         1.0
#define CFG_FOOD_DOOR         2.5
#define CFG_FOOD_LISTEN       1.0

#define CFG_DANGER_MOVE       2.0
#define CFG_DANGER_ECHO       4.0
#define CFG_DANGER_DOOR       6.0
#define CFG_DANGER_LISTEN     12.0
#define CFG_DANGER_SILENT     6.0

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

#define CFG_REPAIR_BASE       45.0
#define CFG_REPAIR_DECAY      0.3
#define CFG_REPAIR_MIN        0.3

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
#define FRAGMENT_DEPTHS       { 5, 12, 20, 30, 42, 55 }

/* 状态结构规模上限 */
#define ECHO_MAX_EVENTS       16     /* 单回合事件槽 */
#define ECHO_MAX_ECHO_DEPTHS  256    /* 本局呼喊足迹上限 */
#define ECHO_MSG_LEN          192    /* 消息缓冲（UTF-8 字节） */
#define ECHO_FLOOR_OFFSET     128    /* depth + OFFSET = 数组下标 */
#define ECHO_FLOOR_SLOTS      512    /* 覆盖 depth ∈ [-128, 383] */
#define ECHO_MAX_PANEL_ITEMS  8

/* ==========================================================================
 * 枚举
 * ========================================================================== */
/* 楼层类型。已移除 FT_STAIRS：本作只能靠电梯上下，不设楼梯。
 * 注意：枚举值变更会导致旧存档的楼层类型错位，故 ECHO_SAVE_VERSION 已升至 2。 */
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
    END_STALLED = 0, END_MADNESS, END_TAKEN, END_BECAME, END_EGG, END_TRUTH, END_STARVED
} end_t;

/* 事件（对应 JS emit 的字符串）。带音效的由音频层消费，其余保留供调试/扩展。 */
typedef enum {
    EV_NONE = 0,
    EV_KNOCK, EV_ECHO, EV_MOVE_UP, EV_MOVE_DOWN, EV_PANEL, EV_BLIP,
    EV_ENCOUNTER, EV_BREATH, EV_LISTEN, EV_SILENT_ECHO, EV_AMBIENCE_CUT,
    EV_AHEAD, EV_DOOR, EV_EGG, EV_TRUTH, EV_REPAIR,
    EV_MEMORY_LIE, EV_HOPE, EV_LOOT, EV_FRAGMENT, EV_UNKNOWN_FOOD, EV_ATE,
    EV_EMPTY
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
    uint8_t flags;   /* bit0 hope, bit1 loot, bit2 tool, bit3 unknown_food,
                        bit4 fragment, bit5 seen */
} Floor;

enum {
    FLOOR_F_HOPE = 1 << 0,
    FLOOR_F_LOOT = 1 << 1,
    FLOOR_F_TOOL = 1 << 2,
    FLOOR_F_UNKNOWN_FOOD = 1 << 3,
    FLOOR_F_FRAGMENT = 1 << 4,
    FLOOR_F_SEEN = 1 << 5,
    FLOOR_F_GENERATED = 1 << 6
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

    /* 它（恐惧实体） */
    int fear_pos;        /* 初始 -25 */
    int fear_mode;       /* 0=wander 1=hunt */
    bool fear_here;

    int phase;           /* phase_t */
    int panel_sel;
    int ended;           /* end_t，-1 表示未结束 */
    const EggDef *egg;   /* 触发的彩蛋（END_EGG 时指向静态 EggDef） */

    /* 回声演出 */
    double echo_t;
    int echo_sig;
    int echo_depth;
    bool echo_fired;

    /* 开门演出 */
    double door_t;
    DoorResult door_result;

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

/* 彩蛋查询：返回指向静态 EggDef 的指针，无则 NULL。 */
const EggDef *echo_egg_at(int depth);

/* ==========================================================================
 * 续局存档：全量序列化 EchoState（处理 egg 指针）。
 * 仅用于「继续上一局」，与 LastRun（跨局足迹）是两套不同的存档。
 * ========================================================================== */
#define ECHO_SAVE_MAGIC    0x4543484Fu   /* "ECHO" */
/* v2：移除楼梯层（FT_STAIRS）并调整楼层类型枚举值，v1 存档已不兼容，一律拒绝。 */
#define ECHO_SAVE_VERSION  2

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
