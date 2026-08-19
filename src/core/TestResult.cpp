#include "core/TestResult.h"
namespace ydtest {
static const char* nameOf(Outcome o){ return o==Outcome::Pass?"PASS":(o==Outcome::Fail?"FAIL":(o==Outcome::Skip?"SKIP":"OBSERVED")); }
static std::string csvEscape(std::string s){
    bool q = s.find_first_of(",\"\n") != std::string::npos;
    size_t p=0; while((p=s.find('"',p))!=std::string::npos){ s.insert(p,"\""); p+=2; }
    return q ? '"'+s+'"' : s;
}
TestResult::TestResult(std::string id, std::string account, Logger& log):id_(std::move(id)),account_(std::move(account)),log_(log){}
void TestResult::add(Outcome o,const std::string& n,const std::string& d){
    checks_.push_back({o,n,d});
    if(o!=Outcome::Observed)log_.info("RESULT",std::string(nameOf(o))+" | "+n+(d.empty()?"":" | "+d));
}
void TestResult::pass(const std::string& n,const std::string& d){add(Outcome::Pass,n,d);} void TestResult::fail(const std::string& n,const std::string& d){add(Outcome::Fail,n,d);} void TestResult::skip(const std::string& n,const std::string& d){add(Outcome::Skip,n,d);} void TestResult::observe(const std::string& n,const std::string& d){add(Outcome::Observed,n,d);}
bool TestResult::failed() const { for(auto& c:checks_) if(c.outcome==Outcome::Fail) return true; return false; }
bool TestResult::skippedOnly() const { if(checks_.empty()) return true; for(auto& c:checks_) if(c.outcome!=Outcome::Skip) return false; return true; }
Outcome TestResult::overall() const { if(failed()) return Outcome::Fail; if(skippedOnly()) return Outcome::Skip; return Outcome::Pass; }
void TestResult::writeSummary(const std::filesystem::path& csv) const {
    std::filesystem::create_directories(csv.parent_path());
    const bool exists=std::filesystem::exists(csv);
    std::ofstream out(csv,std::ios::app); if(!exists) out<<"time,test_id,account,outcome,check,detail\n";
    for(auto& c:checks_) out<<timestampText()<<','<<csvEscape(id_)<<','<<csvEscape(account_)<<','<<nameOf(c.outcome)<<','<<csvEscape(c.name)<<','<<csvEscape(c.detail)<<'\n';
}
}
