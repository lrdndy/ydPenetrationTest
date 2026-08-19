# 附件3测试项与程序对应关系

| 附件3 | 可执行程序 | 自动化方式 | 是否发真实报单 |
|---|---|---|---|
| 1.2 登录后查阅行情或持仓（补充） | test_12_market_position | 登录成功后打印账号、时间、指定合约行情和非零持仓 | 否，纯只读 |
| 2.1 连通性 | test_01_connect | 连接、认证登录、静态数据、caught up | 否 |
| 2.2 / 3.2.2 基础交易功能 | test_02_basic_trade | 逐笔完成买开成交 2 笔、独立撤单 2 笔、卖平成交 2 笔；每笔输出账号、合约、委托价、成交价/状态等证据 | 是，需 --live |
| 2.3 / 3.3.1.1 连接状态异常监测 | test_03_reconnect | 登录后记录正常状态；调用 disconnect，并按新事件序号等待断开、重连、重新登录和 caught-up | 否 |
| 2.4 / 3.3.1.2 报撤单笔数监测 | test_04_order_cancel_count | 同一真实账号逐笔提交 2 张被动单并分别撤单；统计 API 请求，用账号、合约、订单字段和系统单号绑定柜台确认，并在会话正常的静默窗口后复核委托/成交量及关闭边界，意外成交时恢复仓位 | 是，需 --live |
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
