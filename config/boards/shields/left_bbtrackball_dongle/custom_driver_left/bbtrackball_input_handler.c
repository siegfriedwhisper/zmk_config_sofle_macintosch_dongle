/*
 * bbtrackball_input_handler.c
 * BB Trackball FULL interrupt-driven version
 *
 * + Dedicated workqueue version (NO system workqueue)
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_bbtrackball

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/input/input.h>

#include <stdlib.h>

#include <zmk/events/position_state_changed.h>

#include "trackball_config.h"

LOG_MODULE_REGISTER(bbtrackball_input_handler, LOG_LEVEL_INF);

/* =========================================================
 * Workqueue config  ⭐⭐⭐新增
 * ========================================================= */

#define BBTRACKBALL_WORKQ_STACK_SIZE 2048
#define BBTRACKBALL_WORKQ_PRIORITY 5

K_THREAD_STACK_DEFINE(bbtrackball_workq_stack, BBTRACKBALL_WORKQ_STACK_SIZE);
static struct k_work_q bbtrackball_work_q;

/* =========================================================
 * GPIO Pins
 * ========================================================= */

#define DOWN_GPIO_PIN 9
#define LEFT_GPIO_PIN 12
#define UP_GPIO_PIN 5
#define RIGHT_GPIO_PIN 27

#define GPIO0_DEV DT_NODELABEL(gpio0)
#define GPIO1_DEV DT_NODELABEL(gpio1)

/* =========================================================
 * Config
 * ========================================================= */

/* keymap 中对应按键的位置号（非运行时参数，保持宏） */
#define ARROW_KEY_POSITION 32
#define SPACE_KEY_POSITION 61

/* 查表与运行时参数在 trackball_config.h/c 中定义 */

/* =========================================================
 * Runtime State
 * ========================================================= */

static bool space_pressed = false;
static bool arrow_key_pressed = false;

static int dx_acc = 0;
static int dy_acc = 0;

static uint32_t last_move_time = 0;
static uint32_t last_arrow_trigger = 0;

/* 滚轮余量：一次只发 1 档，剩余暂存下次继续发 */
static int wheel_residue_x = 0;
static int wheel_residue_y = 0;

typedef struct {
    const struct device *gpio_dev;
    int pin;
    int last_state;
    uint32_t last_time;
    int sign;
} DirInput;

static DirInput dir_inputs[] = {
    {DEVICE_DT_GET(GPIO0_DEV), LEFT_GPIO_PIN, 1, 0, -1},
    {DEVICE_DT_GET(GPIO0_DEV), RIGHT_GPIO_PIN, 1, 0, +1},
    {DEVICE_DT_GET(GPIO0_DEV), UP_GPIO_PIN, 1, 0, -1},
    {DEVICE_DT_GET(GPIO1_DEV), DOWN_GPIO_PIN, 1, 0, +1},
};

/* ========================================================= */

struct bbtrackball_dev_config {
    uint16_t x_input_code;
    uint16_t y_input_code;
};

struct bbtrackball_data;

/* ========================================================= */

struct bb_gpio_cb {
    struct gpio_callback cb;
    struct bbtrackball_data *parent;
};

struct bbtrackball_data {
    const struct device *dev;
    struct k_work work;
    struct bb_gpio_cb gpio_cbs[ARRAY_SIZE(dir_inputs)];
};

/* ========================================================= */

bool trackball_is_active(void) { return (k_uptime_get_32() - last_move_time) < g_params.active_ms; }

/* =========================================================
 * Position listener
 * ========================================================= */

static int space_listener_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (!ev)
        return 0;

    if (ev->position == ARROW_KEY_POSITION) {
        arrow_key_pressed = ev->state;
    }

    if (ev->position == SPACE_KEY_POSITION) {
        space_pressed = ev->state;
    }

    return 0;
}

ZMK_LISTENER(space_listener, space_listener_cb);
ZMK_SUBSCRIPTION(space_listener, zmk_position_state_changed);

/* =========================================================
 * GPIO interrupt callback
 * ========================================================= */

static void dir_edge_cb(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {

    struct bb_gpio_cb *wrapper = CONTAINER_OF(cb, struct bb_gpio_cb, cb);
    struct bbtrackball_data *data = wrapper->parent;

    for (size_t i = 0; i < ARRAY_SIZE(dir_inputs); i++) {

        DirInput *d = &dir_inputs[i];

        if ((dev == d->gpio_dev) && (pins & BIT(d->pin))) {

            int val = gpio_pin_get(dev, d->pin);

            if (val != d->last_state) {

                uint32_t now = k_uptime_get_32();
                uint32_t delta = now - d->last_time;
                if (delta == 0)
                    delta = 1;

                int delta_px = (delta < SPEED_LUT_SIZE) ? g_speed_lut_eff[delta]
                                                        : g_params.base_move_pixels;

                if (i < 2)
                    dx_acc += d->sign * delta_px;
                else
                    dy_acc += d->sign * delta_px;

                d->last_state = val;
                d->last_time = now;

                if (!k_work_is_pending(&data->work)) {
                    k_work_submit_to_queue(&bbtrackball_work_q, &data->work); // ⭐修改点
                }
            }
        }
    }
}

/* =========================================================
 * Work Handler
 * ========================================================= */

static void bbtrackball_work_handler(struct k_work *work) {

    struct bbtrackball_data *data = CONTAINER_OF(work, struct bbtrackball_data, work);
    const struct device *dev = data->dev;

    uint32_t now = k_uptime_get_32();

    int dx = dx_acc;
    int dy = dy_acc;

    dx_acc = 0;
    dy_acc = 0;

    if (dx == 0 && dy == 0 && wheel_residue_x == 0 && wheel_residue_y == 0) {
        return;
    }

    last_move_time = now;

    if (arrow_key_pressed) {

        int abs_dx = abs(dx);
        int abs_dy = abs(dy);

        if (abs_dx < g_params.arrow_threshold && abs_dy < g_params.arrow_threshold) {
            return;
        }

        if (now - last_arrow_trigger < g_params.arrow_repeat_ms) {
            return;
        }

        last_arrow_trigger = now;

        uint16_t key = 0;

        if (abs_dx > abs_dy)
            key = (dx > 0) ? INPUT_BTN_1 : INPUT_BTN_0;
        else
            key = (dy > 0) ? INPUT_BTN_3 : INPUT_BTN_2;

        input_report_key(dev, key, 1, false, K_NO_WAIT);
        input_report_key(dev, key, 0, true, K_NO_WAIT);

        return;
    }

    if (space_pressed) {
        input_report_rel(dev, INPUT_REL_X, -dx, false, K_NO_WAIT);
        input_report_rel(dev, INPUT_REL_Y, -dy, true, K_NO_WAIT);
        return;
    }

    /* 滚轮模式：每次至多发 wheel_units 档，余量留 residue 下次继续 */
    wheel_residue_x += dx;
    wheel_residue_y += dy;

    int units = g_params.wheel_units;

    if (wheel_residue_x > 0) {
        int n = (wheel_residue_x > units) ? units : wheel_residue_x;
        input_report_rel(dev, INPUT_REL_HWHEEL, -n, false, K_NO_WAIT);
        wheel_residue_x -= n;
    } else if (wheel_residue_x < 0) {
        int n = (-wheel_residue_x > units) ? units : -wheel_residue_x;
        input_report_rel(dev, INPUT_REL_HWHEEL, n, false, K_NO_WAIT);
        wheel_residue_x += n;
    }

    if (wheel_residue_y > 0) {
        int n = (wheel_residue_y > units) ? units : wheel_residue_y;
        input_report_rel(dev, INPUT_REL_WHEEL, n, true, K_NO_WAIT);
        wheel_residue_y -= n;
    } else if (wheel_residue_y < 0) {
        int n = (-wheel_residue_y > units) ? units : -wheel_residue_y;
        input_report_rel(dev, INPUT_REL_WHEEL, -n, true, K_NO_WAIT);
        wheel_residue_y += n;
    }

    /* 若还有余量且无新中断 pending，主动再调度继续消化 */
    if ((wheel_residue_x != 0 || wheel_residue_y != 0) && !k_work_is_pending(&data->work)) {
        k_work_submit_to_queue(&bbtrackball_work_q, &data->work);
    }
    
}

/* =========================================================
 * Init
 * ========================================================= */

static int bbtrackball_init(const struct device *dev) {

    struct bbtrackball_data *data = dev->data;

    LOG_INF("Initializing BBtrackball");

    data->dev = dev;

    /* ⭐ 启动独立 workqueue */
    k_work_queue_start(&bbtrackball_work_q, bbtrackball_workq_stack,
                       K_THREAD_STACK_SIZEOF(bbtrackball_workq_stack), BBTRACKBALL_WORKQ_PRIORITY,
                       NULL);

    k_work_init(&data->work, bbtrackball_work_handler);

    for (size_t i = 0; i < ARRAY_SIZE(dir_inputs); i++) {

        DirInput *d = &dir_inputs[i];

        gpio_pin_configure(d->gpio_dev, d->pin, GPIO_INPUT | GPIO_PULL_UP);

        d->last_state = gpio_pin_get(d->gpio_dev, d->pin);
        d->last_time = k_uptime_get_32();

        data->gpio_cbs[i].parent = data;

        gpio_init_callback(&data->gpio_cbs[i].cb, dir_edge_cb, BIT(d->pin));
        gpio_add_callback(d->gpio_dev, &data->gpio_cbs[i].cb);

        gpio_pin_interrupt_configure(d->gpio_dev, d->pin, GPIO_INT_EDGE_BOTH);
    }

    return 0;
}

/* ========================================================= */

#define BBTRACKBALL_INIT_PRIORITY CONFIG_INPUT_INIT_PRIORITY

#define BBTRACKBALL_DEFINE(inst)                                                                   \
    static struct bbtrackball_data bbtrackball_data_##inst;                                        \
                                                                                                   \
    static const struct bbtrackball_dev_config bbtrackball_config_##inst = {                       \
        .x_input_code = DT_PROP_OR(DT_DRV_INST(inst), x_input_code, INPUT_REL_X),                  \
        .y_input_code = DT_PROP_OR(DT_DRV_INST(inst), y_input_code, INPUT_REL_Y),                  \
    };                                                                                             \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(inst, bbtrackball_init, NULL, &bbtrackball_data_##inst,                  \
                          &bbtrackball_config_##inst, POST_KERNEL, BBTRACKBALL_INIT_PRIORITY,      \
                          NULL);

DT_INST_FOREACH_STATUS_OKAY(BBTRACKBALL_DEFINE);
