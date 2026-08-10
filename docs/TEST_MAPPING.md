# 附件3测试项与程序对应关系

| 附件3 | 可执行程序 | 自动化方式 | 是否发真实报单 |
|---|---|---|---|
| 2.1 连通性 | test_01_connect | 连接、认证登录、静态数据、caught up | 否 |
| 2.2 基础交易功能 | test_02_basic_trade | 买开、卖平、挂单后撤单 | 是，需 --live |
| 2.3 连接异常监测 | test_03_reconnect | 调用 disconnect，等待断开与自动重连 | 否 |
| 2.4 报撤单笔数监测 | test_04_order_cancel_count | Monitor 统计报单/撤单意图 | 否 |
| 2.5 重复报单监测 | test_05_duplicate_order | 相同合约/方向/开平/价/量重复检测 | 否 |
| 2.6 阈值设置预警 | test_06_threshold | 报单、撤单、重复报单阈值告警 | 否 |
| 2.7 交易指令检查 | test_07_instruction_check | 错合约、错最小变动价位、超最大手数，本地拒绝 | 否 |
| 2.8 错误提示 | test_08_error_message | 接收 notifyOrder 中 ErrorNo；三种 case 由环境制造 | 是，需 --live |
| 2.9 暂停交易 | test_09_pause_trade | TradingGate 暂停后阻止下单 | 否 |
| 2.10 批量撤单 | test_10_batch_cancel | 建两张挂单，调用 cancelMultiOrders | 是，需 --live |
| 2.11 日志记录 | test_11_logging | 交易/系统/监测/错误四类日志 + YD连接事件 | 否（示例交易日志） |

## 2.8 特别说明

“资金不足”“无仓可平”“市场状态不允许”是柜台/交易所环境相关错误，程序无法无条件制造同一个错误。
因此 test_08_error_message 提供：

- `--case no-position`
- `--case insufficient-funds`
- `--case market-state`

测试时必须先确保测试账户和市场状态满足相应前置条件，并以实际 `ErrorNo` 为证据。

## 安全设计

所有会发送真实报单/撤单的程序默认 SKIP，必须显式加 `--live` 才会发送请求。
这避免把开发机配置误指向生产柜台时自动发单。
