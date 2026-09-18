#pragma once

#include <stdint.h>
#include <stdbool.h>

/* =========================================================
 * 运行时参数结构体——所有可调参数
 * 默认值 = 当前代码 #define 值（手感一致）
 * ========================================================= */
typedef struct {
    uint8_t sens_percent;        /* 查表缩放 50-150%，默认 100% */
    uint8_t wheel_units;         /* 每次滚轮发送档数，默认 1 */
    uint8_t arrow_threshold;     /* 方向键触发阈值，默认 4 */
    uint16_t arrow_repeat_ms;    /* 方向键重复间隔，默认 35 */
    uint16_t active_ms;          /* 移动判定窗，默认 40 */
    uint8_t base_move_pixels;    /* 查表越界兜底，默认 3 */
} trackball_params_t;

/* 默认值（与当前 #define 一致） */
#define TRACKBALL_PARAMS_DEFAULT { \
    .sens_percent = 100,           \
    .wheel_units = 1,              \
    .arrow_threshold = 4,          \
    .arrow_repeat_ms = 35,         \
    .active_ms = 40,               \
    .base_move_pixels = 3,         \
}

/* =========================================================
 * 外部可访问的运行时参数与缩放查表
 * ========================================================= */
extern trackball_params_t g_params;

#define SPEED_LUT_SIZE 45
extern uint16_t g_speed_lut_eff[SPEED_LUT_SIZE];
extern const uint16_t speed_lut_base[SPEED_LUT_SIZE];  /* 原物理查表 */

/* =========================================================
 * API
 * ========================================================= */

/* 注：初始化（重建 LUT + 加载 settings）由本模块内的 SYS_INIT 触发，
 *     函数为 static，不对外暴露声明。 */

/** 按当前 sens 重建缩放查表 g_speed_lut_eff */
void trackball_config_rebuild_lut(void);

/** 保存当前参数到 flash（settings 持久化） */
int trackball_params_save(void);