#pragma once

#include <stdint.h>
#include <stdbool.h>

/* =========================================================
 * TrackPoint 运行时参数（右手）
 *
 * 默认值 = 原 Kconfig / 硬编码值 ⇒ 改造前后手感逐项一致。
 * Kconfig 仍然保留，作为"出厂默认"的来源；运行时以 g_tp_params 为准。
 *
 * 参数由接收器（dongle）经 split 通道下发，见 docs/tuning-protocol.md
 * ========================================================= */
typedef struct {
    /* --- 鼠标指针 --- */
    uint16_t mouse_base_speed;    /* %，原 MOUSE_BASE_SPEED_PERCENT，默认 50 */
    uint16_t mouse_sens_base;     /* %，默认 30 */
    uint16_t mouse_sens_step;     /* %，默认 1（每档 LED 亮度的灵敏度增量） */

    /* --- 滚轮 --- */
    int16_t scroll_x_dir;         /* 1 / -1，默认 1 */
    int16_t scroll_y_dir;         /* 1 / -1，默认 1 */
    uint16_t scroll_deadzone;     /* 默认 2 */
    uint16_t scroll_input_max;    /* 默认 128 */
    uint16_t scroll_divisor_slow; /* 默认 60 */
    uint16_t scroll_divisor_fast; /* 默认 8 */

    /* --- 防误触（主轴比值）--- */
    uint16_t dominant_num;        /* 默认 3 */
    uint16_t dominant_den;        /* 默认 2 */

    /* --- 加速曲线（原本写死在 .c 里）--- */
    uint8_t exponential;          /* 0/1，原 CONFIG_TRACKPOINT_EXPONENTIAL，默认 1 */
    uint16_t exp_slope;           /* ×1000，原硬编码 1.307357f ⇒ 1307 */
    uint16_t exp_max_mult;        /* ×100，原 TP_MAX_MULT 2.0f ⇒ 200 */
    uint16_t slow_key_mult;       /* ×100，原 SLOW_KEY_MULTIPLIER 0.5f ⇒ 50 */
} trackpoint_params_t;

#define TRACKPOINT_PARAMS_DEFAULT {                                        \
    .mouse_base_speed    = CONFIG_TRACKPOINT_MOUSE_BASE_SPEED_PERCENT,     \
    .mouse_sens_base     = CONFIG_TRACKPOINT_MOUSE_SENS_BASE_PERCENT,      \
    .mouse_sens_step     = CONFIG_TRACKPOINT_MOUSE_SENS_STEP_PERCENT,      \
    .scroll_x_dir        = CONFIG_TRACKPOINT_SCROLL_X_DIR,                 \
    .scroll_y_dir        = CONFIG_TRACKPOINT_SCROLL_Y_DIR,                 \
    .scroll_deadzone     = CONFIG_TRACKPOINT_SCROLL_DEADZONE,              \
    .scroll_input_max    = CONFIG_TRACKPOINT_SCROLL_INPUT_MAX,             \
    .scroll_divisor_slow = CONFIG_TRACKPOINT_SCROLL_DIVISOR_SLOW,          \
    .scroll_divisor_fast = CONFIG_TRACKPOINT_SCROLL_DIVISOR_FAST,          \
    .dominant_num        = CONFIG_TRACKPOINT_DOMINANT_NUMERATOR,           \
    .dominant_den        = CONFIG_TRACKPOINT_DOMINANT_DENOMINATOR,         \
    .exponential         = 1,                                              \
    .exp_slope           = 1307,                                           \
    .exp_max_mult        = 200,                                            \
    .slow_key_mult       = 50,                                             \
}

/* =========================================================
 * 外部接口
 * ========================================================= */

/** 当前生效的运行时参数 */
extern trackpoint_params_t g_tp_params;

/** 恢复出厂默认（编译期 Kconfig 值） */
void trackpoint_params_reset(void);

/**
 * 按参数 ID 写入一个参数。
 * @return 0 = 已应用；-1 = 该 ID 不属于右手（静默忽略）
 */
int trackpoint_param_apply(uint8_t id, uint32_t value);
