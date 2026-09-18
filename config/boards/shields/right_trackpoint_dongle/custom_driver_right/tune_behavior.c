/*
 * "tune" behavior —— 接收器（dongle）下发参数的入口（右手）
 *
 * 接收器经 split 的 INVOKE_BEHAVIOR 命令下发，本半按 ID 段处理：
 *   0x40–0x7F → TrackPoint 参数（本半）
 *   0x01–0x3F → 左手轨迹球参数（不属本半，静默忽略）
 *   0x00      → SAVE（本半不持久化，dongle 是权威源）
 *   0xFF      → RESET
 *
 * ⚠️ 为什么不走 devicetree 注册：
 *    ZMK 的 BEHAVIOR_DT_DEFINE() 需要 dts/bindings 下的 binding yaml，而
 *    user config 仓库的 dts 不在 Zephyr 的 DTS_ROOT 搜索路径内（只有 ZMK
 *    app 自己的 app/dts 在）。因此这里直接注册 zmk_behavior_ref 条目 ——
 *    这正是 BEHAVIOR_DT_DEFINE() 展开后做的事，其中 node_id 只用于生成
 *    metadata，留空即可（见 app/include/drivers/behavior.h）。
 *    ZMK 外设端执行下发命令时不查 keymap，只用名字查 behavior 表
 *    （app/src/split/peripheral.c 的 INVOKE_BEHAVIOR 分支），故等效。
 *
 * 协议：docs/tuning-protocol.md
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/iterable_sections.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include "trackpoint_config.h"

LOG_MODULE_DECLARE(trackpoint, LOG_LEVEL_INF);

#define TUNE_CMD_SAVE 0x00
#define TUNE_CMD_RESET 0xFF

static int on_tune_binding_pressed(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    uint8_t id = (uint8_t)binding->param1;
    uint32_t value = binding->param2;

    switch (id) {
    case TUNE_CMD_RESET:
        trackpoint_params_reset();
        return ZMK_BEHAVIOR_OPAQUE;
    case TUNE_CMD_SAVE:
        /* dongle 持有权威参数表，本半无需落盘 */
        return ZMK_BEHAVIOR_OPAQUE;
    default:
        break;
    }

    /* 不属于右手段（0x40–0x7F）的 ID 由 trackpoint_param_apply 返回 -1，静默忽略 */
    trackpoint_param_apply(id, value);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_tune_binding_released(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api tune_driver_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = on_tune_binding_pressed,
    .binding_released = on_tune_binding_released,
};

static int tune_init(const struct device *dev) {
    ARG_UNUSED(dev);
    LOG_INF("tune behavior ready (TrackPoint param sink)");
    return 0;
}

/* 设备名 "tune" 必须与 dongle 下发的 behavior_dev 逐字一致 */
DEVICE_DEFINE(zmk_behavior_tune, "tune", tune_init, NULL, NULL, NULL, POST_KERNEL,
              CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &tune_driver_api);

/* 等价于 BEHAVIOR_DEFINE()，但不依赖 devicetree */
static const STRUCT_SECTION_ITERABLE(zmk_behavior_ref, zmk_behavior_tune_ref) = {
    .device = DEVICE_GET(zmk_behavior_tune),
};
