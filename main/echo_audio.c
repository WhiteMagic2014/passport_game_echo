/*
 * echo_audio.c —— 音效合成实现。逐行对照 prototype/js/audio.js。
 *
 * 关键对照：
 *   - 采样率固定 16kHz（JS 用 ctx.sampleRate，这里按 BSP 固定）。
 *   - 每个音效 = 一个「单循环 + 逐样本 double 累加」的填充过程，与 JS 的
 *     fill(d, sr, n) 结构一致；噪声低通 prev 在该循环内联续算。
 *   - Math.random → xorshift32；Math.exp/sin/pow/min/max/abs → math.h。
 */
#include "echo_audio.h"

#include <math.h>
#include <string.h>

static double g_volume = 0.85;
static uint32_t g_rng = 0x1234ABCDu;

void echo_audio_set_volume(double v) { g_volume = v; }
double echo_audio_volume(void) { return g_volume; }
void echo_audio_seed(uint32_t s) { g_rng = s; }

/* [0,1) 均匀随机（xorshift32） */
static double rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (double)(g_rng >> 8) / 16777216.0;
}

static inline int16_t quant(double v) {
    double s = v * g_volume * 32767.0;
    if (s > 32767.0) s = 32767.0;
    if (s < -32768.0) s = -32768.0;
    return (int16_t)lround(s);
}

static int dur(double sec) { return (int)(sec * ECHO_SR + 0.5); }

/* 金属敲击单次贡献（t 为距敲击起点秒数）。带起音冲击噪声。 */
static double knock_at(double t) {
    double env = exp(-t * 42.0);
    double v = sin(2 * M_PI * 418 * t)
             + 0.62 * sin(2 * M_PI * 611 * t)
             + 0.38 * sin(2 * M_PI * 1013 * t);
    v /= 2.0;
    if (t < 0.006) v += (rnd() * 2.0 - 1.0) * 0.9;
    return v * env;
}

int echo_audio_samples(sfx_t sfx) {
    switch (sfx) {
        case SFX_KNOCK:        return dur(1.2);
        case SFX_KNOCK_SOFT:   return dur(1.4);
        case SFX_ECHO_OPEN:
        case SFX_ECHO_BLOCKED:
        case SFX_ECHO_ALIVE:
        case SFX_ECHO_ANSWERED:return dur(3.2);
        case SFX_FOOTSTEP:     return dur(0.5);
        case SFX_ELEV_UP:
        case SFX_ELEV_DOWN:    return dur(1.6);
        case SFX_DOOR:         return dur(2.4);
        case SFX_BLIP:         return dur(0.12);
        case SFX_BREATH:       return dur(2.0);
        case SFX_STINGER:      return dur(1.8);
        case SFX_TRUTH:        return dur(2.6);
        default:               return 0;
    }
}

int echo_audio_render(sfx_t sfx, int16_t *out, int cap) {
    int n = echo_audio_samples(sfx);
    if (n <= 0 || cap < n) return 0;

    switch (sfx) {
        case SFX_KNOCK: {
            for (int i = 0; i < n; i++)
                out[i] = quant(knock_at((double)i / ECHO_SR) * 0.75);
            break;
        }
        case SFX_KNOCK_SOFT: {
            for (int i = 0; i < n; i++)
                out[i] = quant(knock_at((double)i / ECHO_SR) * 0.42);
            break;
        }
        case SFX_ECHO_OPEN:
        case SFX_ECHO_BLOCKED:
        case SFX_ECHO_ALIVE:
        case SFX_ECHO_ANSWERED: {
            int taps; double gap, decay, amp;
            switch (sfx) {
                case SFX_ECHO_OPEN:     taps = 5; gap = 0.19; decay = 0.62; amp = 0.62; break;
                case SFX_ECHO_BLOCKED:  taps = 2; gap = 0.07; decay = 0.34; amp = 0.70; break;
                default:                taps = 3; gap = 0.14; decay = 0.55; amp = 0.66; break;
            }
            int alive = (sfx == SFX_ECHO_ALIVE || sfx == SFX_ECHO_ANSWERED);
            int answered = (sfx == SFX_ECHO_ANSWERED);
            double extra_t0 = 0.6 + taps * gap;
            for (int i = 0; i < n; i++) {
                double t = (double)i / ECHO_SR;
                double acc = 0.0;
                for (int k = 1; k <= taps; k++) {
                    double t0 = k * gap;
                    if (t >= t0) acc += knock_at(t - t0) * amp * pow(decay, k - 1);
                }
                if (alive) {
                    double br = sin(2 * M_PI * 0.9 * t) * exp(-t * 1.4) * 0.16;
                    acc += br * (0.6 + 0.4 * sin(2 * M_PI * 7 * t));
                }
                if (answered && t >= extra_t0) acc += knock_at(t - extra_t0) * 0.58;
                out[i] = quant(acc);
            }
            break;
        }
        case SFX_FOOTSTEP: {
            /* 由 echo_audio_footstep_render 处理距离参数；此处按 dist=0 兜底。 */
            break;
        }
        case SFX_ELEV_UP:
        case SFX_ELEV_DOWN: {
            double f = (sfx == SFX_ELEV_UP) ? 62.0 : 74.0;
            for (int i = 0; i < n; i++) {
                double t = (double)i / ECHO_SR;
                double hum = sin(2 * M_PI * f * t) * 0.16;
                double slip = sin(2 * M_PI * 190 * t) * 0.03 * sin(2 * M_PI * 3 * t);
                double env = fmin(1.0, t * 4.0) * exp(-fmax(0.0, t - 1.1) * 3.5);
                out[i] = quant((hum + slip) * env);
            }
            break;
        }
        case SFX_DOOR: {
            double i0 = 0.05 * ECHO_SR, i1 = (0.05 + 0.9) * ECHO_SR;
            double prev = 0.0;
            for (int i = 0; i < n; i++) {
                double t = (double)i / ECHO_SR;
                double hyd = sin(2 * M_PI * 52 * t) * 0.18 * exp(-t * 1.1);
                double creak = sin(2 * M_PI * (300 + 140 * sin(2 * M_PI * 1.7 * t)) * t)
                             * 0.07 * exp(-t * 0.9);
                double acc = hyd + creak;
                if (i >= (int)i0 && i < (int)i1) {
                    double tn = ((double)i - i0) / ECHO_SR;
                    double w = rnd() * 2.0 - 1.0;
                    prev = prev + (w - prev) * 0.03;
                    acc += prev * 0.14 * exp(-tn * 3.0);
                }
                out[i] = quant(acc);
            }
            break;
        }
        case SFX_BLIP: {
            for (int i = 0; i < n; i++) {
                double t = (double)i / ECHO_SR;
                out[i] = quant(sin(2 * M_PI * 880 * t) * exp(-t * 60) * 0.10);
            }
            break;
        }
        case SFX_BREATH: {
            for (int i = 0; i < n; i++) {
                double t = (double)i / ECHO_SR;
                double e = exp(-t * 1.6);
                out[i] = quant((sin(2 * M_PI * 0.8 * t) * 0.14 + (rnd() * 2.0 - 1.0) * 0.05) * e);
            }
            break;
        }
        case SFX_STINGER: {
            int i1 = (int)(0.5 * ECHO_SR);
            double prev = 0.0;
            for (int i = 0; i < n; i++) {
                double t = (double)i / ECHO_SR;
                double v = sin(2 * M_PI * 55 * t) + 0.7 * sin(2 * M_PI * 83 * t);
                double acc = v * 0.22 * exp(-t * 1.9);
                if (i < i1) {
                    double w = rnd() * 2.0 - 1.0;
                    prev = prev + (w - prev) * 0.12;
                    acc += prev * 0.3 * exp(-t * 3.0);
                }
                out[i] = quant(acc);
            }
            break;
        }
        case SFX_TRUTH: {
            for (int i = 0; i < n; i++) {
                double t = (double)i / ECHO_SR;
                double f = 110 + 220 * fmin(1.0, t / 2.0);
                double v = sin(2 * M_PI * f * t) + 0.5 * sin(2 * M_PI * f * 2 * t);
                double env = 0.10 * fmin(1.0, t * 1.5) * exp(-fmax(0.0, t - 1.8) * 2.0);
                out[i] = quant(v * env);
            }
            break;
        }
        default:
            return 0;
    }
    return n;
}

int echo_audio_footstep_render(int dist, int16_t *out, int cap) {
    int n = dur(0.5);
    if (cap < n) return 0;

    double amp = fmax(0.05, 0.85 - fabs((double)dist) * 0.09);
    int i1 = (int)(0.16 * ECHO_SR);
    double prev = 0.0;
    for (int i = 0; i < n; i++) {
        double t = (double)i / ECHO_SR;
        double acc = sin(2 * M_PI * 88 * t) * exp(-t * 22) * amp * 0.5;
        if (i < i1) {
            double w = rnd() * 2.0 - 1.0;
            prev = prev + (w - prev) * 0.06;
            acc += prev * (amp * 0.7) * exp(-t * 3.0);
        }
        out[i] = quant(acc);
    }
    return n;
}

int echo_audio_ambience_render(int16_t *out, int cap) {
    int n = dur(4.0);
    if (cap < n) return 0;
    echo_audio_ambience_fill(out, 0, n);
    return n;
}

void echo_audio_ambience_fill(int16_t *out, int64_t start, int count) {
    for (int i = 0; i < count; i++) {
        double t = (double)(start + i) / ECHO_SR;
        double v = 0.035 * sin(2 * M_PI * 54 * t)
                 + 0.018 * sin(2 * M_PI * 81 * t)
                 + 0.010 * (rnd() * 2.0 - 1.0);
        out[i] = quant(v);
    }
}
