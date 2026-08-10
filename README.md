# ydPenetrationTest

面向“程序化外部系统功能测试 / 穿透式测试准备”的 YD C++ 自动化测试项目。
它不是策略项目，不包含 EMA。核心是：**测试步骤 -> YD API -> 等待回调 -> PASS/FAIL -> 证据日志**。

## 1. 项目结构

- `ydapi/`：YD 1.502 SDK 的头文件和 Win64/Linux64 动态库
- `include/core + src/core`：公共 YD 会话、日志、监控、指令检查
- `src/tests/TestCases.cpp`：附件3的 2.1~2.11 测试逻辑
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
build\bin\test_07_instruction_check.exe --accounts config\accounts.local.csv
```

## 5. 会发真实测试报单的项目

只有显式 `--live` 才发送报撤单：

```bat
build\bin\test_02_basic_trade.exe --accounts config\accounts.local.csv --live
build\bin\test_10_batch_cancel.exe --accounts config\accounts.local.csv --live
```

2.8 错误提示：

```bat
build\bin\test_08_error_message.exe --accounts config\accounts.local.csv --live --case no-position
build\bin\test_08_error_message.exe --accounts config\accounts.local.csv --live --case insufficient-funds
build\bin\test_08_error_message.exe --accounts config\accounts.local.csv --live --case market-state
```

这些必须在经纪商指定的测试环境、测试账号上运行。

## 6. 指定合约

```bat
build\bin\test_02_basic_trade.exe --instrument cu2408 --live
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

日志统一标记：`SYSTEM / LOGIN / API_EVENT / TRADE / ORDER / MONITOR / ALERT / VALIDATION / ERROR / RESULT`，方便录屏和事后追溯。

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

- 2.7 是**程序自身在 insertOrder 前拒绝错误指令**，不是把明知错误的指令都扔给柜台。
- 2.8 的具体错误必须由账户/市场前置条件制造，不能靠代码保证每次产生同一个柜台错误。
- 2.10 `cancelMultiOrders` 一次最多 16 张，本项目示例使用 2 张。
- YD API 的回调线程要快速返回，本项目回调只做状态复制和日志，不在回调中做复杂测试流程。
