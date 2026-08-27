#include "core/Config.h"

namespace ydtest {
bool Config::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto p = line.find('=');
        if (p == std::string::npos) continue;
        values_[trim(line.substr(0, p))] = trim(line.substr(p + 1));
    }
    return true;
}
std::string Config::get(const std::string& key, const std::string& def) const {
    const auto it = values_.find(key);
    return it == values_.end() ? def : it->second;
}
int Config::getInt(const std::string& key, int def) const {
    try { return std::stoi(get(key)); } catch (...) { return def; }
}
double Config::getDouble(const std::string& key, double def) const {
    try { return std::stod(get(key)); } catch (...) { return def; }
}
bool Config::getBool(const std::string& key, bool def) const {
    auto v = get(key);
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "yes" || v == "true" || v == "1" || v == "on") return true;
    if (v == "no" || v == "false" || v == "0" || v == "off") return false;
    return def;
}

std::vector<Account> loadAccountsCsv(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open accounts file: " + path);
    std::vector<Account> out;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (first) {
            first = false;
            if (line.find("username") != std::string::npos) continue;
        }
        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) cols.push_back(trim(cell));
        if (cols.size() < 2) continue;
        Account a{cols[0], cols[1], cols.size() >= 3 ? cols[2] : cols[0]};
        if (!a.username.empty()) out.push_back(std::move(a));
    }
    return out;
}

RunOptions parseArgs(int argc, char** argv) {
    RunOptions o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value after " + a);
            return argv[++i];
        };
        if (a == "--yd-config") o.ydConfig = next();
        else if (a == "--test-config") o.testConfig = next();
        else if (a == "--accounts") o.accounts = next();
        else if (a == "--instrument") o.instrument = next();
        else if (a == "--case") o.caseName = next();
        else if (a == "--log-date") o.logDate = next();
        else if (a == "--output") o.outputRoot = next();
        else if (a == "--live") o.live = true;
        else if (a == "--allow-existing-today-position") o.allowExistingTodayPosition = true;
        else if (a == "--interactive") o.interactive = true;
        else if (a == "--help" || a == "-h") { printCommonUsage(argv[0]); std::exit(0); }
        else throw std::runtime_error("unknown argument: " + a);
    }
    return o;
}

void printCommonUsage(const char* exe) {
    std::cout << "Usage: " << exe << " [options]\n"
              << "  --yd-config <file>      YD API config file\n"
              << "  --test-config <file>    Functional-test config\n"
              << "  --accounts <csv>        username,password,label CSV\n"
              << "  --instrument <id>       Override test instrument\n"
              << "  --case <name>            Error-message test case\n"
              << "  --log-date <YYYYMMDD>    Session-start date for the logging archive audit\n"
              << "  --output <dir>           Output root\n"
              << "  --live                   Allow real order/cancel requests\n"
              << "  --allow-existing-today-position\n"
              << "                           Permit a non-zero today-position quantity baseline\n"
              << "  --interactive            Enable manual console commands where supported\n";
}
}
