#include "core/Logger.h"
namespace ydtest {
Logger::Logger(const std::filesystem::path& file) : file_(file) {
    std::filesystem::create_directories(file.parent_path());
    out_.open(file, std::ios::out | std::ios::app);
    if (!out_) throw std::runtime_error("cannot open log: " + file.string());
}
void Logger::write(const char* level, const std::string& category, const std::string& msg) {
    std::lock_guard<std::mutex> lk(mu_);
    std::ostringstream line;
    line << '[' << timestampText() << "] [" << level << "] [" << category << "] " << msg;
    std::cout << line.str() << std::endl;
    out_ << line.str() << '\n'; out_.flush();
}
void Logger::info(const std::string& c, const std::string& m){ write("INFO", c, m); }
void Logger::warn(const std::string& c, const std::string& m){ write("WARN", c, m); }
void Logger::error(const std::string& c, const std::string& m){ write("ERROR", c, m); }
}
