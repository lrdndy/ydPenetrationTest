# 附件3测试项与程序对应关系

| 附件3 | 可执行程序 | 自动化方式 | 是否发真实报单 |
|---|---|---|---|
| 1.2 登录后查阅行情或持仓（补充） | test_12_market_position | 登录成功后打印账号、时间、指定合约行情和非零持仓 | 否，纯只读 |
| 2.1 连通性 | test_01_connect | 连接、认证登录、静态数据、caught up | 否 |
| 2.2 / 3.2.2 基础交易功能 | test_02_basic_trade | 逐笔完成买开成交 2 笔、独立撤单 2 笔、卖平成交 2 笔；每笔输出账号、合约、委托价、成交价/状态等证据 | 是，需 --live |
| 2.3 / 3.3.1.1 连接状态异常监测 | test_03_reconnect | 登录后记录正常状态；调用 disconnect，并按新事件序号等待断开、重连、重新登录和 caught-up | 否 |
| 2.4 / 3.3.1.2 报撤单笔数监测 | test_04_order_cancel_count | 同一真实账号逐笔提交 2 张被动单并分别撤单；统计 API 请求，用账号、合约、订单字段和系统单号绑定柜台确认，并在会话正常的静默窗口后复核委托/成交量及关闭边界，意外成交时恢复仓位 | 是，需 --live |
| 2.5 重复报单监测 | test_05_duplicate_order | 同一账号下按合约/方向/开平/价/量识别相同指令，分别显示开仓、平仓、撤单的指令数、相同指令出现次数和重复笔数；监测组件同时接入所有 `YdSession` 真实报单、单笔撤单和批量撤单边界 | test_05 样本不报单；其他实盘项目仍需 --live |
| 2.6 阈值设置预警 | test_06_threshold | 配置报单、撤单、重复报单阈值并自然输出配置、达到阈值的终端 ALERT、可选 Windows 置顶弹窗和最终风险统计；相同阈值由每个 YdSession 加载并作用于真实报撤单入口 | test_06 样本不报单；其他实盘项目仍需 --live |
| 2.7.1 合约代码检查 | test_07_1_invalid_instrument | `--instrument`/`Validation.InvalidInstrument` 指定待拦截的不存在合约；持续送入 YdSession 公共入口并核对仅合约拒绝计数增加、apiCalled=false、API 请求/提交增量均为 0 | 否；会话级真实提交门保持关闭 |
| 2.7.2 最小变动价位检查 | test_07_2_invalid_price | `--instrument`/`Validation.ReferenceInstrument` 指定真实参考合约；按其 Tick 构造非法价位并核对仅价格拒绝计数增加、apiCalled=false、API 请求/提交增量均为 0 | 否；会话级真实提交门保持关闭 |
| 2.7.3 单笔最大委托数量检查 | test_07_3_invalid_volume | `--instrument`/`Validation.ReferenceInstrument` 指定真实参考合约；按其最大委托手数构造超限数量并核对仅数量拒绝计数增加、apiCalled=false、API 请求/提交增量均为 0 | 否；会话级真实提交门保持关闭 |
| 2.7 汇总入口 | test_07_instruction_check | 保留兼容入口，一次执行上述三个测试点；`--instrument` 表示不存在合约，有效参考合约从 `Validation.ReferenceInstrument` 读取 | 否；会话级真实提交门保持关闭 |
| 2.8.1 资金不足错误提示 | test_08_1_insufficient_funds | 真实开仓指令经 YdSession 送到柜台；从拥有订单的 notifyOrder 接收并自然显示资金不足 ErrorNo=2，同时核对安全收尾 | 是，需 --live；需低资金账号或适当保证金/数量 |
| 2.8.2 持仓不足错误提示 | test_08_2_no_position | 报单前确认该合约投机多仓为 0，再发送平仓指令；从 notifyOrder 接收并自然显示无仓可平 ErrorNo=1 | 是，需 --live |
| 2.8.3 市场状态错误提示 | test_08_3_market_state | 收盘或暂停期间发送被动开仓指令；接收并显示市场状态错误码 37/66/93/133/138 | 是，需 --live；建议收盘期间 |
| 2.8 兼容入口 | test_08_error_message | 通过 `--case no-position|insufficient-funds|market-state` 选择上述单项 | 是，需 --live |
| 2.9 暂停交易（采用测试点1：限制账号交易权限） | test_09_pause_trade | `--interactive` 下持续接收并显示行情/运行心跳，Windows 原始按键线程在后台监听暂停与退出控制，不显示操作提示、监听参数或 command 提示，也不回显输入；ASCII/全角冒号 `:wq`、`：wq` 或 `wq` 无需回车，先关闭新报单权限再安全退出。账号会话级线程安全 TradingGate 嵌入 YdSession 公共报单入口，暂停后策略订单显示 ORDER_BLOCKED、apiCalled=false，并核对 API 请求/提交增量为0；撤单不受暂停影响 | 否；程序拒绝 --live |
| 2.10 批量撤单（采用测试点2：多笔已报单） | test_10_batch_cancel | 同一账号提交两张一手被动买开单并逐张确认 QUEUING、成交量0及系统单号；公共入口整批核验归属后只调用一次 cancelMultiOrders，再逐张确认 CANCELED。最终输出 batchApiCalls=1、batchTargetOrders=2、confirmedCancellations=2、无成交/挂单、仓位恢复及回报流封口证据；意外成交时安全恢复但不冒充测试点1 | 是，需 --live |
| 2.11 日志记录 | test_11_logging | 离线索引指定/最近归档日期同账号的真实日志：以 OrderRef 关联已提交报单与柜台回报，核对完整登录退出生命周期、真实报撤单统计、安全封口及柜台 notifyOrder 错误，并输出来源文件与保存路径 | 否；拒绝 --live，需先有真实业务日志 |

## 2.8 特别说明

“资金不足”“无仓可平”“市场状态不允许”是柜台/交易所环境相关错误，程序无法无条件制造同一个错误。
因此三个独立程序依赖相应柜台环境；兼容入口仍提供：

- `--case no-position`
- `--case insufficient-funds`
- `--case market-state`

测试时必须先确保测试账户和市场状态满足相应前置条件，并以实际 `notifyOrder`、`ORDER_REJECTED`、`ErrorNo` 及 `ORDER_ERROR_STATISTICS` 为证据。若柜台未按预期拒绝，程序会撤销仍在工作的指令，并仅按本程序拥有订单的回报尝试恢复意外新增仓位；出现 `MANUAL ACTION REQUIRED` 时必须人工核对柜台。

## 安全设计

所有会发送真实报单/撤单的程序默认 SKIP，必须显式加 `--live` 才会发送请求。
这避免把开发机配置误指向生产柜台时自动发单。
