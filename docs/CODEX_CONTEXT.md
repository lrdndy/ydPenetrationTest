# Codex Context - ydPenetrationTest

## Project purpose

`ydPenetrationTest` is a C++ project for automated YD API functional testing.

The goal is programmatic testing for external-system certification / penetration-style functional tests, including multiple accounts. It is separate from the EMA strategy demo.

## Main tests

- test_01_connect: connectivity and login
- test_12_market_position: read-only login, market-data, and current-position snapshot
- test_02_basic_trade: order-lifecycle flow with 2 opening fills, 2 independent cancellations, and 2 closing fills; every order result includes account/instrument/price/status details and keeps the `--live` gate
- test_03_reconnect: generation-ordered normal/disconnected/reconnected/re-login/caught-up monitoring; no orders
- test_04_order_cancel_count: live same-account 2-order/2-cancel monitoring with API/callback counters, ready-session callback quieting, immutable order/trade snapshots, shutdown-boundary validation, and shared position-restoring cleanup; requires `--live`
- test_05_duplicate_order
- test_06_threshold
- test_07_instruction_check
- test_08_error_message
- test_09_pause_trade
- test_10_batch_cancel
- test_11_logging
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

Do not break:

- waitOrder
- waitTrade
- condition_variable synchronization

## Relevant files

- include/core/YdSession.h
- src/core/YdSession.cpp
- src/tests/TestCases.cpp
- include/core/Logger.h
- src/core/Logger.cpp

After changes:

1. Build project
2. Show diff
3. Do not automatically run live trading tests
