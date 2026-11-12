# ANCS-ESP32

通过 BLE 连接 iPhone，使用 Apple **ANCS**（Apple Notification Center Service）获取手机通知，打印到串口控制台，并可通过 **Webhook** 将通知 POST 到其他服务器。

## 功能

- BLE 外设广播 ANCS Service Solicitation，iPhone「设置 > 蓝牙」中可直接看到并连接
- Just Works 绑定配对（含 IRK，兼容 iPhone 随机地址），重启后自动重连
- 默认只连接 1 台设备（iPhone / iPad），可在管理后台改为最多 4 台同时连接，每条通知都带有来源设备的名称和地址
- 完整 ANCS 客户端：服务发现 → 订阅 → 获取通知属性（App、标题、副标题、正文、时间、操作按钮）→ 查询 App 显示名称（带缓存）
- Data Source 分包重组，Control Point 请求串行队列 + 超时
- Wi-Fi：保存多个 SSID，无配置或连接超时自动开启配网热点；超时开启的热点在扫描到已保存的 Wi-Fi 且无人连接热点时自动关闭并重连
- Webhook：独立任务 + 队列，Wi-Fi 断开时排队，失败重试，支持 HTTPS（内置证书包）
- 管理后台：连上 Wi-Fi 后在 `http://<设备IP>/` 提供网页，可查看通知记录（默认保存最近 30 条）、配置 Webhook、上传固件升级（OTA）
- BOOT 按键：长按进入 Wi-Fi 配网，三击清除 BLE 绑定

## 目录结构

```
ANCS-ESP32/
├── CMakeLists.txt
├── sdkconfig.defaults[.esp32|.esp32s3|.esp32c3|.esp32c6]
├── partitions/            # 4m / 8m / 16m 分区表（双 OTA 分区，预留后续 OTA），默认 16m
└── main/
    ├── main.cc            # NVS 初始化，启动 Application
    ├── application.cc/h   # 主事件循环、Schedule()、模块组装
    ├── settings.cc/h      # NVS 封装
    ├── Kconfig.projbuild  # menuconfig 配置项
    ├── idf_component.yml  # 依赖：espressif/button、espressif/cjson
    ├── ble/
    │   ├── ble_manager.*  # NimBLE 初始化、广播、GAP 事件、配对绑定
    │   ├── ancs_client.*  # ANCS 协议实现
    │   └── ancs_defs.*    # ANCS 常量定义
    ├── network/
    │   ├── wifi_service.* # Wi-Fi 连接（扫描 → 连接最强的已保存 Wi-Fi → 重试）+ 配网模式切换
    │   ├── wifi_store.*   # 已保存的 Wi-Fi（NVS namespace `wifi`）
    │   ├── config_portal.* # 配网热点的网页（config_portal.html）+ 强制门户跳转
    │   └── dns_server.*   # 配网热点的 DNS，所有域名都解析到 192.168.4.1
    ├── notification/
    │   ├── notification.* # 通知数据结构、控制台打印、JSON 序列化
    │   ├── notification_history.* # 最近通知记录及上传状态（管理后台显示）
    │   ├── notification_store.* # 通知记录持久化（`storage` 分区上的环形日志）
    │   └── webhook_client.*
    ├── web/
    │   ├── web_server.*   # 管理后台 HTTP 服务 + OTA
    │   ├── http_utils.*   # JSON 请求 / 响应工具（管理后台与配网页面共用）
    │   └── admin.html     # 管理后台页面（编译时嵌入固件）
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

默认按 16MB Flash 配置（`partitions/16m.csv`：两个 4MB OTA 分区 + 8MB `storage`，其中前 2MB 保存通知记录，其余预留）。
Flash 较小的板子可在 menuconfig 中把 `Partition Table > Custom partition CSV file` 改为
`partitions/4m.csv` 或 `partitions/8m.csv`，并相应修改 `Serial flasher config > Flash size`。
这两个分区表没有 `storage` 分区，通知记录只保存在内存中，重启后清空。

> OTA 升级不会更新分区表，所以 `storage` 分区固定为 8MB，由代码按偏移划分用途：以后新增要保存的内容时只需 OTA，不必用 USB 重新烧录。之前烧录的设备（子类型为 `spiffs`）无需重新烧录，固件按名称查找分区。

### 通知记录持久化

有 `storage` 分区时，每条通知及其 Webhook 上传状态都会写入 Flash（`NotificationStore`），重启后恢复：

- 存储格式是环形日志：按 4KB 扇区顺序追加，写满 2MB 后擦除最旧的扇区循环使用，每个扇区的擦写次数相同。每条记录带 CRC，写入中途掉电只会丢失这一条
- 上传状态（等待上传 / 已上传 / 上传失败 / 未配置）只占记录头里的一个字节，状态变化时只把 bit 由 1 改为 0，原地写入，不擦除扇区
- 每条通知约 0.25 次扇区擦除（正文 512 字节时一条最大约 1KB）。按 NOR Flash 每扇区 10 万次擦写估算：每天 1000 条可用数百年；即使每秒 1 条、24 小时不停，也能用约 6 年
- 管理后台显示最近的若干条（默认 30，最多 100）；更早的记录仍留在 Flash 中，直到被循环覆盖。「清空」只是让已有记录不再显示（NVS namespace `history`，key `cleared`），不会擦除 Flash
- 重启前还没上传完成的通知，开机后会重新发送（最多 `CONFIG_WEBHOOK_QUEUE_SIZE / 2` 条最新的，更早的记为上传失败）。如果断电时请求已经到达服务器、但还没收到响应，这条通知会被重复发送
- 设备没有校准时钟，所以重启前收到的记录在页面上显示为「重启前」，通知本身的时间以 iOS 提供的 `date` 为准

## 使用

1. **配网**：首次启动没有 Wi-Fi 配置，设备开启热点 `ANCS-XXXXXX`（XXXXXX 为 MAC 地址后 3 字节）。手机连接后一般会自动弹出配网页面（没有弹出就手动打开 `http://192.168.4.1`），从附近的 Wi-Fi 中选择（或手动输入隐藏网络的名称）并输入密码。设备会先实际连接一次验证密码，成功后保存、显示设备在该 Wi-Fi 下的 IP 地址，几秒后关闭热点并连接。页面上也可以删除已保存的 Wi-Fi，或直接退出配网去连接已保存的 Wi-Fi。之后长按 BOOT 键可重新进入配网。

   开机 60 秒内连不上已保存的 Wi-Fi（例如停电后路由器比设备启动得慢）也会开启热点。这种热点每 10 秒扫描一次，发现已保存的 Wi-Fi、且没有手机连着热点时，会自动关闭热点去连接。如果还是连不上（比如路由器改了密码），下次热点至少保持 1 分钟才自动关闭，之后依次为 2、4……最长 10 分钟，方便手机连上热点重新配网。长按 BOOT 键或在管理后台进入的配网模式不会自动关闭。
2. **连接 iPhone**：打开 iPhone「设置 > 蓝牙」，在「其他设备」中点击 `ANCS-XXXXXX`（与热点名称相同）→ 配对。
3. 首次连接 iOS 会询问是否允许该设备接收通知，选择允许；或在「设置 > 蓝牙 > ANCS-XXXXXX (i)」中打开 **「共享系统通知」**。
4. 收到通知后串口输出示例：

```
==================== ANCS Notification ====================
Device   : 张三的iPhone (5c:52:84:aa:bb:cc)
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

### 管理后台

设备连上 Wi-Fi 后，串口会打印 `Admin page: http://192.168.x.x/`，用同一局域网内的电脑或手机浏览器打开即可：

- **概览**：蓝牙名称（`ANCS-XXXXXX`，页面标题也会显示，方便区分多块板子）、固件版本、IP、Wi-Fi 信号、蓝牙连接数、内存
- **通知记录**：列表中正文最多显示 3 行，点击任意一条弹出完整内容和详情（App ID、来源设备、操作按钮等）。最近收到的通知（默认 30 条，可在「设置」中改为 1–100 条），每 5 秒自动刷新。同一条通知的 `modified` 事件会覆盖之前的记录。等待上传和上传失败的通知会有标记。有 `storage` 分区时记录保存在 Flash 中（见[通知记录持久化](#通知记录持久化)），否则重启后清空
- **设置**：Webhook URL / Authorization（保存后立即生效，可发送测试通知）、蓝牙同时连接数量、通知保存条数、管理密码
- **Wi-Fi**：查看、添加、编辑（改名称 / 密码）、删除已保存的 Wi-Fi（最多 10 个，存于 NVS namespace `wifi`，与配网热点页面共用）。密码不会回显。设备总是连接信号最强的已保存 Wi-Fi，所以没有优先级设置；修改不影响当前连接，在下次重新扫描连接或重启后生效
- **固件升级**：选择 `build/ancs-esp32.bin` 上传。设备重启后页面会检查是否运行在新分区上，新固件启动失败被回滚时会提示。设备会检查芯片型号和项目名，写入另一个 OTA 分区后自动重启；新固件连上 Wi-Fi、管理后台启动后才会被确认，如果新固件在此之前崩溃重启，bootloader 会自动回滚到旧版本
- **系统**：
  - 诊断：上次重启原因，蓝牙收到 / 已处理 / 跳过的通知数，主任务是否卡住，Flash 持久化状态和读写错误。发现异常时标题旁会标红
  - 运行日志：串口输出（`ESP_LOGx` 和 `printf`）的最近 32KB，保存在内存中，按级别着色，打开时每 3 秒只拉取新增部分，可下载。`app_main` 之前的启动信息只在串口上
  - 重启、清除蓝牙配对、重新配网

默认没有密码。设置管理密码后浏览器会弹出登录框，用户名 `admin`。已保存的 Webhook Authorization 不会在页面上回显。

用脚本调用接口时，JSON 请求须带 `Content-Type: application/json`，OTA 须带 `Content-Type: application/octet-stream`。也可在 menuconfig `ANCS-ESP32 Configuration > Admin web page` 中设置默认密码和默认保存条数（页面上修改的值保存在 NVS 中，优先级更高）。

> 进入配网模式（长按 BOOT 键或页面上的「重新配网」）时管理后台会关闭，因为配网热点的网页使用同一个 80 端口。

### 多设备

默认只连接 1 台 iPhone / iPad，可在管理后台「设置 > 蓝牙」中改为 1–4 台，立即生效（保存在 NVS，默认值见 menuconfig `Bluetooth (ANCS) > Default number of devices connected at the same time`）。
上限 4 由编译时的 `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` 决定，最多保存 4 台设备的配对（`CONFIG_BT_NIMBLE_MAX_BONDS`）。
允许多台时，已有设备连接后 ESP32 会继续广播，下一台设备按同样步骤配对即可；连满后停止广播，任意一台断开后恢复。调小数量时已连接的设备不会被断开，只是不再接受新设备。

### 常见问题

- **日志提示 `ANCS service not found`**：iPhone 上没有开启「共享系统通知」，开启后会自动重试。
- **日志提示 `pairing keys do not match`**：ESP32 的绑定信息被清除（例如擦除了 Flash 或三击了 BOOT 键），但 iPhone 还保留着旧的配对。请在 iPhone 上「忽略此设备」后重新配对。
- 订阅成功后 iOS 会把通知中心里已有的通知全部重发一次（`PreExisting` 标记）。iPhone 断开重连时，已经上报过的通知会被识别并跳过（记住最近 256 条；有 `storage` 分区时开机会从 Flash 中的记录恢复，所以 ESP32 重启后也不会重复上报，否则重启后清空），不会重复写入通知记录或发送 Webhook；断线期间新收到的通知仍会上报。如果完全不需要已有通知，可在 menuconfig 中开启 `Ignore pre-existing notifications`。

## Webhook

URL 为空时 Webhook 关闭。配置来源（优先级从高到低）：

1. NVS：namespace `webhook`，key `url` / `auth`（在管理后台「设置」页面修改，或代码中调用 `WebhookClient::GetInstance().SetUrl(...)` / `SetAuthToken(...)` 写入）
2. menuconfig：`ANCS-ESP32 Configuration > Webhook`

每条通知发送一次 `POST`，`Content-Type: application/json`，如配置了 token 则带 `Authorization` 头：

```json
{
  "device": "aa:bb:cc:dd:ee:ff",
  "peer_name": "张三的iPhone",
  "peer_address": "5c:52:84:aa:bb:cc",
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
  "message_truncated": false,
  "date": "2026-11-11T09:30:00",
  "positive_action": "",
  "negative_action": "清除"
}
```

`device` 是 ESP32 的 MAC；`peer_name` / `peer_address` 标识通知来自哪台 iPhone / iPad（地址为绑定后的身份地址，重连不变）。
`event` 取值为 `added` / `modified`；管理后台发送的测试通知为 `test`。
`message` 最多 `CONFIG_ANCS_MAX_MESSAGE_LEN`（默认 512）字节，更长的内容 iOS 不会发送，此时 `message_truncated` 为 `true`、`message_size` 为原始长度。返回 2xx 视为成功；网络错误或 5xx 最多重试 3 次；4xx 不重试。重试期间 Wi-Fi 断开时会等待重新连上再发送，这种失败不计入重试次数。

连接会复用（HTTP keep-alive）：连续多条通知只需建立一次连接 / TLS 握手；空闲 30 秒、修改 URL 或 Wi-Fi 断开后关闭。复用的连接已被服务器关闭时会立即重连重发，不计入重试次数。

## 扩展

- 对通知执行操作（如拒接来电）：`BleManager::GetInstance().PerformAction(notification.conn_handle, notification.uid, kAncsActionNegative)`，可从任意任务调用。
- 新增通知去向（MQTT、屏幕显示等）：在 `Application::OnNotification()` 中添加。
