# ydPenetrationTest

面向“程序化外部系统功能测试 / 穿透式测试准备”的 YD C++ 自动化测试项目。
它不是策略项目，不包含 EMA。核心是：**测试步骤 -> YD API -> 等待回调 -> PASS/FAIL -> 证据日志**。

## 1. 项目结构

- `ydapi/`：YD 1.502 SDK 的头文件和 Win64/Linux64 动态库
- `include/core + src/core`：公共 YD 会话、日志、监控、指令检查
- `src/tests/TestCases.cpp`：附件3的 2.1~2.11 测试逻辑，以及补充的只读 1.2 行情/持仓快照
- `apps/test_XX_*.cpp`：每个文件只有 `main()`，编译成一个独立 exe
- `config/accounts.local.csv`：多账户批量运行（不要提交 Git）
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

## 4. 先跑安全测试

```bat
run_safe_tests.bat
```

或者单项：

```bat
build\bin\test_01_connect.exe --accounts config\accounts.local.csv
build\bin\test_12_market_position.exe --accounts config\accounts.local.csv --instrument au2612
build\bin\test_03_reconnect.exe --accounts config\accounts.local.csv
build\bin\test_07_instruction_check.exe --accounts config\accounts.local.csv
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

## 5. 会发真实测试报单的项目

只有显式 `--live` 才发送报撤单：

```bat
build\bin\test_02_basic_trade.exe --accounts config\accounts.local.csv --instrument au2612 --live
build\bin\test_04_order_cancel_count.exe --accounts config\accounts.local.csv --instrument au2612 --live
build\bin\test_10_batch_cancel.exe --accounts config\accounts.local.csv --instrument au2612 --live
```

`test_02_basic_trade` 对应 3.2.2 基础交易流程：按顺序逐笔完成 **2 笔买开成交、2 笔独立撤单、2 笔卖平成交**。每笔 `[ORDER_RESULT]` 日志都包含账号、结果时间、合约、方向、开平、柜台回报的委托价、成交价（撤单无成交价）、数量、OrderRef、系统编号、错误码和最终状态。为避免误平旧仓，程序只允许选择该账号当前“投机多仓为 0”的合约，并记录测试前后的仓位数量。任一步骤失败时，程序会先撤销本测试仍在工作的委托，再尝试平掉本测试实际新增的仓位；若无法确认订单终态、净新增仓位归零和仓位数量恢复，会输出 `MANUAL ACTION REQUIRED` 并停止后续账号。

`test_04_order_cancel_count` 对应 3.3.1.2 报撤单笔数监测。它使用同一真实登录账号逐笔发送 **2 张一手被动限价单**，每张收到柜台排队回报后分别撤单。计数发生在真实 `insertOrder` / `cancelOrder` 调用边界，并结合账号、合约、订单字段和系统单号绑定柜台回报；最终判定前会在连接、登录和历史追平均正常时等待本测试订单回报流进入静默窗口，重新核对委托累计成交量与成交回报量，并在关闭 API 前原子确认快照没有变化，关闭后再检查是否出现迟到回报。`[COUNT_RESULT]` 同时打印 API 请求数、成功提交数、柜台受理委托数、撤单请求数、确认撤单数、失败撤单回报、身份校验失败数、成交量一致性、回报流封口状态和清理状态。测试只允许选择该账号当前“投机多仓为 0”的合约，并复用 `test_02` 的失败清理：被动单意外成交时会判定测试失败并尝试恢复仓位，无法确认恢复时输出 `MANUAL ACTION REQUIRED`。

运行前必须把 `--instrument` 换成柜台测试环境当前可交易、流动性足够且该账号投机多仓为 0 的合约，并确认账户允许开仓。从程序启动到退出并打印最终 `[COUNT_RESULT]` 之前，该账号不得由其他客户端或程序并发报撤单，以免不同终端复用相同引用号。验收时应同时确认 `status=PASS`、`tradeVolumeConsistent=true`、`callbackStreamQuiet=true`、`streamStableThroughStop=true` 和 `cleanupRestored=true`；任一稳定性字段为 `false` 都按失败处理并人工核对。不要在生产账户上执行。

2.8 错误提示：

```bat
build\bin\test_08_error_message.exe --accounts config\accounts.local.csv --instrument au2612 --live --case no-position
build\bin\test_08_error_message.exe --accounts config\accounts.local.csv --instrument au2612 --live --case insufficient-funds
build\bin\test_08_error_message.exe --accounts config\accounts.local.csv --instrument au2612 --live --case market-state
```

这些必须在经纪商指定的测试环境、测试账号上运行。

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

日志统一标记：`TEST / SYSTEM / LOGIN / ACCOUNT / CONNECTION / MARKET / POSITION / POSITION_BASELINE / POSITION_VERIFY / API_EVENT / TRADE / ORDER / ORDER_RESULT / COUNT_RESULT / CLEANUP / MONITOR / ALERT / VALIDATION / ERROR / RESULT`，方便录屏和事后追溯。

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
- 2.7 是**程序自身在 insertOrder 前拒绝错误指令**，不是把明知错误的指令都扔给柜台。
- 2.8 的具体错误必须由账户/市场前置条件制造，不能靠代码保证每次产生同一个柜台错误。
- 2.10 `cancelMultiOrders` 一次最多 16 张，本项目示例使用 2 张。
- YD API 的回调线程要快速返回，本项目回调只做状态复制和日志，不在回调中做复杂测试流程。
