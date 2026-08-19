# 从零理解 ydPenetrationTest（给 Python 开发者）

## 1. C++ 不是“直接运行 cpp”

Python：

```text
python test_login.py
```

C++：

```text
test_01_connect.cpp
        ↓ 编译
build/bin/test_01_connect.exe
        ↓ 运行
测试结果
```

这个项目里 `apps/*.cpp` 各自有一个 `main()`，所以每个文件会编译成一个独立 exe。
公共代码不复制，而是放在 `yd_test_core` 库里。

## 2. 编译关系

```text
                         CMakeLists.txt
                              │
                ┌─────────────┴─────────────┐
                │                           │
         yd_test_core                  apps/*.cpp
                │                           │
       ┌────────┼────────┐                  │
       │        │        │                  │
  YdSession  Monitor  Validator             │
       │                                     │
       └──────────────────┬──────────────────┘
                          │ link
                          ▼
                 test_01_connect.exe
                 test_02_basic_trade.exe
                 ...
```

## 3. 最重要的公共类：YdSession

`YdSession` 继承官方 `YDListener`：

```text
YD API TCP回报线程
   │
   ├─ notifyEvent
   ├─ notifyLogin
   ├─ notifyFinishInit
   ├─ notifyCaughtUp
   ├─ notifyOrder
   └─ notifyTrade
          │
          ▼
       YdSession
          │
          ├─ 保存最新状态
          ├─ condition_variable 唤醒测试主线程
          └─ 写证据日志
```

测试主线程不是用 `sleep(5)` 猜结果，而是等待明确条件：

```cpp
session.waitLogin(12);
session.waitInit(12);
session.waitCaughtUp(12);
```

下单后也是：

```cpp
int ref = session.sendLimitOrder(...);
session.waitOrder(ref, 12, ...);
session.waitTrade(ref, 12, trade);
```

因此测试由 YD 的真实回调驱动。

## 4. 为什么多个账户默认串行

```text
account 1
  ↓ create YDApi
  ↓ login
  ↓ test
  ↓ startDestroy
account 2
  ↓ create YDApi
  ...
```

这样更容易：

- 保证录屏和日志清楚
- 避免账户之间的订单混在一起
- 避免柜台同时登录账户数限制
- 一个账户失败时更容易定位

等第一版稳定后，再考虑并发账户。

## 5. 三类测试

### A. 纯本地功能测试

不需要发单：

- 2.5 重复报单
- 2.6 阈值预警
- 2.9 暂停交易

### B. 需要登录 YD，但不发交易指令

- 2.1 连通性
- 2.3 断开/重连
- 2.7 指令检查（从真实 YDInstrument 读取 Tick、最大手数，但错误订单在本地被拦截）
- 2.11 日志

### C. 真的会向测试柜台发报撤单

必须加 `--live`：

- 2.2 开仓、平仓、撤单
- 2.4 同一真实账号报单、撤单及笔数监测
- 2.8 柜台错误提示
- 2.10 批量撤单

## 6. 为什么有 `--live`

错误操作：

```text
开发配置突然指到生产柜台
↓
运行 test_all
↓
程序自动发单
```

因此项目默认禁止真实交易测试。

例如：

```bat
test_02_basic_trade.exe --accounts config\accounts.local.csv --instrument au2612 --live
test_04_order_cancel_count.exe --accounts config\accounts.local.csv --instrument au2612 --live
```

才允许执行会报撤单的测试逻辑。

## 7. 如何增加第 13 项测试

假设以后新增“密码修改测试”：

1. 在 `include/tests/TestCases.h` 增加：

```cpp
int runTest13Password(const RunOptions&);
```

2. 在 `src/tests/TestCases.cpp` 写测试逻辑。

3. 新建：

```text
apps/test_13_password.cpp
```

只写一个很短的 main：

```cpp
#include "tests/TestCases.h"
int main(int argc, char** argv) {
    auto o = ydtest::parseArgs(argc, argv);
    return ydtest::runTest13Password(o);
}
```

4. `CMakeLists.txt` 增加：

```cmake
add_yd_test(test_13_password)
```

重新编译后就得到：

```text
build/bin/test_13_password.exe
```
