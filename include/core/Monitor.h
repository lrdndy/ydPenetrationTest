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

struct DuplicateStatistics {
    int openInstructions = 0;
    int closeInstructions = 0;
    int cancelInstructions = 0;
    int duplicateOpenInstructions = 0;
    int duplicateCloseInstructions = 0;
    int duplicateCancelInstructions = 0;

    int duplicateTotal() const {
        return duplicateOpenInstructions + duplicateCloseInstructions + duplicateCancelInstructions;
    }
};

struct MonitorThresholds {
    int orderCount = 0;
    int cancelCount = 0;
    int duplicateCount = 0;
    bool popupEnabled = false;
};

class Monitor {
public:
    Monitor(Logger& log, std::string account, MonitorThresholds thresholds = {});
    ~Monitor();
    void recordOrder(const OrderIntent& x);
    void recordCancel(const OrderIntent& x);
    void logThresholdConfiguration();
    void logRiskStatistics() const;
    void logDuplicateStatistics() const;
    int orderCount() const { return orderCount_; }
    int cancelCount() const { return cancelCount_; }
    int duplicateCount() const { return duplicateCount_; }
    const DuplicateStatistics& duplicateStatistics() const { return duplicateStatistics_; }
    bool orderAlerted() const { return orderAlerted_; }
    bool cancelAlerted() const { return cancelAlerted_; }
    bool duplicateAlerted() const { return duplicateAlerted_; }
private:
    enum class InstructionType { Open, Close, Cancel };
    std::string key(const OrderIntent& x) const;
    InstructionType instructionType(const OrderIntent& x) const;
    void record(const OrderIntent& x);
    void emitAlert(const char* event, int value, int threshold);
    void checkThresholds();
    Logger& log_;
    std::string account_;
    MonitorThresholds thresholds_;
    int orderCount_=0,cancelCount_=0,duplicateCount_=0;
    DuplicateStatistics duplicateStatistics_;
    bool thresholdConfigurationLogged_=false;
    bool orderAlerted_=false,cancelAlerted_=false,duplicateAlerted_=false;
    std::unordered_map<std::string,int> seen_;
    std::vector<std::thread> popupThreads_;
};

class TradingGate {
public:
    bool pause(){ return !paused_.exchange(true,std::memory_order_acq_rel); }
    bool resume(){ return paused_.exchange(false,std::memory_order_acq_rel); }
    bool canTrade() const { return !paused_.load(std::memory_order_acquire); }
    bool paused() const { return paused_.load(std::memory_order_acquire); }
private:
    std::atomic<bool> paused_{false};
};
}
