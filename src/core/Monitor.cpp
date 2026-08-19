#include "core/Monitor.h"
#include "ydApi.h"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace ydtest {
namespace {
const char* directionName(int value) {
    return value == YD_D_Buy ? "BUY" : (value == YD_D_Sell ? "SELL" : "UNKNOWN");
}

const char* offsetName(int value) {
    switch (value) {
        case YD_OF_Open: return "OPEN";
        case YD_OF_Close: return "CLOSE";
        case YD_OF_ForceClose: return "FORCE_CLOSE";
        case YD_OF_CloseToday: return "CLOSE_TODAY";
        case YD_OF_CloseYesterday: return "CLOSE_YESTERDAY";
        default: return "UNKNOWN";
    }
}

std::string priceText(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(8) << value;
    auto text = out.str();
    while (text.size() > 1 && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text;
}
}

Monitor::Monitor(Logger& log, std::string account, MonitorThresholds thresholds)
    : log_(log), account_(std::move(account)), thresholds_(thresholds) {}

Monitor::~Monitor() {
    for (auto& thread : popupThreads_) if (thread.joinable()) thread.join();
}

std::string Monitor::key(const OrderIntent& x) const {
    std::ostringstream out;
    out << x.cancel << '|' << account_ << '|' << x.instrument << '|' << x.direction << '|'
        << x.offset << '|' << std::fixed << std::setprecision(8) << x.price << '|' << x.volume;
    return out.str();
}

Monitor::InstructionType Monitor::instructionType(const OrderIntent& x) const {
    if (x.cancel) return InstructionType::Cancel;
    return x.offset == YD_OF_Open ? InstructionType::Open : InstructionType::Close;
}

void Monitor::recordOrder(const OrderIntent& x) {
    ++orderCount_;
    record(x);
}

void Monitor::recordCancel(const OrderIntent& x) {
    ++cancelCount_;
    record(x);
}

void Monitor::record(const OrderIntent& x) {
    logThresholdConfiguration();
    const InstructionType type = instructionType(x);
    int* instructionCount = nullptr;
    int* categoryDuplicateCount = nullptr;
    const char* typeName = nullptr;
    switch (type) {
        case InstructionType::Open:
            instructionCount = &duplicateStatistics_.openInstructions;
            categoryDuplicateCount = &duplicateStatistics_.duplicateOpenInstructions;
            typeName = "OPEN";
            break;
        case InstructionType::Close:
            instructionCount = &duplicateStatistics_.closeInstructions;
            categoryDuplicateCount = &duplicateStatistics_.duplicateCloseInstructions;
            typeName = "CLOSE";
            break;
        case InstructionType::Cancel:
            instructionCount = &duplicateStatistics_.cancelInstructions;
            categoryDuplicateCount = &duplicateStatistics_.duplicateCancelInstructions;
            typeName = "CANCEL";
            break;
    }

    ++*instructionCount;
    const int occurrence = ++seen_[key(x)];
    const bool duplicate = occurrence > 1;
    if (duplicate) {
        ++duplicateCount_;
        ++*categoryDuplicateCount;
    }

    std::ostringstream line;
    line << "account=" << account_
         << " event=" << (duplicate ? "DUPLICATE_INSTRUCTION" : "INSTRUCTION_RECEIVED")
         << " type=" << typeName
         << " instrument=" << x.instrument
         << " direction=" << directionName(x.direction)
         << " offset=" << offsetName(x.offset)
         << " price=" << priceText(x.price)
         << " volume=" << x.volume
         << " sameInstructionCount=" << occurrence
         << " typeInstructionCount=" << *instructionCount
         << " typeDuplicateCount=" << *categoryDuplicateCount
         << " duplicateTotal=" << duplicateCount_;
    if (duplicate) log_.warn("MONITOR", line.str());
    else log_.info("MONITOR", line.str());
    checkThresholds();
}

void Monitor::logThresholdConfiguration() {
    if (thresholdConfigurationLogged_) return;
    thresholdConfigurationLogged_ = true;
    log_.info("MONITOR", "account=" + account_
        + " event=THRESHOLD_CONFIGURATION"
        + " orderCountThreshold=" + std::to_string(thresholds_.orderCount)
        + " cancelCountThreshold=" + std::to_string(thresholds_.cancelCount)
        + " duplicateCountThreshold=" + std::to_string(thresholds_.duplicateCount)
        + " popupEnabled=" + (thresholds_.popupEnabled ? std::string("true") : std::string("false")));
}

void Monitor::logRiskStatistics() const {
    log_.info("MONITOR", "account=" + account_
        + " event=RISK_STATISTICS"
        + " orderCount=" + std::to_string(orderCount_)
        + " orderCountThreshold=" + std::to_string(thresholds_.orderCount)
        + " orderAlerted=" + (orderAlerted_ ? std::string("true") : std::string("false"))
        + " cancelCount=" + std::to_string(cancelCount_)
        + " cancelCountThreshold=" + std::to_string(thresholds_.cancelCount)
        + " cancelAlerted=" + (cancelAlerted_ ? std::string("true") : std::string("false"))
        + " duplicateCount=" + std::to_string(duplicateCount_)
        + " duplicateCountThreshold=" + std::to_string(thresholds_.duplicateCount)
        + " duplicateAlerted=" + (duplicateAlerted_ ? std::string("true") : std::string("false")));
}

void Monitor::logDuplicateStatistics() const {
    std::ostringstream line;
    line << "account=" << account_
         << " event=DUPLICATE_STATISTICS"
         << " openInstructionCount=" << duplicateStatistics_.openInstructions
         << " openDuplicateCount=" << duplicateStatistics_.duplicateOpenInstructions
         << " closeInstructionCount=" << duplicateStatistics_.closeInstructions
         << " closeDuplicateCount=" << duplicateStatistics_.duplicateCloseInstructions
         << " cancelInstructionCount=" << duplicateStatistics_.cancelInstructions
         << " cancelDuplicateCount=" << duplicateStatistics_.duplicateCancelInstructions
         << " duplicateTotal=" << duplicateStatistics_.duplicateTotal();
    log_.info("MONITOR", line.str());
}

void Monitor::emitAlert(const char* event, int value, int threshold) {
    const std::string line = "account=" + account_ + " event=" + event
        + " value=" + std::to_string(value) + " threshold=" + std::to_string(threshold);
    log_.warn("ALERT", line);
    if (!thresholds_.popupEnabled) return;
#ifdef _WIN32
    const std::string title = "YD Risk Alert";
    const std::string message = "Account: " + account_ + "\r\nEvent: " + event
        + "\r\nCurrent value: " + std::to_string(value)
        + "\r\nThreshold: " + std::to_string(threshold);
    popupThreads_.emplace_back([title,message] {
        MessageBoxA(nullptr, message.c_str(), title.c_str(), MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    });
#else
    log_.warn("ALERT", "account=" + account_ + " event=POPUP_UNAVAILABLE platform=non_windows originalEvent=" + event);
#endif
}

void Monitor::checkThresholds(){
    if(thresholds_.orderCount>0 && orderCount_>=thresholds_.orderCount && !orderAlerted_){orderAlerted_=true;emitAlert("ORDER_COUNT_THRESHOLD",orderCount_,thresholds_.orderCount);}
    if(thresholds_.cancelCount>0 && cancelCount_>=thresholds_.cancelCount && !cancelAlerted_){cancelAlerted_=true;emitAlert("CANCEL_COUNT_THRESHOLD",cancelCount_,thresholds_.cancelCount);}
    if(thresholds_.duplicateCount>0 && duplicateCount_>=thresholds_.duplicateCount && !duplicateAlerted_){duplicateAlerted_=true;emitAlert("DUPLICATE_ORDER_THRESHOLD",duplicateCount_,thresholds_.duplicateCount);}
}
}
