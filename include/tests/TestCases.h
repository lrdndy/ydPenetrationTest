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
int runTest071InvalidInstrument(const RunOptions&);
int runTest072InvalidPrice(const RunOptions&);
int runTest073InvalidVolume(const RunOptions&);
int runTest07InstructionCheck(const RunOptions&);
int runTest081InsufficientFunds(const RunOptions&);
int runTest082NoPosition(const RunOptions&);
int runTest083MarketState(const RunOptions&);
int runTest08ErrorMessage(const RunOptions&);
int runTest14ManualPriceOrder(const RunOptions&);
int runTest15ManualBasicTrade(const RunOptions&);
int runTest16ManualOrderCount(const RunOptions&);
int runTest09PauseTrade(const RunOptions&);
int runTest10BatchCancel(const RunOptions&);
int runTest11Logging(const RunOptions&);
int runTest13SellAg2610(const RunOptions&);
int runAllTests(const RunOptions&);
}
