# Codex Context - ydPenetrationTest

## Project purpose

`ydPenetrationTest` is a C++ project for automated YD API functional testing.

The goal is programmatic testing for external-system certification / penetration-style functional tests, including multiple accounts. It is separate from the EMA strategy demo.

## Main tests

- test_01_connect: connectivity and login
- test_02_basic_trade: open, close, cancel
- test_03_reconnect: disconnect and reconnect
- test_04_order_cancel_count
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

## Current issue: log flooding in test_02

During `test_02_basic_trade --live`, many historical callbacks are printed:

Examples:

    notifyOrder ref=20002532 instrument=sc2410P570
    notifyOrder ref=20002516 instrument=sc2410P590

These are not necessarily orders created by the current test.

YD sends historical order/trade callbacks before `notifyCaughtUp`.

## Required improvement

Do not ignore historical callbacks. Continue maintaining state.

Before caught-up:

- process callbacks
- count them
- avoid printing every historical order/trade

After caught-up:

- focus logging on orders created by this test process

Track owned OrderRef values instead of filtering only by instrument.

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
