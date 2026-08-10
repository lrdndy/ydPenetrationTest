#pragma once
#include "core/Common.h"
#include "core/Logger.h"

namespace ydtest {
enum class Outcome { Pass, Fail, Skip };
struct Check { Outcome outcome; std::string name; std::string detail; };
class TestResult {
public:
    TestResult(std::string id, std::string account, Logger& log);
    void pass(const std::string& name, const std::string& detail = "");
    void fail(const std::string& name, const std::string& detail = "");
    void skip(const std::string& name, const std::string& detail = "");
    bool failed() const;
    bool skippedOnly() const;
    Outcome overall() const;
    void writeSummary(const std::filesystem::path& csv) const;
private:
    void add(Outcome o, const std::string& name, const std::string& detail);
    std::string id_, account_;
    Logger& log_;
    std::vector<Check> checks_;
};
}
