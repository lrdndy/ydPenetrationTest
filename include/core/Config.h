#pragma once
#include "core/Common.h"

namespace ydtest {
class Config {
public:
    bool load(const std::string& path);
    std::string get(const std::string& key, const std::string& def = "") const;
    int getInt(const std::string& key, int def = 0) const;
    double getDouble(const std::string& key, double def = 0.0) const;
    bool getBool(const std::string& key, bool def = false) const;
private:
    std::unordered_map<std::string, std::string> values_;
};

struct Account {
    std::string username;
    std::string password;
    std::string label;
};

std::vector<Account> loadAccountsCsv(const std::string& path);

struct RunOptions {
    std::string ydConfig = "config/yd_config.txt";
    std::string testConfig = "config/test_config.ini";
    std::string accounts = "config/accounts.local.csv";
    std::string instrument;
    std::string caseName;
    std::string logDate;
    std::string outputRoot = "output";
    // Manual fixed-price order (test_14) — no market data required.
    double price = 0.0;
    int orderVolume = 1;
    std::string direction = "buy";
    std::string offset = "open";
    bool keepWorking = false;
    // Manual basic-trade workflow (test_15) — no market data required.
    double openPrice = 0.0;
    double passivePrice = 0.0;
    double closePrice = 0.0;
    // Manual order/cancel count monitoring (test_16) — no market data required.
    double countPrice = 0.0;
    int count = 2;
    bool live = false;
    bool allowExistingTodayPosition = false;
    bool interactive = false;
};

RunOptions parseArgs(int argc, char** argv);
void printCommonUsage(const char* exe);
}
