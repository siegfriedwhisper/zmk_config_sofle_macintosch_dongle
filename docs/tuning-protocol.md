# 手感参数实时调参协议 v1

> 目标：**两半固件从今往后不再需要刷**，所有手感参数改动都经接收器（dongle）实时下发。

## 架构

```
网页  ──USB CDC-ACM 串口──▶  dongle  ──BLE split / INVOKE_BEHAVIOR──▶  左手 / 右手
                                              （广播）
```

- **dongle = 参数权威源**：持有完整参数表，持久化在自己的 flash 里；网页只跟 dongle 对话。
- **两半 = 执行端**：收到参数后写入运行时变量立即生效，**不需要自己持久化**。
- 两半连上 dongle 时，dongle 自动把全表推一遍 —— 所以两半断电、重刷、换固件都不会丢参数。

## 为什么借道 behavior

ZMK 的 split 下发命令只有 4 种（`app/include/zmk/split/transport/types.h`）：

```
POLL_EVENTS / INVOKE_BEHAVIOR / SET_PHYSICAL_LAYOUT / SET_HID_INDICATORS
```

没有通用参数通道。但 `INVOKE_BEHAVIOR` 自带：

```c
char     behavior_dev[16];
uint32_t param1, param2;
```

正好当参数通道用。dongle 侧调 `zmk_split_central_invoke_behavior()`（公开 API），
两半的 `tune` behavior 被触发时把 `param1`(参数ID) / `param2`(值) 写进运行时变量。

**不改 ZMK 源码**，纯用现成机制。

## 参数 ID 表

左右手靠 **ID 分段**区分（不依赖"谁先连上占 slot 0"这种不确定的事）。
dongle 一律广播，每半只认自己那一段。

### 左手 · 轨迹球（BB trackball）`0x01–0x3F`

| ID | 名称 | 默认 | 建议范围 | 说明 |
|---|---|---|---|---|
| 0x01 | `sens` | 100 | 50–150 | 查表缩放强度(%) |
| 0x02 | `wheel` | 1 | 1–16 | 滚轮单次档数 |
| 0x03 | `arrow_threshold` | 4 | 1–32 | 方向键触发阈值 |
| 0x04 | `arrow_repeat_ms` | 35 | 10–500 | 方向键重复间隔 |
| 0x05 | `active_ms` | 40 | 10–1000 | 移动判定窗(LED点亮时长) |
| 0x06 | `base_move_pixels` | 3 | 1–64 | 查表越界兜底 |

### 右手 · TrackPoint（小红点）`0x40–0x7F`

| ID | 名称 | 默认 | 单位/编码 | 原 Kconfig |
|---|---|---|---|---|
| 0x41 | `mouse_base_speed` | 50 | % | `TRACKPOINT_MOUSE_BASE_SPEED_PERCENT` |
| 0x42 | `mouse_sens_base` | 30 | % | `TRACKPOINT_MOUSE_SENS_BASE_PERCENT` |
| 0x43 | `mouse_sens_step` | 1 | % | `TRACKPOINT_MOUSE_SENS_STEP_PERCENT` |
| 0x44 | `scroll_deadzone` | 2 | | `TRACKPOINT_SCROLL_DEADZONE` |
| 0x45 | `scroll_input_max` | 128 | | `TRACKPOINT_SCROLL_INPUT_MAX` |
| 0x46 | `scroll_divisor_slow` | 60 | | `TRACKPOINT_SCROLL_DIVISOR_SLOW` |
| 0x47 | `scroll_divisor_fast` | 8 | | `TRACKPOINT_SCROLL_DIVISOR_FAST` |
| 0x48 | `dominant_num` | 3 | | `TRACKPOINT_DOMINANT_NUMERATOR` |
| 0x49 | `dominant_den` | 2 | | `TRACKPOINT_DOMINANT_DENOMINATOR` |
| 0x4A | `scroll_x_dir` | 1 | 1/-1 | `TRACKPOINT_SCROLL_X_DIR` |
| 0x4B | `scroll_y_dir` | 1 | 1/-1 | `TRACKPOINT_SCROLL_Y_DIR` |
| 0x4C | `exponential` | 1 | 0/1 | `TRACKPOINT_EXPONENTIAL` |
| 0x4D | `exp_slope` | 1307 | ×1000 | 硬编码 `1.307357f` |
| 0x4E | `exp_max_mult` | 200 | ×100 | 硬编码 `TP_MAX_MULT 2.0f` |
| 0x4F | `slow_key_mult` | 50 | ×100 | 硬编码 `SLOW_KEY_MULTIPLIER 0.5f` |

> `0x4D`–`0x4F` 三个原本连 Kconfig 都没有，是写死在 `.c` 里的手感核心。
>
> **名称前缀**：右手段参数在 dongle 侧统一带 `tp_` 前缀（`tp_base_speed`、`tp_scroll_deadzone`…），
> 避免与左手同名参数混淆；命令里用 ID 或这个名字都可以。

### 控制命令（两半共用）

| ID | 名称 | 值 | 说明 |
|---|---|---|---|
| 0x00 | `SAVE` | 忽略 | 两半把当前参数写自己的 flash（可选，dongle 才是权威） |
| 0xFF | `RESET` | 忽略 | 两半恢复出厂默认 |

## 下发机制

dongle 侧：

```c
struct zmk_behavior_binding b = {
    .behavior_dev = "tune",
    .param1 = param_id,
    .param2 = value,
};
struct zmk_behavior_binding_event ev = {
    .position = 0,
    .layer = 0,
    .timestamp = k_uptime_get(),
};
for (int src = 0; src < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; src++) {
    zmk_split_central_invoke_behavior(src, &b, ev, true);
}
```

两半侧各注册一个名字叫 `tune` 的 behavior（`custom_driver_left/tune_behavior.c` /
`custom_driver_right/tune_behavior.c`）。

**不走 devicetree 注册**：ZMK 的 `BEHAVIOR_DT_DEFINE()` 需要 `dts/bindings/` 下的 binding yaml，
而 user config 仓库的 dts 不在 Zephyr 的 `DTS_ROOT` 搜索路径内（只有 ZMK app 自己的 `app/dts`
在内）。所以直接注册 `zmk_behavior_ref` 条目 —— 这正是 `BEHAVIOR_DT_DEFINE()` 展开后做的事，
其中 `node_id` 只用于生成 metadata，省略即可（`ZMK_BEHAVIOR_METADATA` 默认为 n）。
ZMK 外设端执行下发命令时不查 keymap，只用名字查 behavior 表
（`app/src/split/peripheral.c` 的 `INVOKE_BEHAVIOR` 分支），故等效。

```c
static const struct behavior_driver_api tune_driver_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = on_tune_binding_pressed,
    .binding_released = on_tune_binding_released,
};
DEVICE_DEFINE(zmk_behavior_tune, "tune", tune_init, NULL, NULL, NULL, POST_KERNEL,
              CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &tune_driver_api);
/* 等价于 BEHAVIOR_DEFINE()，不依赖 devicetree */
static const STRUCT_SECTION_ITERABLE(zmk_behavior_ref, zmk_behavior_tune_ref) = {
    .device = DEVICE_GET(zmk_behavior_tune),
};
```

设备名 `"tune"` 必须与 dongle 下发的 `behavior_dev` 逐字一致。

`binding_pressed` 里按 ID 段判断：左手只处理 `0x01–0x3F`，右手只处理 `0x40–0x7F`，
越界静默忽略（各自的 `*_param_apply()` 返回 -1）。值用 `uint32` 原样承载 `int32` 位模式。

## 串口协议（dongle 的 CDC-ACM）

文本行协议，`\r\n` 结尾（沿用左手原调参串口的风格）：

| 命令 | 返回 | 说明 |
|---|---|---|
| `GET` | 全表 `name=value` 逐行 + `OK` | 读 dongle 权威表的当前值 |
| `SET <key> <value>` | `OK` / `ERR ...` | 改一个参数并**立即广播**到两半；`key` 可为参数名或 ID（`0x41` / `65`） |
| `SETALL` | `OK` | 把全表重推一遍 |
| `SAVE` | `OK` | 把权威表写入 dongle flash |
| `RESET` | `OK` | 恢复出厂默认并全表广播 |
| `STATUS` | `params=` / `peripherals=` + `OK` | 参数条数与分体槽位数 |

值支持负数（`tp_scroll_x_dir` 取 `1` / `-1`）。范围校验在 dongle 侧完成，越界返回 `ERR out of range`。

## 同步策略

1. 网页改一个值 → `SET` → dongle 更新权威表 → 广播 → 两半立即生效
2. `SAVE` → dongle 写自己的 flash（两半不写）
3. **监听 `zmk_split_peripheral_status_changed`**：任一分体连上 → 延时 800ms 推全表（等 BLE 链路稳）
4. dongle 上电 10 秒后再推一次作兜底（防止两半先于 dongle 就绪）
5. 网页每次连上先发 `SETALL`，保证三方一致

⇒ 两半断电、重启、重刷固件、`settings_reset` 都不影响手感，连上即自动恢复。

## 落地清单

- [x] 右手：15 个参数从 Kconfig/硬编码迁到运行时 `g_tp_params`（宏名不变，调用点零改动）
- [x] 右手：新增 `trackpoint_config.c/h` + `tune` behavior
- [x] 左手：新增 `trackball_param_apply()` / `trackball_params_reset()` + `tune` behavior
      （原 USB 串口调参通道**保留**，可作左手本地调试入口）
- [x] dongle：CDC-ACM 串口协议 + 权威参数表 + `zmk_split_central_invoke_behavior` 广播
- [x] dongle：外设连接事件触发全表推送
- [ ] 网页：扩成两半统一面板
- [ ] 刷三块板 + 真机验收
