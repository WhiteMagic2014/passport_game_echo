/*
 * sfx_table.h —— 预生成音效的 flash 索引表。
 *
 * EMBED_FILES 生成的符号名基于文件名（不含目录前缀）：
 *   sfx_data/door.raw → _binary_door_raw_start / _end
 *
 * 每个音效是 16kHz / 16bit / mono 裸 PCM。
 * 枚举顺序与 echo_audio.h 的 sfx_t 完全对齐，可直接用 sfx_t 值做下标。
 */
#ifndef SFX_TABLE_H
#define SFX_TABLE_H

#include <stdint.h>
#include <stdbool.h>

/* 与 echo_audio.h 的 sfx_t 枚举对齐，尾部追加 AMBIENCE */
typedef enum {
    SFXF_KNOCK = 0,
    SFXF_KNOCK_SOFT,
    SFXF_ECHO_OPEN,
    SFXF_ECHO_BLOCKED,
    SFXF_ECHO_ALIVE,
    SFXF_ECHO_ANSWERED,
    SFXF_FOOTSTEP,
    SFXF_ELEV_UP,
    SFXF_ELEV_DOWN,
    SFXF_DOOR,
    SFXF_BLIP,
    SFXF_BREATH,
    SFXF_STINGER,
    SFXF_TRUTH,
    SFXF_AMBIENCE,
    SFXF_COUNT
} sfxf_t;

typedef struct {
    const uint8_t *data;
    uint32_t       size;
} sfx_entry_t;

/* EMBED_FILES 生成的符号声明（文件名不含目录前缀） */
#define SFX_DECL(name) \
    extern const char _binary_##name##_raw_start[]; \
    extern const char _binary_##name##_raw_end[]

SFX_DECL(knock);
SFX_DECL(knock_soft);
SFX_DECL(echo_open);
SFX_DECL(echo_blocked);
SFX_DECL(echo_alive);
SFX_DECL(echo_answered);
SFX_DECL(elev_up);
SFX_DECL(elev_down);
SFX_DECL(door);
SFX_DECL(blip);
SFX_DECL(breath);
SFX_DECL(stinger);
SFX_DECL(truth);
SFX_DECL(footstep);
SFX_DECL(ambience);

/* 运行时初始化的音效表（首次调用时填充） */
static const sfx_entry_t *sfx_table(void) {
    static sfx_entry_t tbl[SFXF_COUNT];
    static bool inited = false;
    if (!inited) {
        tbl[SFXF_KNOCK].data         = (const void *)_binary_knock_raw_start;
        tbl[SFXF_KNOCK].size         = _binary_knock_raw_end - _binary_knock_raw_start;
        tbl[SFXF_KNOCK_SOFT].data    = (const void *)_binary_knock_soft_raw_start;
        tbl[SFXF_KNOCK_SOFT].size    = _binary_knock_soft_raw_end - _binary_knock_soft_raw_start;
        tbl[SFXF_ECHO_OPEN].data     = (const void *)_binary_echo_open_raw_start;
        tbl[SFXF_ECHO_OPEN].size     = _binary_echo_open_raw_end - _binary_echo_open_raw_start;
        tbl[SFXF_ECHO_BLOCKED].data  = (const void *)_binary_echo_blocked_raw_start;
        tbl[SFXF_ECHO_BLOCKED].size  = _binary_echo_blocked_raw_end - _binary_echo_blocked_raw_start;
        tbl[SFXF_ECHO_ALIVE].data    = (const void *)_binary_echo_alive_raw_start;
        tbl[SFXF_ECHO_ALIVE].size    = _binary_echo_alive_raw_end - _binary_echo_alive_raw_start;
        tbl[SFXF_ECHO_ANSWERED].data = (const void *)_binary_echo_answered_raw_start;
        tbl[SFXF_ECHO_ANSWERED].size = _binary_echo_answered_raw_end - _binary_echo_answered_raw_start;
        tbl[SFXF_FOOTSTEP].data      = (const void *)_binary_footstep_raw_start;
        tbl[SFXF_FOOTSTEP].size      = _binary_footstep_raw_end - _binary_footstep_raw_start;
        tbl[SFXF_ELEV_UP].data       = (const void *)_binary_elev_up_raw_start;
        tbl[SFXF_ELEV_UP].size       = _binary_elev_up_raw_end - _binary_elev_up_raw_start;
        tbl[SFXF_ELEV_DOWN].data     = (const void *)_binary_elev_down_raw_start;
        tbl[SFXF_ELEV_DOWN].size     = _binary_elev_down_raw_end - _binary_elev_down_raw_start;
        tbl[SFXF_DOOR].data          = (const void *)_binary_door_raw_start;
        tbl[SFXF_DOOR].size          = _binary_door_raw_end - _binary_door_raw_start;
        tbl[SFXF_BLIP].data          = (const void *)_binary_blip_raw_start;
        tbl[SFXF_BLIP].size          = _binary_blip_raw_end - _binary_blip_raw_start;
        tbl[SFXF_BREATH].data        = (const void *)_binary_breath_raw_start;
        tbl[SFXF_BREATH].size        = _binary_breath_raw_end - _binary_breath_raw_start;
        tbl[SFXF_STINGER].data       = (const void *)_binary_stinger_raw_start;
        tbl[SFXF_STINGER].size       = _binary_stinger_raw_end - _binary_stinger_raw_start;
        tbl[SFXF_TRUTH].data         = (const void *)_binary_truth_raw_start;
        tbl[SFXF_TRUTH].size         = _binary_truth_raw_end - _binary_truth_raw_start;
        tbl[SFXF_AMBIENCE].data      = (const void *)_binary_ambience_raw_start;
        tbl[SFXF_AMBIENCE].size      = _binary_ambience_raw_end - _binary_ambience_raw_start;
        inited = true;
    }
    return tbl;
}

#endif /* SFX_TABLE_H */
