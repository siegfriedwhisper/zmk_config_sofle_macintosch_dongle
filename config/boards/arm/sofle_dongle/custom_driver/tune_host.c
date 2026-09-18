/*
 * tune_host.c —— 接收器（dongle）侧参数调校入口
 *
 * 接收器是参数的权威源：
 *   网页 ──USB CDC-ACM 文本命令──▶ 本文件（权威参数表）
 *        ──split INVOKE_BEHAVIOR──▶ 左右两半的 "tune" behavior
 *
 * 两半按参数 ID 段各自认领（0x01–0x3F 左手轨迹球 / 0x40–0x7F 右手
 * TrackPoint），故广播时无需知道哪个 peripheral 是哪一半，也不依赖槽位
 * 顺序。两半不持久化，每次连上由本机推送全表。
 *
 * 协议详见 docs/tuning-protocol.md
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

#include <zmk/behavior.h>
#include <zmk/split/central.h>

LOG_MODULE_REGISTER(tune_host, LOG_LEVEL_INF);

#define TUNE_BEHAVIOR_NAME "tune"
#define TUNE_CMD_SAVE 0x00
#define TUNE_CMD_RESET 0xFF
#define TUNE_PUSH_GAP_MS 3
#define TUNE_STARTUP_PUSH_DELAY_S 10

/* =========================================================
 * 权威参数表
 * ========================================================= */
struct tune_param {
    uint8_t id;
    const char *name;
    int32_t value;
    int32_t def;
    int32_t min;
    int32_t max;
};

static struct tune_param tune_params[] = {
    /* ---- 左手 · 轨迹球（0x01–0x3F）---- */
    {0x01, "sens", 100, 100, 50, 150},
    {0x02, "wheel", 1, 1, 1, 16},
    {0x03, "arrow_threshold", 4, 4, 1, 32},
    {0x04, "arrow_repeat_ms", 35, 35, 10, 500},
    {0x05, "active_ms", 40, 40, 10, 1000},
    {0x06, "base_move_pixels", 3, 3, 1, 64},

    /* ---- 右手 · TrackPoint（0x40–0x7F）---- */
    {0x41, "tp_base_speed", 50, 50, 1, 300},
    {0x42, "tp_sens_base", 30, 30, 0, 300},
    {0x43, "tp_sens_step", 1, 1, 0, 50},
    {0x44, "tp_scroll_deadzone", 2, 2, 0, 128},
    {0x45, "tp_scroll_input_max", 128, 128, 1, 1024},
    {0x46, "tp_scroll_divisor_slow", 60, 60, 1, 1024},
    {0x47, "tp_scroll_divisor_fast", 8, 8, 1, 1024},
    {0x48, "tp_dominant_num", 3, 3, 1, 64},
    {0x49, "tp_dominant_den", 2, 2, 1, 64},
    {0x4A, "tp_scroll_x_dir", 1, 1, -1, 1},
    {0x4B, "tp_scroll_y_dir", 1, 1, -1, 1},
    {0x4C, "tp_exponential", 1, 1, 0, 1},
    {0x4D, "tp_exp_slope", 1307, 1307, 0, 5000},
    {0x4E, "tp_exp_max_mult", 200, 200, 50, 1000},
    {0x4F, "tp_slow_key_mult", 50, 50, 0, 100},
};

#define TUNE_PARAM_COUNT ARRAY_SIZE(tune_params)

/* =========================================================
 * 下发：广播给所有 peripheral
 * ========================================================= */
static void tune_push_one(uint8_t id, int32_t value) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = TUNE_BEHAVIOR_NAME,
        .param1 = id,
        .param2 = (uint32_t)value,
    };
    struct zmk_behavior_binding_event event = {
        .layer = 0,
        .position = 0,
        .timestamp = k_uptime_get(),
    };

    for (uint8_t i = 0; i < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; i++) {
        int err = zmk_split_central_invoke_behavior(i, &binding, event, true);
        if (err) {
            LOG_DBG("tune: peripheral %u unreachable (%d)", i, err);
        }
    }
}

static void tune_push_all(void) {
    for (size_t i = 0; i < TUNE_PARAM_COUNT; i++) {
        tune_push_one(tune_params[i].id, tune_params[i].value);
        k_sleep(K_MSEC(TUNE_PUSH_GAP_MS));
    }
    LOG_INF("Pushed %u params to peripherals", (unsigned)TUNE_PARAM_COUNT);
}

/* =========================================================
 * 持久化：接收器自己记住参数（两半不落盘）
 * ========================================================= */
static int tune_settings_set(const char *key, size_t len_rd, settings_read_cb read_cb,
                             void *cb_arg) {
    if (strcmp(key, "vals") != 0) {
        return -ENOENT;
    }

    int32_t vals[TUNE_PARAM_COUNT];
    if (len_rd != sizeof(vals)) {
        LOG_ERR("Invalid tune vals size: %zu (expect %zu)", len_rd, sizeof(vals));
        return -EINVAL;
    }

    ssize_t rc = read_cb(cb_arg, vals, sizeof(vals));
    if (rc < 0) {
        LOG_ERR("Failed to read tune vals: %zd", rc);
        return rc;
    }

    for (size_t i = 0; i < TUNE_PARAM_COUNT; i++) {
        if (vals[i] >= tune_params[i].min && vals[i] <= tune_params[i].max) {
            tune_params[i].value = vals[i];
        }
    }
    LOG_INF("Loaded %u tune params", (unsigned)TUNE_PARAM_COUNT);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(tune_host_settings, "tune", NULL, tune_settings_set, NULL, NULL);

static int tune_save(void) {
    int32_t vals[TUNE_PARAM_COUNT];
    for (size_t i = 0; i < TUNE_PARAM_COUNT; i++) {
        vals[i] = tune_params[i].value;
    }

    int rc = settings_save_one("tune/vals", vals, sizeof(vals));
    if (rc) {
        LOG_ERR("Failed to save tune params: %d", rc);
    } else {
        LOG_INF("Saved tune params");
    }
    return rc;
}

static void tune_reset(void) {
    for (size_t i = 0; i < TUNE_PARAM_COUNT; i++) {
        tune_params[i].value = tune_params[i].def;
    }
}

/* =========================================================
 * USB CDC-ACM 文本命令通道
 *   GET              → 列出全部参数
 *   SET <key> <val>  → 设置一个并下发（key 可为名字或 ID）
 *   SETALL           → 全表下发
 *   SAVE             → 持久化到 flash
 *   RESET            → 恢复出厂默认并全表下发
 *   STATUS           → 显示 parameter id 区间与 peripheral 数
 * ========================================================= */
#define CMD_BUF_SIZE 96

static const struct device *cdc_dev;

static void serial_send(const char *s) {
    if (!cdc_dev) {
        return;
    }
    while (*s) {
        uart_poll_out(cdc_dev, *s++);
    }
    uart_poll_out(cdc_dev, '\r');
    uart_poll_out(cdc_dev, '\n');
}

static void cmd_get(void) {
    char buf[CMD_BUF_SIZE];
    for (size_t i = 0; i < TUNE_PARAM_COUNT; i++) {
        snprintf(buf, sizeof(buf), "%s=%d", tune_params[i].name, (int)tune_params[i].value);
        serial_send(buf);
    }
    serial_send("OK");
}

/* key 支持参数名（tp_base_speed）或 ID（0x41 / 65） */
static struct tune_param *find_param(const char *key) {
    for (size_t i = 0; i < TUNE_PARAM_COUNT; i++) {
        if (strcmp(key, tune_params[i].name) == 0) {
            return &tune_params[i];
        }
    }

    char *end;
    long v = strtol(key, &end, 0);
    if (end != key && *end == '\0') {
        for (size_t i = 0; i < TUNE_PARAM_COUNT; i++) {
            if (tune_params[i].id == (uint8_t)v) {
                return &tune_params[i];
            }
        }
    }
    return NULL;
}

static void cmd_set(char *args) {
    char *key = strtok(args, " \r\n");
    char *val_s = strtok(NULL, " \r\n");
    if (!key || !val_s) {
        serial_send("ERR usage: SET key value");
        return;
    }

    struct tune_param *p = find_param(key);
    if (!p) {
        serial_send("ERR unknown key");
        return;
    }

    char *end;
    long v = strtol(val_s, &end, 0);
    if (end == val_s || *end != '\0') {
        serial_send("ERR bad value");
        return;
    }
    if (v < p->min || v > p->max) {
        serial_send("ERR out of range");
        return;
    }

    p->value = (int32_t)v;
    tune_push_one(p->id, p->value);
    serial_send("OK");
}

static void cmd_status(void) {
    char buf[CMD_BUF_SIZE];
    snprintf(buf, sizeof(buf), "params=%u", (unsigned)TUNE_PARAM_COUNT);
    serial_send(buf);
    snprintf(buf, sizeof(buf), "peripherals=%u", (unsigned)ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT);
    serial_send(buf);
    serial_send("OK");
}

static void cmd_process(char *line) {
    char *cmd = strtok(line, " \r\n");
    if (!cmd) {
        return;
    }

    if (strcmp(cmd, "GET") == 0) {
        cmd_get();
    } else if (strcmp(cmd, "SET") == 0) {
        char *args = strtok(NULL, "");
        cmd_set(args);
    } else if (strcmp(cmd, "SETALL") == 0) {
        tune_push_all();
        serial_send("OK");
    } else if (strcmp(cmd, "SAVE") == 0) {
        serial_send(tune_save() == 0 ? "OK" : "ERR save failed");
    } else if (strcmp(cmd, "RESET") == 0) {
        tune_reset();
        tune_push_all();
        serial_send("OK");
    } else if (strcmp(cmd, "STATUS") == 0) {
        cmd_status();
    } else {
        serial_send("ERR unknown command");
    }
}

/* 串口读取线程：轮询 CDC-ACM，按行解析 */
static void serial_thread(void *p1, void *p2, void *p3) {
    char line[CMD_BUF_SIZE];
    size_t pos = 0;

    while (1) {
        if (cdc_dev) {
            uint8_t c;
            if (uart_poll_in(cdc_dev, &c) == 0) {
                if (c == '\n' || c == '\r') {
                    if (pos > 0) {
                        line[pos] = '\0';
                        cmd_process(line);
                        pos = 0;
                    }
                } else if (pos < sizeof(line) - 1) {
                    line[pos++] = (char)c;
                }
            }
        }
        k_sleep(K_MSEC(5));
    }
}

K_THREAD_STACK_DEFINE(tune_serial_stack, 2048);
static struct k_thread tune_serial_thr;

/* 上电后延时推送一次全表（此时 BLE 分体链路应已建立）。
 * 网页端每次连上也会主动发 SETALL 兜底。 */
static void tune_startup_push(struct k_work *work) {
    ARG_UNUSED(work);
    tune_push_all();
}
static K_WORK_DELAYABLE_DEFINE(tune_startup_work, tune_startup_push);

static int tune_host_init(void) {
    cdc_dev = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart));

    k_thread_create(&tune_serial_thr, tune_serial_stack, K_THREAD_STACK_SIZEOF(tune_serial_stack),
                    serial_thread, NULL, NULL, NULL, 7, 0, K_NO_WAIT);

    k_work_schedule(&tune_startup_work, K_SECONDS(TUNE_STARTUP_PUSH_DELAY_S));

    LOG_INF("tune host ready: %u params", (unsigned)TUNE_PARAM_COUNT);
    return 0;
}

SYS_INIT(tune_host_init, POST_KERNEL, 90);
