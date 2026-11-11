# ANCS-ESP32

通过 BLE 连接 iPhone，使用 Apple **ANCS**（Apple Notification Center Service）获取手机通知，打印到串口控制台，并可通过 **Webhook** 将通知 POST 到其他服务器。

## 功能

- BLE 外设广播 ANCS Service Solicitation，iPhone「设置 > 蓝牙」中可直接看到并连接
- Just Works 绑定配对（含 IRK，兼容 iPhone 随机地址），重启后自动重连
- 完整 ANCS 客户端：服务发现 → 订阅 → 获取通知属性（App、标题、副标题、正文、时间、操作按钮）→ 查询 App 显示名称（带缓存）
- Data Source 分包重组，Control Point 请求串行队列 + 超时
- Wi-Fi：保存多个 SSID，无配置或连接超时自动开启配网热点
- Webhook：独立任务 + 队列，Wi-Fi 断开时排队，失败重试，支持 HTTPS（内置证书包）
- BOOT 按键：长按进入 Wi-Fi 配网，三击清除 BLE 绑定

## 目录结构

```
ANCS-ESP32/
├── CMakeLists.txt
├── sdkconfig.defaults[.esp32|.esp32s3|.esp32c3|.esp32c6]
├── partitions/            # 4m / 8m / 16m 分区表（双 OTA 分区，预留后续 OTA）
└── main/
    ├── main.cc            # NVS 初始化，启动 Application
    ├── application.cc/h   # 主事件循环、Schedule()、模块组装
    ├── settings.cc/h      # NVS 封装
    ├── Kconfig.projbuild  # menuconfig 配置项
    ├── idf_component.yml  # 依赖：espressif/button
    ├── ble/
    │   ├── ble_manager.*  # NimBLE 初始化、广播、GAP 事件、配对绑定
    │   ├── ancs_client.*  # ANCS 协议实现
    │   └── ancs_defs.*    # ANCS 常量定义
    ├── network/
    │   └── wifi_service.* # Wi-Fi 连接 + 热点配网
    ├── notification/
    │   ├── notification.* # 通知数据结构、控制台打印、JSON 序列化
    │   └── webhook_client.*
    └── peripherals/
        └── button.*       # 按键封装
```

## 编译烧录

需要 ESP-IDF v5.4 及以上（已在 v5.5.2 验证 esp32s3 / esp32 编译通过）。

```bash
idf.py set-target esp32s3      # 也支持 esp32 / esp32c3 / esp32c6
idf.py menuconfig              # 可选：ANCS-ESP32 Configuration
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

> 修改 `sdkconfig.defaults` 后需要删除 `sdkconfig` 或重新 `set-target` 才会生效。

默认分区表为 4MB。16MB Flash 的板子可在 menuconfig 中把
`Partition Table > Custom partition CSV file` 改为 `partitions/16m.csv`，并设置 `Serial flasher config > Flash size` 为 16MB。

## 使用

1. **配网**：首次启动没有 Wi-Fi 配置，设备开启热点 `ANCS-XXXX`。手机连接后打开 `http://192.168.4.1` 选择 Wi-Fi 并输入密码。之后长按 BOOT 键可重新进入配网。
2. **连接 iPhone**：打开 iPhone「设置 > 蓝牙」，在「其他设备」中点击 `ANCS-ESP32` → 配对。
3. 首次连接 iOS 会询问是否允许该设备接收通知，选择允许；或在「设置 > 蓝牙 > ANCS-ESP32 (i)」中打开 **「共享系统通知」**。
4. 收到通知后串口输出示例：

```
==================== ANCS Notification ====================
Event    : added (uid=12)
Category : Social (count=1)
Flags    : PositiveAction|NegativeAction
App      : 微信 (com.tencent.xin)
Date     : 2026-11-11T09:30:00
Title    : 张三
Message  : 晚上一起吃饭吗？
Actions  : [] / [清除]
===========================================================
```

### 常见问题

- **日志提示 `ANCS service not found`**：iPhone 上没有开启「共享系统通知」，开启后会自动重试。
- **日志提示 `pairing keys do not match`**：ESP32 的绑定信息被清除（例如擦除了 Flash 或三击了 BOOT 键），但 iPhone 还保留着旧的配对。请在 iPhone 上「忽略此设备」后重新配对。
- 订阅成功后 iOS 会把通知中心里已有的通知全部重发一次（`PreExisting` 标记）。如果不需要，可在 menuconfig 中开启 `Ignore pre-existing notifications`。

## Webhook

URL 为空时 Webhook 关闭。配置来源（优先级从高到低）：

1. NVS：namespace `webhook`，key `url` / `auth`（代码中调用 `WebhookClient::GetInstance().SetUrl(...)` / `SetAuthToken(...)` 写入）
2. menuconfig：`ANCS-ESP32 Configuration > Webhook`

每条通知发送一次 `POST`，`Content-Type: application/json`，如配置了 token 则带 `Authorization` 头：

```json
{
  "device": "aa:bb:cc:dd:ee:ff",
  "event": "added",
  "uid": 12,
  "category": "Social",
  "category_id": 4,
  "category_count": 1,
  "flags": {
    "silent": false,
    "important": false,
    "pre_existing": false,
    "positive_action": true,
    "negative_action": true
  },
  "app_id": "com.tencent.xin",
  "app_name": "微信",
  "title": "张三",
  "subtitle": "",
  "message": "晚上一起吃饭吗？",
  "message_size": 24,
  "date": "2026-11-11T09:30:00",
  "positive_action": "",
  "negative_action": "清除"
}
```

`event` 取值为 `added` / `modified` / `removed`。`removed` 事件只包含 `uid`、`category` 与 `flags`，因为 iOS 不再提供已删除通知的内容。返回 2xx 视为成功；网络错误或 5xx 最多重试 3 次；4xx 不重试。

## 扩展

- 对通知执行操作（如拒接来电）：`BleManager::GetInstance().GetAncsClient().PerformAction(uid, kAncsActionNegative)`，可从任意任务调用。
- 新增通知去向（MQTT、屏幕显示等）：在 `Application::OnNotification()` 中添加。
