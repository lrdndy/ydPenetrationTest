# ydPenetrationTest

面向“程序化外部系统功能测试 / 穿透式测试准备”的 YD C++ 自动化测试项目。
它不是策略项目，不包含 EMA。核心是：**测试步骤 -> YD API -> 等待回调 -> PASS/FAIL -> 证据日志**。

## 1. 项目结构

- `ydapi/`：YD 1.502 SDK 的头文件和 Win64/Linux64 动态库
- `include/core + src/core`：公共 YD 会话、日志、监控、指令检查
- `src/tests/TestCases.cpp`：附件3的 2.1~2.11 测试逻辑，以及补充的只读 1.2 行情/持仓快照
- `apps/test_XX_*.cpp`：每个文件只有 `main()`，编译成一个独立 exe
- `config/accounts.local.csv`：多账户批量运行（不要提交 Git）
- `config/yd_config.local.txt`：本机 YD 柜台、行情及认证参数（不要提交 Git）
- `output/`：每次运行的日志和 `summary.csv`

## 2. Windows 从零编译

先安装 Visual Studio 2022 或 Build Tools，并勾选 **Desktop development with C++**。
然后打开：

`x64 Native Tools Command Prompt for VS 2022`

进入项目目录：

```bat
cd /d D:\projects\ydPenetrationTest
build_windows.bat
```

生成：

```text
build\bin\test_01_connect.exe
build\bin\test_12_market_position.exe
...
build\bin\test_11_logging.exe
build\bin\test_all.exe
```

CMake 会把 `yd.dll` 自动复制到 exe 目录。

## 3. 配置 10 个账户

编辑 `config/accounts.local.csv`：

```csv
username,password,label
80186617,******,acct01
80186618,******,acct02
...
```

默认是**顺序逐账户**运行，避免同时登录数授权限制和测试互相干扰。

### 本机 YD 接入参数

账号和密码写入本机账户 CSV；交易、行情地址以及分配给该程序的 AppID/AuthCode 写入本机 YD 配置。两类文件均已加入 `.gitignore`，不得提交真实密码或 AuthCode：

```ini
TradingServerIP=<交易地址>
TradingServerPort=<交易端口>
ConnectTCPMarketData=yes
TCPMarketDataServerIP=<行情地址>
TCPMarketDataServerPort=<行情端口>
AppID=<分配的 AppID>
AuthCode=<分配的 AuthCode>
```

当前 SDK 的登录接口接收 `username/password/AppID/AuthCode`，不接收独立的 Broker ID，因此不要自行添加无效的 `BrokerID=` 配置项。运行时用 `--yd-config` 显式选择本机配置：

```bat
build\bin\test_01_connect.exe --yd-config config\yd_config.local.txt --accounts config\accounts.yida.local.csv
```

公共会话会显式把配置中的 AppID/AuthCode 传给 YD 登录接口。`LOGIN_REQUESTED` 和 `login OK` 日志显示账号及 AppID，便于截图追溯；密码和 AuthCode 只用于认证，任何程序日志都不会输出它们。

## 4. 先跑安全测试

```bat
run_safe_tests.bat
```

或者单项：

```bat
build\bin\test_01_connect.exe --accounts config\accounts.local.csv
build\bin\test_12_market_position.exe --accounts config\accounts.local.csv --instrument au2612
build\bin\test_03_reconnect.exe --accounts config\accounts.local.csv
build\bin\test_07_1_invalid_instrument.exe --accounts config\accounts.local.csv --instrument au2617
build\bin\test_07_2_invalid_price.exe --accounts config\accounts.local.csv --instrument au2612
build\bin\test_07_3_invalid_volume.exe --accounts config\accounts.local.csv --instrument au2612
```

### 1.2 登录后查阅行情或持仓

`test_12_market_position` 是纯只读测试：登录并等待历史同步后，打印账号、登录/快照时间、指定合约行情，以及该账号最多 10 条非零持仓。它不会报单或撤单，不需要 `--live`。该测试默认等待历史同步 120 秒，可通过 `Snapshot.TimeoutSeconds` 调整。

必须选择柜台当前存在且有行情的活跃合约：

```bat
build\bin\test_12_market_position.exe --accounts config\accounts.local.csv --instrument au2612
```

如果账号没有持仓，只要取得指定合约行情，仍可作为“行情或持仓”的截图证据。完整结果写入本次运行目录中的 `test.log` 和 `summary.csv`。

### 3.3.1.1 连接状态异常监测

`test_03_reconnect` 登录成功后记录正常状态，主动断开当前 YD 交易连接，再等待本次新产生的断开、重连、重新登录和历史追平事件。它使用单调递增的事件序号核对先后关系，不会把之前发生过的断线状态误当成本次结果；日志中的 `[CONNECTION]` 行包含账号、状态、事件时间和事件序号。该测试不报单，不需要 `--live`：

```bat
build\bin\test_03_reconnect.exe --accounts config\accounts.local.csv
```

### 2.5 重复报单监测

`test_05_duplicate_order` 把同一账号、合约、方向、开平、价格和数量的开仓、平仓及撤单指令分别重复送入系统监测入口。监测器按指令特征累计相同指令的出现次数，并在终端自然输出 `INSTRUCTION_RECEIVED`、`DUPLICATE_INSTRUCTION` 和 `DUPLICATE_STATISTICS` 运行记录；正常监测不打印人为的 `PASS` 文案。该项目不调用 YD 报单或撤单 API，不需要 `--live`：

```bat
build\bin\test_05_duplicate_order.exe --accounts config\accounts.local.csv --instrument au2612
```

最终 `DUPLICATE_STATISTICS` 分别显示 `openDuplicateCount`、`closeDuplicateCount` 和 `cancelDuplicateCount`。样本的重复次数、价格和数量可通过 `Duplicate.RepeatCount`、`Duplicate.Price`、`Duplicate.Volume` 配置。CSV 汇总使用中性的 `OBSERVED` 状态保存监测快照。

同一监测组件已经接入 `YdSession` 的真实 `sendLimitOrder`、`cancelOrder` 和 `cancelMultiOrders` 调用边界。因此运行 `test_02`、`test_04`、`test_08` 或 `test_10` 时，如果实际送入 API 的账号、合约、方向、开平、价格和数量与本会话之前的指令相同，终端会直接输出 `DUPLICATE_INSTRUCTION`；会话结束时输出分项统计。接入监测不会绕过这些程序原有的 `--live` 安全门，也不会因为发现重复而自动阻止报单。

### 2.6 阈值设置及预警

每个 `YdSession` 都从 `config/test_config.ini` 加载报单、撤单和重复报单阈值。真实报撤单调用使相应统计值达到或超过阈值时，公共监测器立即在终端输出 `[ALERT]`。阈值设置为 `0` 表示禁用该类预警；录屏验收时建议使用 1~10 的正整数：

```ini
Threshold.OrderCount=3
Threshold.CancelCount=2
Threshold.DuplicateCount=2
Threshold.PopupEnabled=true
```

`Threshold.PopupEnabled=true` 时，每次首次达到某类阈值都会在保留终端 `[ALERT]` 的同时弹出一个置顶 Windows 警告框，显示账号、预警类型、当前值和阈值。弹窗使用独立线程，不会暂停真实报单调用；程序退出前会等待尚未关闭的风险窗口。设置为 `false` 可用于无人值守运行。非 Windows 平台仍保留终端告警，并记录弹窗不可用。

`test_06_threshold` 可在不连接柜台、不发送报单的情况下，以同一套监测组件确定性触发三类预警，终端输出 `THRESHOLD_CONFIGURATION`、三个阈值事件和 `RISK_STATISTICS`，正常运行不打印人为的 `PASS`：

```bat
build\bin\test_06_threshold.exe --accounts config\accounts.local.csv --instrument au2612
```

真实运行 `test_02`、`test_04`、`test_08`、`test_10` 时也使用相同阈值及弹窗配置，但仍必须遵守各程序的 `--live` 安全门。

### 2.7 交易指令检查

合约代码、最小变动价位和单笔最大委托数量检查已经嵌入 `YdSession` 的公共限价报单入口。任一检查失败时，系统在分配 API 请求计数和调用 `insertOrder` 之前输出 `INSTRUCTION_REJECTED`，其中包含账号、合约、方向、开平、价格、数量、拒绝原因和 `apiCalled=false`。此外，`YdSession` 自身默认关闭真实报撤单能力，只有现有 `--live` 流程会显式开启，形成第二层安全门。

三个测试点分别提供独立程序，便于逐项录屏、截图和留存结果。每个程序只持续构造自己对应的一类错误指令，每类默认重复 3 次，最终输出带有相应 `testPoint` 的 `INSTRUCTION_CHECK_STATISTICS`，并核对 API 请求数和成功提交数均为 0。正常运行不打印人为的 `PASS`，且不需要 `--live`：

```bat
rem 测试点 1：au2617 必须是柜台不存在的合约
build\bin\test_07_1_invalid_instrument.exe --accounts config\accounts.local.csv --instrument au2617

rem 测试点 2：au2612 必须是柜台存在的有效参考合约
build\bin\test_07_2_invalid_price.exe --accounts config\accounts.local.csv --instrument au2612

rem 测试点 3：au2612 必须是柜台存在的有效参考合约
build\bin\test_07_3_invalid_volume.exe --accounts config\accounts.local.csv --instrument au2612
```

`test_07_1` 的 `--instrument` 表示要拦截的“不存在合约”，它覆盖 `Validation.InvalidInstrument`；若该代码实际上存在，程序会要求更换，不能伪造合约错误。`test_07_2` 和 `test_07_3` 的 `--instrument` 表示真实参考合约，分别用于取得 `Tick` 和最大委托手数；未传时使用 `Validation.ReferenceInstrument`。不存在合约与有效参考合约彼此独立，避免把正确合约做成合约代码错误。配置示例：

```ini
Validation.InvalidInstrument=au2617
Validation.ReferenceInstrument=au2612
Validation.RepeatCount=3
```

原来的 `test_07_instruction_check.exe` 仍保留为一次执行三个测试点的汇总入口；其中 `--instrument` 沿用“不存在合约”的含义，有效参考合约仍从 `Validation.ReferenceInstrument` 读取。

错误指令永远不会发送到柜台。

## 5. 会发真实测试报单的项目

只有显式 `--live` 才发送报撤单：

```bat
build\bin\test_02_basic_trade.exe --accounts config\accounts.local.csv --instrument au2612 --live
build\bin\test_04_order_cancel_count.exe --accounts config\accounts.local.csv --instrument au2612 --live
build\bin\test_10_batch_cancel.exe --accounts config\accounts.local.csv --instrument au2612 --live
```

`test_02_basic_trade` 对应 3.2.2 基础交易流程：按顺序逐笔完成 **2 笔买开成交、2 笔独立撤单、2 笔卖平成交**。每笔 `[ORDER_RESULT]` 日志都包含账号、结果时间、合约、方向、开平、柜台回报的委托价、成交价（撤单无成交价）、数量、OrderRef、系统编号、错误码和最终状态。为避免误平旧仓，程序只允许选择该账号当前“投机多仓为 0”的合约，并记录测试前后的仓位数量。任一步骤失败时，程序会先撤销本测试仍在工作的委托，再尝试平掉本测试实际新增的仓位；若无法确认订单终态、净新增仓位归零和仓位数量恢复，会输出 `MANUAL ACTION REQUIRED` 并停止后续账号。

`test_04_order_cancel_count` 对应 3.3.1.2 报撤单笔数监测。它使用同一真实登录账号逐笔发送 **2 张一手被动限价单**，每张收到柜台排队回报后分别撤单。计数发生在真实 `insertOrder` / `cancelOrder` 调用边界，并结合账号、合约、订单字段和系统单号绑定柜台回报；最终判定前会在连接、登录和历史追平均正常时等待本测试订单回报流进入静默窗口，重新核对委托累计成交量与成交回报量，并在关闭 API 前原子确认快照没有变化，关闭后再检查是否出现迟到回报。`[COUNT_RESULT]` 同时打印 API 请求数、成功提交数、柜台受理委托数、撤单请求数、确认撤单数、失败撤单回报、身份校验失败数、成交量一致性、回报流封口状态和清理状态。测试只允许选择该账号当前“投机多仓为 0”的合约，并复用 `test_02` 的失败清理：被动单意外成交时会判定测试失败并尝试恢复仓位，无法确认恢复时输出 `MANUAL ACTION REQUIRED`。

`test_10_batch_cancel` 采用选测测试点2“多笔已报单批量撤单”。它提交 **2 张一手被动买开单**，逐张确认属于当前会话、柜台状态为 `QUEUING`、系统单号已绑定且成交量为 0；随后只调用一次 YD `cancelMultiOrders`，再根据两张订单各自的柜台回报确认均为 `CANCELED`。公共批撤入口会在整批调用前再次核验订单归属、最新状态、合约和系统单号，任一目标不一致则整批不调用 API。成功路径不打印人为 `PASS`，最终输出 `BATCH_CANCEL_STATISTICS`，其中 `batchApiCalls=1` 证明使用的是一次批量调用，`batchTargetOrders=2`、`confirmedCancellations=2` 和 `canceledOrders=2` 证明两张委托均被撤销。

该流程复用基础交易测试的仓位安全边界：测试前要求所选合约投机多仓为 0，价格由新鲜盘口按 `BatchCancel.WorkingOrderOffsetTicks` 构造；如果被动单意外部分或全部成交，测试不会把它冒充测试点1，而是判定异常、撤销剩余数量，并只按本程序验证过的订单和成交回报恢复净新增多仓。最终必须同时满足 `unexpectedTradeVolume=0`、`noWorkingOrders=true`、`ownedNetLong=0`、`positionRestored=true`、`callbackStreamQuiet=true`、`streamStableThroughStop=true` 和 `cleanupRestored=true`。出现 `MANUAL ACTION REQUIRED` 时立即到柜台客户端核对。

运行前必须把 `--instrument` 换成柜台测试环境当前可交易、流动性足够且该账号投机多仓为 0 的合约，并确认账户允许开仓。从程序启动到退出并打印最终统计之前，该账号不得由其他客户端或程序并发报撤单，以免不同终端复用相同引用号。`test_04` 验收时检查 `[COUNT_RESULT] status=PASS`；`test_10` 检查 `[BATCH_CANCEL_STATISTICS]` 中的批次、逐单确认和安全字段。两者都要求 `tradeVolumeConsistent=true`、`callbackStreamQuiet=true`、`streamStableThroughStop=true` 和 `cleanupRestored=true`；任一稳定性字段为 `false` 都按失败处理并人工核对。不要在生产账户上执行。

### 2.8 错误提示

三个必测点分别使用独立程序。它们通过真实 `YdSession` 报单入口向柜台发送指令，并从本程序拥有订单的 `notifyOrder` 回调接收 `ErrorNo`。终端会自然显示 `[ERROR] event=ORDER_REJECTED source=notifyOrder`，包含账号、合约、错误码和可读错误名称；成功路径不打印人为的 `PASS`，最终以生产式 `ORDER_ERROR_STATISTICS` 留存错误接收及账户状态证据。程序内部日志不显示预期错误码、测试场景或匹配结论。

```bat
rem 测试点1：账号资金必须不足以承担配置数量的开仓保证金
build\bin\test_08_1_insufficient_funds.exe --accounts config\accounts.local.csv --instrument au2612 --live

rem 测试点2：程序会先确认该账号在该合约上的投机多仓为0
build\bin\test_08_2_no_position.exe --accounts config\accounts.local.csv --instrument au2612 --live

rem 测试点3：建议在收盘后或合约暂停交易时执行
build\bin\test_08_3_market_state.exe --accounts config\accounts.local.csv --instrument au2612 --live
```

预期错误码分别为：资金不足 `2`，无仓可平 `1`，市场状态不允许 `37/66/93/133/138`。柜台返回其他错误时仍会原样显示，但不会把它归入目标错误类别。

三个程序都必须显式指定 `--live`，且只能在经纪商批准的测试环境和测试账号上运行。为了降低柜台未拒绝时的风险，程序要求测试前投机多仓为 0，并使用距离盘口 `ErrorTest.PassiveOffsetTicks` 个 Tick 的被动价格；若指令被受理则立即撤单，若发生意外开仓，只根据本程序拥有订单的回报尝试恢复净新增仓位。清理状态不完整时会显示 `MANUAL ACTION REQUIRED`，此时必须立即到柜台客户端核对。

资金不足不能由程序凭空保证，需根据测试账号资金和合约保证金调整 `ErrorTest.InsufficientFundsVolume`，且数量必须处于该合约单笔委托范围内。收盘后若订阅不到有效盘口，可将 `ErrorTest.OrderPrice` 从 `0` 改为该合约当日合法价位。原来的 `test_08_error_message.exe --case no-position|insufficient-funds|market-state` 继续保留为兼容入口。

### 2.9 暂停交易

项目选择测试点1“限制账号交易权限”。账户会话内的线程安全 `TradingGate` 已嵌入 `YdSession::sendLimitOrder` 公共报单入口；操作员执行暂停后，该账号所有新的报单指令都会在订单引用号、API计数和 `insertOrder` 之前被拒绝，已有订单的撤单仍保持可用。

人工录屏模式不需要也不接受 `--live`：

```bat
build\bin\test_09_pause_trade.exe --accounts config\accounts.local.csv --instrument au2612 --interactive
```

程序完成登录和静态数据同步后会订阅指定合约并持续运行。行情快照和运行心跳会继续输出，终端输入由后台线程监听，不会出现 `command>`，也不会因为等待键盘命令而停住主循环。Windows 下采用不回显的原始按键读取，输入内容不会与行情日志串行。程序不会在正常运行日志中显示快捷键说明或终端监听参数：

```text
[MARKET] account=001 event=SUBSCRIPTION_ACTIVE instrument=au2612
[MARKET] account=001 event=MARKET_SNAPSHOT instrument=au2612 ...
[SYSTEM] account=001 component=TRADING_RUNTIME state=ACTIVE tradingState=RUNNING ...
```

系统正常运行期间可随时直接输入 `pause`、`resume` 或 `status`。录屏时只需在英文或中文输入法下键入以下三个字符，不需要按回车：

```text
:wq
```

ASCII `:wq`、中文全角冒号 `：wq` 以及不带冒号的 `wq` 都会被识别。它们执行安全停止：先关闭该账号的新报单权限，验证已进入公共入口的策略报单意图会被门禁拦截，然后请求程序退出。退出命令也兼容 `quit`、`q`、`:q` 和 `exit`。整个过程不会向柜台发送报单。

输入 `pause` 时系统保持在线并继续接收行情，只暂停新的交易指令；输入 `resume` 后恢复。输入 `pause` 或直接输入 `:wq` 时，终端应自然显示：

```text
[RISK] account=001 event=TRADING_PAUSED source=MANUAL_TERMINAL newOrdersAllowed=false
[TRADE] account=001 event=ORDER_INTENT source=STRATEGY instrument=au2612 ...
[RISK] account=001 event=ORDER_BLOCKED source=STRATEGY instrument=au2612 reason=TRADING_PAUSED pauseSource=MANUAL_TERMINAL apiCalled=false
[SYSTEM] account=001 event=SHUTDOWN_REQUESTED source=MANUAL_TERMINAL input=:wq newOrdersAllowed=false
[RISK] account=001 event=TRADING_CONTROL_STATISTICS blockedOrderInstructions=1 orderApiRequests=0 orderRequestsSubmitted=0 apiCalled=false
```

正常路径不打印人为 `PASS`。不加 `--interactive` 时程序执行同一公共门控的自动安全检查，供 `run_safe_tests.bat` 和 `test_all` 使用；无论哪种模式，`test_09` 都拒绝 `--live`。

### 2.11 日志记录

`test_11_logging` 是只读日志归档审计程序，不连接 YD，也不报单或撤单，并明确拒绝 `--live`。运行前需要先保留同一账号的真实业务日志：`test_04` 或 `test_10` 提供真实报单、撤单监测及安全封口记录，任一成功收到柜台错误的 `test_08_1/2/3` 提供错误提示记录。完整 YD 启动、连接、登录、初始化、历史追平和退出记录可来自上述任一完整会话。

默认审计该账号最近完整会话的启动日期，也可以固定归档日期。跨午夜的夜盘会话仍作为一个完整会话读取，不会丢弃次日的退出和最终统计：

```bat
build\bin\test_11_logging.exe --accounts config\accounts.local.csv
build\bin\test_11_logging.exe --accounts config\accounts.local.csv --log-date 20260819
```

程序不会把单独的 `[TRADE]`、`insertOrder`、策略意图或监测样本当成真实交易。真实报单必须以同一 `OrderRef` 关联 `ORDER_API_REQUEST apiReturned=true` 与柜台 `notifyOrder`；报撤单监测还必须有关联撤单确认以及 `cleanupRestored=true`、`streamStableThroughStop=true` 等安全封口；柜台错误必须来自 `ORDER_REJECTED source=notifyOrder` 且错误码大于 0。

审计结果以自然的 `LOG_RECORD_INDEXED`、`LOG_RETENTION_LOCATION` 和 `LOG_ARCHIVE_STATISTICS` 记录写入本次 `test.log`。每条索引都包含原始日志绝对路径和原始记录；最终应显示 `tradeLog=true`、`systemRuntimeLog=true`、`monitoringLog=true`、`cabinetErrorLog=true`、`retentionPath=true`、`traceable=true`。成功路径没有人为 `PASS` 或 `example`。缺少证据时会列出 `missingRecordTypes` 并返回失败，不能用本地构造内容补齐。由于它依赖先前的真实业务日志，`run_safe_tests.bat` 和 `test_all` 不自动运行该项。

## 6. 指定合约

```bat
build\bin\test_02_basic_trade.exe --accounts config\accounts.local.csv --instrument au2612 --live
```

默认合约在 `config/test_config.ini` 的 `Test.Instrument`。

## 7. 输出证据

每次运行建立时间目录：

```text
output/20260810_120000/
  summary.csv
  2.1_connect/acct01/test.log
  2.1_connect/acct02/test.log
  ...
```

日志统一标记：`TEST / SYSTEM / LOGIN / ACCOUNT / CONNECTION / MARKET / POSITION / POSITION_BASELINE / POSITION_VERIFY / API_EVENT / TRADE / ORDER / ORDER_RESULT / COUNT_RESULT / BATCH_CANCEL / CLEANUP / MONITOR / ALERT / VALIDATION / ERROR / LOG_ARCHIVE / RESULT`，方便录屏和事后追溯。

## 8. 为什么测试 cpp 很短

例如 `apps/test_01_connect.cpp` 只负责选择测试：

```cpp
int main(int argc, char** argv) {
    auto o = ydtest::parseArgs(argc, argv);
    return ydtest::runTest01Connect(o);
}
```

YD 登录、回调、等待、报撤单全部封装在公共 `YdSession` 中。以后修改 API 适配只改一处。

## 9. 重要边界

- `test_all` 明确拒绝 `--live`；真实报单测试必须逐个运行，确认当前账号的清理结果后才能继续下一项。
- 2.7 通过 `YdSession` 公共报单入口在 API 请求计数和 `insertOrder` 前强制拒绝错误指令，并以 `apiCalled=false` 和请求增量为 0 作为证据。
- 2.8 的具体错误必须由账户/市场前置条件制造，不能靠代码保证每次产生同一个柜台错误。
- 2.10 `cancelMultiOrders` 一次最多 16 张；本项目选择测试点2并使用 2 张完全未成交的已报委托，测试点1“部分成交报单批撤”不自动制造。
- YD API 的回调线程要快速返回，本项目回调只做状态复制和日志，不在回调中做复杂测试流程。
