#pragma once
#include "core/Common.h"

namespace ydtest {
class Logger {
public:
    explicit Logger(const std::filesystem::path& file);
    void info(const std::string& category, const std::string& msg);
    void warn(const std::string& category, const std::string& msg);
    void error(const std::string& category, const std::string& msg);
    const std::filesystem::path& file() const { return file_; }
private:
    void write(const char* level, const std::string& category, const std::string& msg);
    std::mutex mu_;
    std::ofstream out_;
    std::filesystem::path file_;
};
}
