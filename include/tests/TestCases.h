#pragma once
#include "core/Config.h"
namespace ydtest {
int runTest01Connect(const RunOptions&);
int runTest12MarketPosition(const RunOptions&);
int runTest02BasicTrade(const RunOptions&);
int runTest03Reconnect(const RunOptions&);
int runTest04OrderCancelCount(const RunOptions&);
int runTest05Duplicate(const RunOptions&);
int runTest06Threshold(const RunOptions&);
int runTest07InstructionCheck(const RunOptions&);
int runTest08ErrorMessage(const RunOptions&);
int runTest09PauseTrade(const RunOptions&);
int runTest10BatchCancel(const RunOptions&);
int runTest11Logging(const RunOptions&);
int runAllTests(const RunOptions&);
}
