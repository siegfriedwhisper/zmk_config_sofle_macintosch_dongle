/*
 * TrackPoint 运行时参数（右手）
 *
 * 参数经接收器（dongle）通过 split 的 INVOKE_BEHAVIOR 通道下发；
 * dongle 是参数的权威源，本半不持久化 —— 每次连上由 dongle 推全表。
 * 见 docs/tuning-protocol.md
 */

#include "trackpoint_config.h"

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(trackpoint, LOG_LEVEL_INF);

trackpoint_params_t g_tp_params = TRACKPOINT_PARAMS_DEFAULT;

void trackpoint_params_reset(void) {
    g_tp_params = (trackpoint_params_t)TRACKPOINT_PARAMS_DEFAULT;
    LOG_INF("TrackPoint params reset to defaults");
}

int trackpoint_param_apply(uint8_t id, uint32_t value) {
    switch (id) {
    case 0x41:
        g_tp_params.mouse_base_speed = (uint16_t)value;
        break;
    case 0x42:
        g_tp_params.mouse_sens_base = (uint16_t)value;
        break;
    case 0x43:
        g_tp_params.mouse_sens_step = (uint16_t)value;
        break;
    case 0x44:
        g_tp_params.scroll_deadzone = (uint16_t)value;
        break;
    case 0x45:
        g_tp_params.scroll_input_max = (uint16_t)value;
        break;
    case 0x46:
        g_tp_params.scroll_divisor_slow = (uint16_t)value;
        break;
    case 0x47:
        g_tp_params.scroll_divisor_fast = (uint16_t)value;
        break;
    case 0x48:
        g_tp_params.dominant_num = (uint16_t)value;
        break;
    case 0x49:
        g_tp_params.dominant_den = (uint16_t)value;
        break;
    case 0x4A:
        /* 方向只认 ±1，避免 0 导致滚动彻底失效 */
        g_tp_params.scroll_x_dir = (value == 0) ? 1 : (int16_t)(int32_t)value;
        break;
    case 0x4B:
        g_tp_params.scroll_y_dir = (value == 0) ? 1 : (int16_t)(int32_t)value;
        break;
    case 0x4C:
        g_tp_params.exponential = value ? 1 : 0;
        break;
    case 0x4D:
        g_tp_params.exp_slope = (uint16_t)value;
        break;
    case 0x4E:
        g_tp_params.exp_max_mult = (uint16_t)value;
        break;
    case 0x4F:
        g_tp_params.slow_key_mult = (uint16_t)value;
        break;

    default:
        /* 不属于右手段（0x40–0x7F），静默忽略 */
        return -1;
    }

    LOG_DBG("TP param 0x%02X = %u", id, (unsigned)value);
    return 0;
}
