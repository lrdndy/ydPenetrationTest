#pragma once
#include "core/Common.h"
#include "core/Logger.h"

namespace ydtest {
struct OrderIntent {
    std::string instrument;
    int direction = 0;
    int offset = 0;
    double price = 0;
    int volume = 0;
    bool cancel = false;
};
class Monitor {
public:
    Monitor(Logger& log, int orderThreshold, int cancelThreshold, int duplicateThreshold);
    void recordOrder(const OrderIntent& x);
    void recordCancel(const OrderIntent& x);
    int orderCount() const { return orderCount_; }
    int cancelCount() const { return cancelCount_; }
    int duplicateCount() const { return duplicateCount_; }
    bool orderAlerted() const { return orderAlerted_; }
    bool cancelAlerted() const { return cancelAlerted_; }
    bool duplicateAlerted() const { return duplicateAlerted_; }
private:
    std::string key(const OrderIntent& x) const;
    void checkThresholds();
    Logger& log_;
    int orderThreshold_, cancelThreshold_, duplicateThreshold_;
    int orderCount_=0,cancelCount_=0,duplicateCount_=0;
    bool orderAlerted_=false,cancelAlerted_=false,duplicateAlerted_=false;
    std::unordered_map<std::string,int> seen_;
};

class TradingGate {
public:
    void pause(){ paused_=true; }
    void resume(){ paused_=false; }
    bool canTrade() const { return !paused_; }
private: bool paused_=false;
};
}
