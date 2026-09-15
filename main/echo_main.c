/*
 * echo_main.c —— 《回声·下沉》ESP32-C3 / FoloToy AI Passport 主程序。
 *
 * 职责：BSP 初始化、按键映射、主循环（tick / 事件消费 / 渲染 / 刷屏）、
 *       音频任务（flash 预生成 PCM 流式播放）、NVS 存档。
 *
 * 音频方案：所有音效在 PC 端预合成为 raw PCM（gen_sfx.c），通过 EMBED_FILES
 * 嵌入 flash。运行时音频任务用 1KB 小缓冲从 flash 流式读取，不再需要 104KB
 * 合成缓冲。C3 无 FPU，此方案同时省 DRAM 和 CPU。
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"

#include "echo_core.h"
#include "echo_render.h"
#include "echo_history.h"
#include "echo_audio.h"
#include "sfx_table.h"

static const char *TAG = "echo";

/* ---------------------------------------------------------------------
 * 静态资源（无 PSRAM，全部静态分配）
 * ------------------------------------------------------------------- */
#define BAND_H 40                       /* 分带渲染行数：320/40=8 带 */
static uint16_t s_fb[2][ECHO_W * BAND_H];  /* 双缓冲分带帧缓冲 ~38KB */
static int     s_fb_idx = 0;                 /* 交替缓冲索引 */

static EchoState s_st;

/* 按键事件队列（button 任务 → 主循环） */
typedef struct { uint8_t btn; uint8_t ev; } btn_msg_t;
static QueueHandle_t s_btn_q;

/* 音效请求队列（主循环 → 音频任务） */
typedef struct { uint8_t sfx; int32_t param; } sfx_req_t;
static QueueHandle_t s_audio_q;

/* OK 长按检测（button 任务写、主循环读；uint32/byte 原子） */
static volatile bool     s_ok_held;
static volatile bool     s_ok_long_sent;
static volatile uint32_t s_ok_press_ms;

/* 定时事件（仅主循环读写） */
static uint32_t s_silent_echo_at_ms;    /* 静默回声延迟（400ms） */
static uint32_t s_amb_cut_until_ms;     /* 底噪硬切恢复（2600ms） */

/* 底噪开关（主循环写、音频任务读；单字节原子） */
static volatile bool s_amb_on;

/* 结局存档标记 */
static bool s_saved;

/* 主菜单状态机 */
typedef enum { APP_MENU = 0, APP_GAME } app_phase_t;
static app_phase_t s_phase;
static MenuState   s_menu;
static int         s_last_turn;                    /* 续局存档：回合推进后保存 */
static uint8_t     s_cont_buf[ECHO_SAVE_SIZE];     /* 续局序列化缓冲 */

#define NVS_NS          "echo"
#define NVS_KEY_LAST    "last_run"
#define NVS_KEY_CONT    "continue"
#define NVS_KEY_VOLUME  "volume"
#define CHUNK 512                       /* 音频每次混音采样数 (1KB) */

static esp_lcd_panel_handle_t s_panel;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---------------------------------------------------------------------
 * 音频任务：从 flash 流式播放预生成 PCM
 * ------------------------------------------------------------------- */
static void post_sfx(uint8_t sfx, int32_t param) {
    sfx_req_t r = { .sfx = sfx, .param = param };
    xQueueSend(s_audio_q, &r, 0);
}

static void audio_task(void *arg) {
    (void)arg;
    const sfx_entry_t *tbl = sfx_table();
    int16_t chunk[CHUNK];

    /* 环境底噪：循环播放 */
    const int16_t *amb_data = (const int16_t *)tbl[SFXF_AMBIENCE].data;
    int amb_total  = tbl[SFXF_AMBIENCE].size / 2;
    int amb_offset = 0;

    /* 当前音效播放状态 */
    const int16_t *fx_data = NULL;
    int  fx_total = 0;
    int  fx_pos   = 0;
    int  fx_scale = 256;                  /* 8.8 定点音量系数，256=1.0 */

    for (;;) {
        /* 1. 处理音效请求 */
        sfx_req_t r;
        while (xQueueReceive(s_audio_q, &r, 0) == pdTRUE) {
            if (r.sfx >= SFXF_COUNT) continue;
            if (tbl[r.sfx].size == 0) continue;
            fx_data  = (const int16_t *)tbl[r.sfx].data;
            fx_total = tbl[r.sfx].size / 2;
            fx_pos   = 0;
            /* footstep：按距离缩放（预生成时 dist=0, amp=0.85） */
            if (r.sfx == SFXF_FOOTSTEP) {
                double amp = fmax(0.05, 0.85 - fabs((double)r.param) * 0.09);
                fx_scale = (int)(amp / 0.85 * 256.0);
            } else {
                fx_scale = 256;
            }
        }

        /* 2. 生成一帧：底噪循环 + 音效混音 */
        for (int i = 0; i < CHUNK; i++) {
            int val = 0;
            if (s_amb_on && amb_data) {
                val = amb_data[amb_offset];
                amb_offset = (amb_offset + 1) % amb_total;
            }
            if (fx_data && fx_pos < fx_total) {
                val += (fx_data[fx_pos++] * fx_scale) >> 8;
            }
            if (val > 32767) val = 32767;
            if (val < -32768) val = -32768;
            chunk[i] = (int16_t)val;
        }
        if (fx_pos >= fx_total) { fx_data = NULL; fx_total = 0; }

        /* 3. 写出（阻塞，自然节流到实时） */
        bsp_audio_write(chunk, sizeof(chunk));
    }
}

/* ---------------------------------------------------------------------
 * 存档
 * ------------------------------------------------------------------- */
static void load_run(LastRun *out) {
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(*out);
    if (nvs_get_blob(h, NVS_KEY_LAST, out, &len) != ESP_OK) memset(out, 0, sizeof(*out));
    nvs_close(h);
}

static void save_run(void) {
    LastRun r;
    echo_summary(&s_st, &r);
    if (r.max_depth <= 0) return;                 /* 没下过就不存（与 JS 一致） */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_LAST, &r, sizeof(r));
    nvs_commit(h);
    nvs_close(h);
}

/* 续局存档：全量 EchoState（每次回合推进时写） */
static bool load_continue(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = ECHO_SAVE_SIZE;
    esp_err_t e = nvs_get_blob(h, NVS_KEY_CONT, s_cont_buf, &len);
    nvs_close(h);
    if (e != ESP_OK) return false;
    return echo_load_state(&s_st, s_cont_buf, len);
}

static void save_continue(void) {
    size_t n = echo_save_state(&s_st, s_cont_buf, ECHO_SAVE_SIZE);
    if (n == 0) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_CONT, s_cont_buf, n);
    nvs_commit(h);
    nvs_close(h);
}

static void clear_continue(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_KEY_CONT);
    nvs_commit(h);
    nvs_close(h);
}

/* 音量：NVS 持久化（0..100） */
static uint8_t load_volume(void) {
    uint8_t v = 85;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return v;
    uint8_t t = 0;
    if (nvs_get_u8(h, NVS_KEY_VOLUME, &t) == ESP_OK && t <= 100) v = t;
    nvs_close(h);
    return v;
}

static void save_volume(uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_VOLUME, v);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------------------------------------------------------------
 * 游戏流程 / 主菜单
 * ------------------------------------------------------------------- */
static void new_game(void) {
    LastRun last;
    load_run(&last);
    int32_t seed = (int32_t)(esp_timer_get_time() & 0x7FFFFFFF);
    if (seed == 0) seed = 1;
    echo_new_game(&s_st, seed, &last);
    s_saved = false;
    s_amb_on = true;
    s_silent_echo_at_ms = 0;
    s_amb_cut_until_ms = 0;
    ESP_LOGI(TAG, "新一局，seed=%ld", (long)seed);
}

static void enter_menu(void) {
    s_phase = APP_MENU;
    s_menu.screen = MENU_MAIN;
    s_menu.sel = 0;
    /* 探测续局存档是否存在（不落地读状态） */
    nvs_handle_t h;
    s_menu.has_continue = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = ECHO_SAVE_SIZE;
        s_menu.has_continue = (nvs_get_blob(h, NVS_KEY_CONT, s_cont_buf, &len) == ESP_OK);
        nvs_close(h);
    }
    LastRun last;
    load_run(&last);
    s_menu.last_depth = last.max_depth;
    s_amb_on = false;
}

static void start_game(bool cont) {
    if (cont) {
        if (load_continue()) {
            s_saved = false;
            s_amb_on = true;
            s_silent_echo_at_ms = 0;
            s_amb_cut_until_ms = 0;
            ESP_LOGI(TAG, "继续上一局，depth=%d turn=%d", s_st.depth, s_st.turn);
        } else {                       /* 读档失败：清掉脏档，新开 */
            clear_continue();
            new_game();
        }
    } else {
        clear_continue();
        new_game();
    }
    s_last_turn = s_st.turn;
    s_phase = APP_GAME;
}

static void end_to_menu(void) {
    save_run();                        /* 结局时 game_loop 已存，此处幂等兜底 */
    clear_continue();
    s_amb_on = false;
    enter_menu();
}

static bool menu_enabled(int i) { return i != 1 || s_menu.has_continue; }
static int menu_next(int cur) { int i = cur; do { i = (i + 1) % 4; } while (!menu_enabled(i)); return i; }
static int menu_prev(int cur) { int i = cur; do { i = (i + 3) % 4; } while (!menu_enabled(i)); return i; }

static void menu_dispatch(uint8_t btn, uint8_t ev) {
    if (s_menu.screen == MENU_VOLUME) {
        if (ev == BTN_PRESS && btn == BTN_UP) {
            if (s_menu.volume < 100) s_menu.volume += 5;
            post_sfx(SFX_BLIP, 0);
        } else if (ev == BTN_PRESS && btn == BTN_DOWN) {
            if (s_menu.volume > 0) s_menu.volume -= 5;
            post_sfx(SFX_BLIP, 0);
        } else if (btn == BTN_OK && ev == BTN_PRESS) {
            save_volume(s_menu.volume);
            bsp_audio_set_volume(s_menu.volume);
            s_menu.screen = MENU_MAIN;
            post_sfx(SFX_BLIP, 0);
        }
        return;
    }
    if (s_menu.screen == MENU_HELP) {
        if (btn == BTN_OK && ev == BTN_PRESS) {
            s_menu.screen = MENU_MAIN;
            post_sfx(SFX_BLIP, 0);
        }
        return;
    }
    if (s_menu.screen == MENU_HISTORY) {
        HistView *v = &s_menu.hist;
        if (v->detail >= 0) {
            /* 详情：上下翻正文，OK 返回列表 */
            if (ev == BTN_PRESS && btn == BTN_UP) {
                if (v->dscroll > 0) v->dscroll--;
                post_sfx(SFX_BLIP, 0);
            } else if (ev == BTN_PRESS && btn == BTN_DOWN) {
                int maxd = v->dlines - HIST_DET_ROWS;
                if (maxd < 0) maxd = 0;
                if (v->dscroll < maxd) v->dscroll++;
                post_sfx(SFX_BLIP, 0);
            } else if (btn == BTN_OK) {      /* 短按/长按都返回列表 */
                v->detail = -1; v->dscroll = 0;
                post_sfx(SFX_BLIP, 0);
            }
            return;
        }
        /* 列表 */
        if (ev == BTN_PRESS && btn == BTN_UP) {
            echo_hist_move(v, -1); post_sfx(SFX_BLIP, 0);
        } else if (ev == BTN_PRESS && btn == BTN_DOWN) {
            echo_hist_move(v, 1); post_sfx(SFX_BLIP, 0);
        } else if (btn == BTN_OK && ev == BTN_LONG) {
            s_menu.screen = MENU_MAIN; post_sfx(SFX_BLIP, 0);
        } else if (btn == BTN_OK && ev == BTN_PRESS) {
            /* 只有已解锁的条目能进详情（未解锁不剧透） */
            if (v->sel >= 0 && v->sel < v->count && v->items[v->sel].unlocked) {
                v->detail = v->sel; v->dscroll = 0; v->dlines = 0;
                post_sfx(SFX_BLIP, 0);
            }
        }
        return;
    }
    /* 主菜单 */
    if (ev != BTN_PRESS) return;
    if (btn == BTN_UP)      { s_menu.sel = menu_prev(s_menu.sel); post_sfx(SFX_BLIP, 0); }
    else if (btn == BTN_DOWN) { s_menu.sel = menu_next(s_menu.sel); post_sfx(SFX_BLIP, 0); }
    else if (btn == BTN_OK) {
        if (s_menu.sel == 0) start_game(false);
        else if (s_menu.sel == 1) { if (s_menu.has_continue) start_game(true); }
        else if (s_menu.sel == 2) { s_menu.screen = MENU_VOLUME; post_sfx(SFX_BLIP, 0); }
        else {  /* 历史：每次进入都重新读档，保证看到的是最新解锁 */
            echo_hist_load(&s_menu.archive);
            echo_hist_build(&s_menu.hist, &s_menu.archive);
            s_menu.screen = MENU_HISTORY;
            post_sfx(SFX_BLIP, 0);
        }
    }
}

static void handle_input(uint8_t btn, uint8_t ev) {
    if (s_phase == APP_MENU) { menu_dispatch(btn, ev); return; }

    /* 游戏阶段 */
    if (s_st.ended != -1) {
        if (btn == BTN_OK && ev == BTN_PRESS) end_to_menu();
        return;
    }
    int fp = echo_nearest_pos(&s_st);
    echo_input(&s_st, btn, ev);
    /* 回合制：它只在动作后走一步，这一步远近决定脚步音量 */
    if (s_st.ended == -1) {
        int np = echo_nearest_pos(&s_st);
        if (np != fp) {
            post_sfx(SFX_FOOTSTEP, np - s_st.depth);
        }
    }
    /* 续局存档：回合推进后保存（面板光标移动等 UI 操作不算回合） */
    if (s_st.ended == -1 && s_st.turn != s_last_turn) {
        s_last_turn = s_st.turn;
        save_continue();
    }
}

/* 事件 → 音效（对照 main.js 的 consume()） */
static void dispatch_sfx(event_t ev) {
    switch (ev) {
        case EV_KNOCK:      post_sfx(SFX_KNOCK, 0); break;
        case EV_ECHO: {
            sfx_t s = SFX_ECHO_OPEN;
            if (s_st.echo_sig == ECHO_BLOCKED)       s = SFX_ECHO_BLOCKED;
            else if (s_st.echo_sig == ECHO_ALIVE)    s = SFX_ECHO_ALIVE;
            else if (s_st.echo_sig == ECHO_ANSWERED) s = SFX_ECHO_ANSWERED;
            post_sfx(s, 0);
            break;
        }
        case EV_MOVE_UP:     post_sfx(SFX_ELEV_UP, 0); break;
        case EV_MOVE_DOWN:   post_sfx(SFX_ELEV_DOWN, 0); break;
        case EV_PANEL:
        case EV_BLIP:
        case EV_LISTEN:
        case EV_REPAIR:      post_sfx(SFX_BLIP, 0); break;
        case EV_ENCOUNTER:
        case EV_EGG:
        case EV_STRANDED:    post_sfx(SFX_STINGER, 0); break;
        case EV_BREATH:      post_sfx(SFX_BREATH, 0); break;
        case EV_SILENT_ECHO: s_silent_echo_at_ms = now_ms() + 400; break;
        case EV_AMBIENCE_CUT:s_amb_on = false; s_amb_cut_until_ms = now_ms() + 2600; break;
        case EV_AHEAD:       post_sfx(SFX_KNOCK, 0); break;
        case EV_DOOR:        post_sfx(SFX_DOOR, 0); break;
        case EV_TRUTH:       post_sfx(SFX_TRUTH, 0); break;
        default: break;
    }
}

static void consume_events(void) {
    for (int i = 0; i < s_st.ev_count; i++) dispatch_sfx(s_st.events[i]);
    s_st.ev_count = 0;
}

/* 分带渲染 + 双缓冲：交替使用两块 fb，避免 DMA 传输未完成时被覆盖 */
static void render_frame(uint32_t tms) {
    for (int y = 0; y < ECHO_H; y += BAND_H) {
        uint16_t *fb = s_fb[s_fb_idx];
        s_fb_idx ^= 1;
        if (s_phase == APP_MENU) echo_render_menu_band(&s_menu, fb, tms, y, BAND_H);
        else                     echo_render_band(&s_st, fb, tms, y, BAND_H);
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, ECHO_W, y + BAND_H, fb);
    }
}

static void game_loop(void) {
    uint32_t last_ms = 0;
    uint32_t tms = 0;
    for (;;) {
        uint32_t now = now_ms();
        tms = now;
        uint32_t dt = last_ms ? now - last_ms : 16;
        if (dt > 120) dt = 120;
        last_ms = now;

        /* 1. 按键 */
        btn_msg_t m;
        while (xQueueReceive(s_btn_q, &m, 0) == pdTRUE) handle_input(m.btn, m.ev);

        /* 2. OK 长按（450ms） */
        if (s_ok_held && !s_ok_long_sent && (now - s_ok_press_ms) >= CFG_PANEL_LONG_MS) {
            s_ok_long_sent = true;
            handle_input(BTN_OK, BTN_LONG);
        }

        if (s_phase == APP_GAME) {
            /* 3. 定时：静默回声 / 底噪恢复 */
            if (s_silent_echo_at_ms && (int32_t)(now - s_silent_echo_at_ms) >= 0) {
                s_silent_echo_at_ms = 0;
                post_sfx(SFX_KNOCK_SOFT, 0);
            }
            if (s_amb_cut_until_ms && (int32_t)(now - s_amb_cut_until_ms) >= 0) {
                s_amb_cut_until_ms = 0;
                s_amb_on = true;
            }

            /* 4. tick 只推进演出（波纹 / 开门 / 闪白），不改逻辑 */
            echo_tick(&s_st, (double)dt);

            /* 5. 消费事件 → 音效 */
            consume_events();

            /* 6. 结局：自动存档 + 清续局 + 停底噪 */
            if (s_st.ended != -1 && !s_saved) {
                s_saved = true;
                save_run();
                /* 历史档案：登记本局解锁的结局 / 彩蛋 / 碎片 */
                echo_hist_load(&s_menu.archive);
                echo_hist_record(&s_menu.archive, &s_st);
                echo_hist_save(&s_menu.archive);
                clear_continue();
                s_amb_on = false;
            }
        }

        /* 7. 渲染 + 刷屏（分带） */
        render_frame(tms);

        /* 8. 帧率节流 ~60fps */
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

/* ---------------------------------------------------------------------
 * 按键回调（button 组件定时器任务；只投递事件，勿阻塞）
 * ------------------------------------------------------------------- */
static void on_button(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_PRESS) {
            s_ok_held = true;
            s_ok_long_sent = false;
            s_ok_press_ms = now_ms();
            btn_msg_t m = { BTN_OK, BTN_PRESS };
            xQueueSend(s_btn_q, &m, 0);
        } else if (ev == BSP_BTN_CLICK) {
            s_ok_held = false;                 /* 抬起：取消长按 */
        }
        /* BSP_BTN_LONG / DOUBLE 忽略：长按由主循环按 450ms 自判 */
    } else if (ev == BSP_BTN_PRESS) {
        btn_msg_t m = { (uint8_t)btn, BTN_PRESS };
        xQueueSend(s_btn_q, &m, 0);
    }
}

/* ---------------------------------------------------------------------
 * 入口
 * ------------------------------------------------------------------- */
void app_main(void) {
    ESP_LOGI(TAG, "《回声·下沉》启动");

    /* NVS（可选，失败则关闭存档） */
    if (nvs_flash_init() != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失败，尝试清空后重试");
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* 显示 */
    if (bsp_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "显示初始化失败，无法继续");
        return;
    }
    bsp_display_backlight(100);
    s_panel = bsp_display_panel();

    /* 音频 */
    bsp_audio_init();
    bsp_audio_set_format(ECHO_SR, 16, 1);

    /* 音量（NVS 持久化，默认 85） */
    s_menu.volume = load_volume();
    bsp_audio_set_volume(s_menu.volume);

    /* 队列 */
    s_btn_q   = xQueueCreate(8, sizeof(btn_msg_t));
    s_audio_q = xQueueCreate(8, sizeof(sfx_req_t));

    /* 按键 + 音频任务 */
    bsp_button_init(on_button, NULL);
    xTaskCreate(audio_task, "echo_audio", 4096, NULL, 5, NULL);

    /* 进入主菜单（静态先画一帧，避免黑屏） */
    enter_menu();
    render_frame(0);

    ESP_LOGI(TAG, "就绪：主菜单（↑↓ 选择　OK 确认）");

    game_loop();
}
