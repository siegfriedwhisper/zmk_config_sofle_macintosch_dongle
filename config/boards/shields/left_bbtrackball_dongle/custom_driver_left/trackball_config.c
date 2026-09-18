/*
 * trackball_config.c
 * BB Trackball 运行时参数配置
 *  - ZMK settings 持久化
 *  - USB CDC-ACM 串口命令通道（网页 Web Serial 调参）
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "trackball_config.h"

LOG_MODULE_REGISTER(trackball_config, LOG_LEVEL_INF);

/* =========================================================
 * 运行时参数（默认 = 当前 #define 手感）
 * ========================================================= */
trackball_params_t g_params = TRACKBALL_PARAMS_DEFAULT;

/* =========================================================
 * 物理查表（原始值，delta(ms) -> delta_px）
 * delta=0 占位；1-44 真实值；45+ 兜底 base_move_pixels
 * ========================================================= */
const uint16_t speed_lut_base[SPEED_LUT_SIZE] = {
    0,
    /* 1-44 */
    2693, 90, 29, 16, 12, 9, 8, 7, 6, 6, 6, 5,
    5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4,
};

/* 缩放后的查表（中断只读此表，sens 调参时重建一次） */
uint16_t g_speed_lut_eff[SPEED_LUT_SIZE];

void trackball_config_rebuild_lut(void) {
    uint32_t sens = g_params.sens_percent;
    for (int i = 0; i < SPEED_LUT_SIZE; i++) {
        g_speed_lut_eff[i] = (uint16_t)((uint32_t)speed_lut_base[i] * sens / 100u);
    }
    LOG_INF("Rebuilt speed LUT with sens=%u%%", g_params.sens_percent);
}

/* =========================================================
 * ZMK settings 持久化（仿 st7789 snake/settings 模式）
 * ========================================================= */
static int trackball_settings_set(const char *key, size_t len_rd,
                                  settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(key, "params") != 0) {
        return -ENOENT;
    }
    if (len_rd != sizeof(g_params)) {
        LOG_ERR("Invalid params size: %zu", len_rd);
        return -EINVAL;
    }
    ssize_t rc = read_cb(cb_arg, &g_params, sizeof(g_params));
    if (rc < 0) {
        LOG_ERR("Failed to read params: %zd", rc);
        return rc;
    }
    LOG_INF("Loaded trackball params: sens=%u wheel=%u thr=%u rep=%u act=%u base=%u",
            g_params.sens_percent, g_params.wheel_units, g_params.arrow_threshold,
            g_params.arrow_repeat_ms, g_params.active_ms, g_params.base_move_pixels);
    trackball_config_rebuild_lut();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(trackball_settings, "trackball", NULL,
                               trackball_settings_set, NULL, NULL);

int trackball_params_save(void) {
    int rc = settings_save_one("trackball/params", &g_params, sizeof(g_params));
    if (rc) {
        LOG_ERR("Failed to save params: %d", rc);
    } else {
        LOG_INF("Saved trackball params");
    }
    return rc;
}

/* =========================================================
 * USB CDC-ACM 串口命令通道
 * 协议（每行以 \r\n 结尾）：
 *   GET           → 返回所有参数
 *   SET key val   → 设置单个参数
 *   SAVE          → 持久化到 flash
 *   RESET         → 恢复默认
 * ========================================================= */

#define CMD_BUF_SIZE 64

static const struct device *cdc_dev;

static void serial_send(const char *s) {
    if (!cdc_dev)
        return;
    while (*s) {
        uart_poll_out(cdc_dev, *s++);
    }
    uart_poll_out(cdc_dev, '\r');
    uart_poll_out(cdc_dev, '\n');
}

static void serial_send_param(const char *name, uint32_t val) {
    char buf[CMD_BUF_SIZE];
    snprintf(buf, sizeof(buf), "%s=%u", name, val);
    serial_send(buf);
}

static void cmd_get(void) {
    serial_send_param("sens", g_params.sens_percent);
    serial_send_param("wheel", g_params.wheel_units);
    serial_send_param("arrow_threshold", g_params.arrow_threshold);
    serial_send_param("arrow_repeat_ms", g_params.arrow_repeat_ms);
    serial_send_param("active_ms", g_params.active_ms);
    serial_send_param("base_move_pixels", g_params.base_move_pixels);
    serial_send("OK");
}

/* 参数名 → 字段设置函数，返回 0 成功 */
typedef int (*setter_fn)(const char *val_str);

static int set_sens(const char *v) {
    long val = strtol(v, NULL, 10);
    if (val < 50 || val > 150)
        return -1;
    g_params.sens_percent = (uint8_t)val;
    trackball_config_rebuild_lut();
    return 0;
}
static int set_wheel(const char *v) {
    long val = strtol(v, NULL, 10);
    if (val < 1 || val > 16)
        return -1;
    g_params.wheel_units = (uint8_t)val;
    return 0;
}
static int set_arrow_threshold(const char *v) {
    long val = strtol(v, NULL, 10);
    if (val < 1 || val > 32)
        return -1;
    g_params.arrow_threshold = (uint8_t)val;
    return 0;
}
static int set_arrow_repeat(const char *v) {
    long val = strtol(v, NULL, 10);
    if (val < 10 || val > 500)
        return -1;
    g_params.arrow_repeat_ms = (uint16_t)val;
    return 0;
}
static int set_active(const char *v) {
    long val = strtol(v, NULL, 10);
    if (val < 10 || val > 1000)
        return -1;
    g_params.active_ms = (uint16_t)val;
    return 0;
}
static int set_base(const char *v) {
    long val = strtol(v, NULL, 10);
    if (val < 1 || val > 64)
        return -1;
    g_params.base_move_pixels = (uint8_t)val;
    return 0;
}

struct param_entry {
    const char *name;
    setter_fn set;
};
static const struct param_entry param_table[] = {
    {"sens", set_sens},
    {"wheel", set_wheel},
    {"arrow_threshold", set_arrow_threshold},
    {"arrow_repeat_ms", set_arrow_repeat},
    {"active_ms", set_active},
    {"base_move_pixels", set_base},
};

static void cmd_set(char *args) {
    /* 解析 "key value" */
    char *key = strtok(args, " \r\n");
    char *val = strtok(NULL, " \r\n");
    if (!key || !val) {
        serial_send("ERR usage: SET key value");
        return;
    }
    for (size_t i = 0; i < ARRAY_SIZE(param_table); i++) {
        if (strcmp(key, param_table[i].name) == 0) {
            if (param_table[i].set(val) == 0) {
                serial_send("OK");
            } else {
                serial_send("ERR value out of range");
            }
            return;
        }
    }
    serial_send("ERR unknown key");
}

static void cmd_process(char *line) {
    char *cmd = strtok(line, " \r\n");
    if (!cmd)
        return;
    if (strcmp(cmd, "GET") == 0) {
        cmd_get();
    } else if (strcmp(cmd, "SET") == 0) {
        char *args = strtok(NULL, "");
        cmd_set(args);
    } else if (strcmp(cmd, "SAVE") == 0) {
        if (trackball_params_save() == 0) {
            serial_send("OK");
        } else {
            serial_send("ERR save failed");
        }
    } else if (strcmp(cmd, "RESET") == 0) {
        g_params = (trackball_params_t)TRACKBALL_PARAMS_DEFAULT;
        trackball_config_rebuild_lut();
        trackball_params_save();
        serial_send("OK");
    } else {
        serial_send("ERR unknown command");
    }
}

/* 串口读取线程：轮询 CDC-ACM，按行解析 */
static void serial_thread(void *p1, void *p2, void *p3) {
    char line[CMD_BUF_SIZE];
    int pos = 0;
    uint32_t dtr = 0;

    /* 等 CDC-ACM 设备 ready（USB 栈起来后） */
    while (!device_is_ready(cdc_dev)) {
        k_sleep(K_MSEC(100));
    }

    /* 等 DTR 置位（网页/串口工具连上才发数据） */
    while (true) {
        if (uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_DTR, &dtr) == 0 && dtr) {
            break;
        }
        k_sleep(K_MSEC(100));
    }
    uart_line_ctrl_set(cdc_dev, UART_LINE_CTRL_DTR, 1);
    uart_line_ctrl_set(cdc_dev, UART_LINE_CTRL_RTS, 1);

    LOG_INF("USB CDC serial ready");
    serial_send("trackball-tuner v1");

    while (true) {
        uint8_t c;
        while (uart_poll_in(cdc_dev, &c) == 0) {
            if (c == '\n') {
                line[pos] = '\0';
                pos = 0;
                if (line[0] != '\0') {
                    cmd_process(line);
                }
            } else if (c != '\r') {
                if (pos < (int)sizeof(line) - 1) {
                    line[pos++] = c;
                }
            }
        }
        k_sleep(K_MSEC(5));
    }
}

K_THREAD_STACK_DEFINE(serial_stack, 2048);
static struct k_thread serial_thr;

/* =========================================================
 * Init（POST_KERNEL：保证查表先于轨迹球驱动建好）
 * ========================================================= */
static int trackball_config_init(void) {
    cdc_dev = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart));

    /* 先重建默认 LUT，settings 加载后会自动覆盖 */
    trackball_config_rebuild_lut();

    k_thread_create(&serial_thr, serial_stack, K_THREAD_STACK_SIZEOF(serial_stack),
                    serial_thread, NULL, NULL, NULL, 7, 0, K_NO_WAIT);

    return 0;
}

SYS_INIT(trackball_config_init, POST_KERNEL, 90);