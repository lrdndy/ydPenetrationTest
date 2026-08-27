# Codex Context - ydPenetrationTest

## Project purpose

`ydPenetrationTest` is a C++ project for automated YD API functional testing.

The goal is programmatic testing for external-system certification / penetration-style functional tests, including multiple accounts. It is separate from the EMA strategy demo.

## Main tests

- test_01_connect: connectivity and login
- test_12_market_position: read-only login, market-data, and current-position snapshot
- test_02_basic_trade: order-lifecycle flow with 2 opening fills, 2 independent cancellations, and 2 closing fills; every order result includes account/instrument/price/status details and keeps the `--live` gate. For exchanges with `UseTodayPosition=true`, an existing historical speculative long is allowed when baseline today/other long positions are zero. An intentional non-zero today-position quantity additionally requires `--allow-existing-today-position`; this opt-in preserves the final quantity baseline but cannot preserve individual today-lot identity or cost detail. `other` must remain zero, all owned closes use `CLOSE_TODAY`, and final today/history/other quantities must exactly match the baseline. Other exchanges still require zero baseline long position.
- test_03_reconnect: generation-ordered normal/disconnected/reconnected/re-login/caught-up monitoring; no orders
- test_04_order_cancel_count: live same-account 2-order/2-cancel monitoring with API/callback counters, ready-session callback quieting, immutable order/trade snapshots, shutdown-boundary validation, and shared position-restoring cleanup; requires `--live` and uses the same protected-historical-position policy as test_02
- test_05_duplicate_order: account-scoped runtime duplicate-instruction monitoring for open, close, and cancel intents; emits natural monitoring/statistics logs without a synthetic PASS line. Its deterministic samples do not submit to YD, while the same Monitor is embedded at every YdSession real order/cancel API boundary.
- test_06_threshold: deterministic non-live driver for the same configurable order/cancel/duplicate thresholds loaded by every YdSession; emits system-style configuration, ALERT, optional non-blocking Windows warning dialogs, and risk-statistics logs without a synthetic PASS line
- test_07_1_invalid_instrument, test_07_2_invalid_price, test_07_3_invalid_volume: independent recording-friendly entry points for the three mandatory instruction checks. They share YdSession's pre-submit validator, emit natural rejection logs with apiCalled=false, verify only the selected rejection counter changes, and require zero insertOrder request/submission deltas. The invalid-instrument test treats `--instrument` as a nonexistent contract; price/volume tests treat it as a real reference contract. `test_07_instruction_check` remains as the combined compatibility entry point. YdSession itself defaults live submission off as a second safety gate.
- test_08_1_insufficient_funds, test_08_2_no_position, test_08_3_market_state: independent live cabinet-error monitoring entry points. They keep the `--live` gate, receive owned-order ErrorNo values through notifyOrder, and emit production-style ORDER_REJECTED/ORDER_ERROR_STATISTICS logs without expected-code, scenario, match, or synthetic PASS wording. The insufficient-funds case uses cumulative working orders: it submits up to 100 sequential small BUY OPEN orders, keeps each wholly-unfilled QUEUING order active to reserve funds, and stops at the first cabinet error, fill, callback/session failure, or configured order limit. It uses one fixed deep-passive price, enforces a configurable 500ms default minimum interval, waits for a newer market callback before every subsequent submission, verifies that price remains below the current bid and ask, and re-audits all preceding working orders before and after each new order. It then requests cancellation for all known working orders before waiting for terminal states, requires exact order/trade callback-volume agreement before restoring any callback-attributable residual long exposure, and seals only after the final per-order scan proves the first N-1 triggers are zero-fill CANCELED orders and trigger N is the sole ErrorNo=2 rejection. `ErrorTest.InsufficientFundsMaxRestoreAttempts` bounds emergency close attempts, with a hard cap of 100. A non-zero long baseline is allowed, but only quantity restoration—not today-lot identity or cost detail—is guaranteed. The no-position case still requires zero long position, while the market-state case retains its zero-position safety preflight. `test_08_error_message --case ...` remains compatible.
- test_09_pause_trade: implements test point 1 (account trading-permission restriction) with an account-session TradingGate integrated at the start of every YdSession sendLimitOrder path and serialized with submissions, so no new order can pass after pauseTrading returns; cancellation remains available. `--interactive` keeps a market/heartbeat runtime loop active while a background terminal reader accepts pause/resume/status/:wq without a blocking command prompt. On Windows it uses non-echoing raw key input, recognizes ASCII/full-width-colon `:wq`/`：wq` and `wq` without Enter, then first closes the trading gate and verifies the common order path is blocked before graceful shutdown. The normal runtime output intentionally omits shortcut instructions and terminal-listener diagnostics. Default mode performs the same safe automatic check. It rejects `--live`, emits production-style TRADING_PAUSED/ORDER_BLOCKED/TRADING_CONTROL_STATISTICS logs, and verifies zero API request/submission deltas without a synthetic PASS line.
- test_10_batch_cancel: implements optional test point 2 with two owned, wholly-unfilled QUEUING orders and exactly one cancelMultiOrders call. YdSession atomically validates every target's ownership, latest state, instrument, and bound system ID before creating per-order cancel attempts; it separately counts one batch API call and two target cancellations so callback confirmation and failed-cancel attribution remain reliable. The test reuses the live workflow's protected-baseline preflight, passive pricing, unexpected-fill restoration, callback quieting, and shutdown-boundary seal. Existing historical speculative longs are allowed when `UseTodayPosition=true` and baseline today/other longs are zero; an intentional non-zero today baseline additionally requires `--allow-existing-today-position`, while other must remain zero. Other exchanges still require zero baseline long position. Success emits production-style BATCH_CANCEL_REQUEST/BATCH_CANCEL_RESULT/BATCH_CANCEL_STATISTICS records via OBSERVED rather than synthetic PASS. It always requires --live and does not deliberately create partial fills for optional point 1.
- test_11_logging: read-only same-account log archive audit. It rejects `--live` and only indexes correlated real order/cabinet callbacks, confirmed order/cancel statistics, full YD lifecycle records, and cabinet `notifyOrder` errors; it does not create example records or synthetic PASS lines.
- test_13_sell_ag2610: standalone live sell round trip, intentionally excluded from test_all. It defaults to ag2610 unless `--instrument` overrides it, requires `--live`, buys one lot open as test-owned setup, verifies the position increased by exactly one before allowing the sell, then sells one lot with CLOSE_TODAY/CLOSE as supported, seals the callback stream, and requires the long-position date buckets to return exactly to baseline. Every account row performs two real fills, so use a target-account-only CSV.
- test_all

## Important architecture

`YdSession` wraps YD API callbacks:

- notifyEvent
- notifyReadyForLogin
- notifyLogin
- notifyFinishInit
- notifyCaughtUp
- notifyOrder
- notifyTrade

Do not replace callback synchronization with arbitrary sleeps.

## Account safety

Real credentials must not be committed.

`config/accounts.local.csv` is local only.

Environment-specific YD endpoint and authentication files such as `config/yd_config.local.txt` and `config/accounts.yida.local.csv` are local-only and Git-ignored. `YdSession` explicitly passes the configured AppID/AuthCode to `login`; login records may show the account and AppID for traceability, but must never log the password or AuthCode. This YD SDK login API does not take a separate Broker ID.

Live order tests must keep the `--live` protection.

## Confirmed status

Windows build succeeded with Visual Studio 2019 / MSVC 19.29.

Generated executables are under:

    build/bin/

`test_01_connect.exe` successfully completed:

    TCP connected
    login OK
    notifyFinishInit
    notifyCaughtUp

## Historical callback handling (implemented)

During `test_02_basic_trade --live`, many historical callbacks are printed:

Examples:

    notifyOrder ref=20002532 instrument=sc2410P570
    notifyOrder ref=20002516 instrument=sc2410P590

These are not necessarily orders created by the current test.

YD sends historical order/trade callbacks before `notifyCaughtUp`.

The current implementation keeps historical callbacks in state while suppressing their per-order/per-trade log flood:

Before caught-up:

- process callbacks
- count them
- avoid printing every historical order/trade

After caught-up:

- focus logging on orders created by this test process

After caught-up, callbacks for owned requests are accepted only when the account, instrument, submitted order fields, connection source, and bound system order ID agree; `OrderRef` alone is not treated as physical-order identity.

YD may emit an early owned-order callback with `OrderSysID=-1` and `LongOrderSysID=-1` before assigning the cabinet order ID. Only positive system IDs are considered assigned. A valid early callback may be retained without binding identity; the first later callback carrying a positive ID binds it. Cancel paths must wait for that binding, and any later conflicting positive ID must still be rejected.

Do not break:

- waitOrder
- waitTrade
- condition_variable synchronization

## Relevant files

- include/core/YdSession.h
- include/core/SystemOrderIdentity.h
- src/core/YdSession.cpp
- src/tests/TestCases.cpp
- tests/SystemOrderIdentityTests.cpp
- include/core/Logger.h
- src/core/Logger.cpp

After changes:

1. Build project
2. Run non-live regression tests with CTest
3. Show diff
4. Do not automatically run live trading tests
