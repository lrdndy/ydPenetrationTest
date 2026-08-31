#include "tests/TestCases.h"
#include "core/Logger.h"
#include "core/Monitor.h"
#include "core/SystemOrderIdentity.h"
#include "core/TestResult.h"
#include "core/YdSession.h"
#include "ydError.h"
#include <cfloat>
#include <climits>
#include <cmath>
#include <memory>
#ifdef _WIN32
#include <conio.h>
#endif

namespace ydtest {
namespace {
struct Env { Config cfg; std::string runStamp; std::filesystem::path root; std::vector<Account> accounts; };
Env makeEnv(const RunOptions& o){Env e; if(!e.cfg.load(o.testConfig)) throw std::runtime_error("cannot load test config: "+o.testConfig);e.runStamp=timestampForPath();e.root=std::filesystem::path(o.outputRoot)/e.runStamp;e.accounts=loadAccountsCsv(o.accounts);if(e.accounts.empty())throw std::runtime_error("no accounts in "+o.accounts);return e;}
std::string instID(const RunOptions& o,const Config& c){return o.instrument.empty()?c.get("Test.Instrument","au2612"):o.instrument;}
int timeout(const Config& c){return c.getInt("Test.TimeoutSeconds",12);}
MonitorThresholds monitorThresholds(const Config& c){return {std::max(0,c.getInt("Threshold.OrderCount",0)),std::max(0,c.getInt("Threshold.CancelCount",0)),std::max(0,c.getInt("Threshold.DuplicateCount",0)),c.getBool("Threshold.PopupEnabled",false)};}
std::filesystem::path accountLog(const Env& e,const std::string& id,const Account& a){return e.root/id/(a.label.empty()?a.username:a.label)/"test.log";}
int combine(int acc,int code){return (acc||code)?1:0;}

struct ArchivedLogEvidence {
    bool found=false;
    std::filesystem::path file;
    std::string record;
    std::filesystem::file_time_type modified{};
};

struct ArchivedLifecycleEvidence {
    bool found=false;
    std::filesystem::path file;
    std::string started;
    std::string login;
    std::string shutdown;
    std::filesystem::file_time_type modified{};
};

struct ArchivedLogScan {
    std::size_t filesScanned=0;
    ArchivedLogEvidence trade;
    ArchivedLifecycleEvidence lifecycle;
    ArchivedLogEvidence monitoring;
    ArchivedLogEvidence cabinetError;
};

std::optional<std::string> logField(const std::string& line,const std::string& name){
    const std::string prefix=name+'=';
    const auto begin=line.find(prefix);
    if(begin==std::string::npos)return std::nullopt;
    const auto valueBegin=begin+prefix.size();
    auto valueEnd=valueBegin;
    while(valueEnd<line.size()&&!std::isspace(static_cast<unsigned char>(line[valueEnd]))&&line[valueEnd]!='|')++valueEnd;
    if(valueEnd==valueBegin)return std::nullopt;
    return line.substr(valueBegin,valueEnd-valueBegin);
}

std::optional<long long> logIntegerField(const std::string& line,const std::string& name){
    const auto value=logField(line,name);
    if(!value)return std::nullopt;
    try{
        std::size_t parsed=0;
        const long long number=std::stoll(*value,&parsed);
        if(parsed!=value->size())return std::nullopt;
        return number;
    }catch(...){return std::nullopt;}
}

bool logFlag(const std::string& line,const std::string& name,bool expected=true){
    const auto value=logField(line,name);
    return value&&*value==(expected?"true":"false");
}

std::optional<std::string> archivedRecordDate(const std::string& line){
    if(line.size()<11||line[0]!='['||line[5]!='-'||line[8]!='-')return std::nullopt;
    for(const int index:{1,2,3,4,6,7,9,10})if(!std::isdigit(static_cast<unsigned char>(line[index])))return std::nullopt;
    return line.substr(1,10);
}

std::string normalizeArchiveDate(const std::string& value){
    if(value.empty())return {};
    std::string compact;
    for(const char ch:value)if(ch!='-')compact.push_back(ch);
    if(compact.size()!=8||!std::all_of(compact.begin(),compact.end(),[](char ch){return std::isdigit(static_cast<unsigned char>(ch));}))
        throw std::runtime_error("--log-date must use YYYYMMDD or YYYY-MM-DD");
    const int year=std::stoi(compact.substr(0,4)),month=std::stoi(compact.substr(4,2)),day=std::stoi(compact.substr(6,2));
    std::tm calendar{};calendar.tm_year=year-1900;calendar.tm_mon=month-1;calendar.tm_mday=day;calendar.tm_hour=12;calendar.tm_isdst=-1;
    if(std::mktime(&calendar)==static_cast<std::time_t>(-1)||calendar.tm_year!=year-1900||calendar.tm_mon!=month-1||calendar.tm_mday!=day)
        throw std::runtime_error("--log-date is not a valid calendar date");
    return compact.substr(0,4)+'-'+compact.substr(4,2)+'-'+compact.substr(6,2);
}

std::string absolutePathText(const std::filesystem::path& path){
    std::error_code ec;
    const auto absolute=std::filesystem::absolute(path,ec);
    return (ec?path:absolute).lexically_normal().string();
}

void chooseLatest(ArchivedLogEvidence& target,const std::filesystem::path& file,const std::string& record,const std::filesystem::file_time_type modified){
    if(!target.found||modified>=target.modified){target={true,file,record,modified};}
}

void chooseLatest(ArchivedLifecycleEvidence& target,const std::filesystem::path& file,const std::string& started,const std::string& login,const std::string& shutdown,const std::filesystem::file_time_type modified){
    if(!target.found||modified>=target.modified){target={true,file,started,login,shutdown,modified};}
}

bool tradingArchiveId(const std::string& id){return id=="2.2_basic_trade"||id=="2.4_order_cancel_count"||id=="2.10_batch_cancel";}
bool errorArchiveId(const std::string& id){return id=="2.8.1_insufficient_funds"||id=="2.8.2_no_position"||id=="2.8.3_market_state"||id=="2.8_error_message";}
bool evidenceArchiveId(const std::string& id){return tradingArchiveId(id)||errorArchiveId(id);}
bool logAccountIs(const std::string& line,const std::string& account){const auto value=logField(line,"account");return value&&*value==account;}
bool logHasAccount(const std::string& line){return logField(line,"account").has_value();}
bool validArchiveRunStamp(const std::string& value){
    if(value.size()!=15||value[8]!='_')return false;
    for(std::size_t index=0;index<value.size();++index)if(index!=8&&!std::isdigit(static_cast<unsigned char>(value[index])))return false;
    return true;
}

std::optional<std::string> completeLifecycleDate(const std::vector<std::string>& records,const std::string& account){
    const std::size_t missing=records.size();
    std::size_t started=missing,connected=missing,login=missing,init=missing,caughtUp=missing,destroyed=missing;
    bool containsExample=false;
    for(std::size_t index=0;index<records.size();++index){
        const auto& record=records[index];
        containsExample=containsExample||record.find("example")!=std::string::npos;
        if(record.find("[SYSTEM] YD API started;")!=std::string::npos)started=index;
        if(record.find("[API_EVENT]")!=std::string::npos&&record.find("eventName=TCP_TRADE_CONNECTED")!=std::string::npos&&logAccountIs(record,account))connected=index;
        if(record.find("[LOGIN] login OK account=")!=std::string::npos&&logAccountIs(record,account))login=index;
        if(record.find("[SYSTEM] notifyFinishInit: static data ready")!=std::string::npos)init=index;
        if(record.find("[SYSTEM] notifyCaughtUp: history caught up account=")!=std::string::npos&&logAccountIs(record,account))caughtUp=index;
        if(record.find("[SYSTEM] YD API destroyed")!=std::string::npos)destroyed=index;
    }
    if(containsExample||!(started<connected&&connected<login&&login<init&&init<caughtUp&&caughtUp<destroyed))return std::nullopt;
    return archivedRecordDate(records[started]);
}

std::string latestArchivedBusinessDate(const std::filesystem::path& archiveRoot,const std::filesystem::path& currentLog,const std::string& accountDirectory,const std::string& account){
    std::string latest;
    std::error_code ec;
    const auto currentAbsolute=std::filesystem::absolute(currentLog,ec).lexically_normal();
    ec.clear();
    std::filesystem::recursive_directory_iterator it(archiveRoot,std::filesystem::directory_options::skip_permission_denied,ec),end;
    while(!ec&&it!=end){
        const auto entry=*it;
        it.increment(ec);
        if(ec){ec.clear();continue;}
        const std::string testId=entry.path().parent_path().parent_path().filename().string();
        const std::string runStamp=entry.path().parent_path().parent_path().parent_path().filename().string();
        std::error_code typeError;
        if(entry.is_symlink(typeError)||typeError||!entry.is_regular_file(typeError)||typeError||entry.path().filename()!="test.log"
            ||entry.path().parent_path().filename()!=accountDirectory||!evidenceArchiveId(testId)||!validArchiveRunStamp(runStamp))continue;
        std::error_code absoluteError;
        if(std::filesystem::absolute(entry.path(),absoluteError).lexically_normal()==currentAbsolute)continue;
        std::error_code stateError;
        const auto sizeBefore=std::filesystem::file_size(entry.path(),stateError);
        if(stateError||sizeBefore>64ULL*1024ULL*1024ULL)continue;
        const auto modifiedBefore=std::filesystem::last_write_time(entry.path(),stateError);
        if(stateError)continue;
        std::ifstream input(entry.path());
        if(!input)continue;
        std::vector<std::string> records;
        std::string line;
        while(std::getline(input,line))records.push_back(std::move(line));
        const auto sizeAfter=std::filesystem::file_size(entry.path(),stateError);
        if(stateError)continue;
        const auto modifiedAfter=std::filesystem::last_write_time(entry.path(),stateError);
        if(stateError||sizeBefore!=sizeAfter||modifiedBefore!=modifiedAfter)continue;
        std::vector<std::string> session;
        const auto inspectSession=[&](){
            const auto date=completeLifecycleDate(session,account);
            if(date&&*date>latest)latest=*date;
        };
        for(const auto& record:records){
            if(record.find("[SYSTEM] YD API started;")!=std::string::npos){inspectSession();session.clear();}
            if(!session.empty()||record.find("[SYSTEM] YD API started;")!=std::string::npos)session.push_back(record);
        }
        inspectSession();
    }
    return latest;
}

void analyzeArchivedSession(const std::vector<std::string>& records,const std::filesystem::path& file,const std::string& testId,const std::string& account,const std::filesystem::file_time_type modified,ArchivedLogScan& scan){
    bool containsExample=false,manualActionRequired=false;
    std::string apiStarted,tradeConnected,login,init,caughtUp,destroyed,riskStatistics,safeLiveFinal,errorFinal,finalErrorAccount;
    std::set<std::string> submittedRefs,confirmedCancelRefs;
    std::map<std::string,std::string> orderCallbacks,orderResults,batchCancelResults,tradeCallbacks;
    std::map<std::string,std::pair<long long,std::string>> errorCallbacks;
    bool cancelRequestSubmitted=false;
    long long finalErrorNo=0;
    const std::size_t noPosition=records.size();
    std::size_t apiStartedAt=noPosition,tradeConnectedAt=noPosition,loginAt=noPosition,initAt=noPosition,caughtUpAt=noPosition,destroyedAt=noPosition;

    for(std::size_t recordIndex=0;recordIndex<records.size();++recordIndex){
        const auto& record=records[recordIndex];
        containsExample=containsExample||record.find("example")!=std::string::npos;
        manualActionRequired=manualActionRequired||record.find("MANUAL ACTION REQUIRED")!=std::string::npos;
        if(record.find("[SYSTEM] YD API started;")!=std::string::npos){apiStarted=record;apiStartedAt=recordIndex;}
        if(record.find("[API_EVENT]")!=std::string::npos&&record.find("eventName=TCP_TRADE_CONNECTED")!=std::string::npos&&logAccountIs(record,account)){tradeConnected=record;tradeConnectedAt=recordIndex;}
        if(record.find("[LOGIN] login OK account=")!=std::string::npos&&logAccountIs(record,account)){login=record;loginAt=recordIndex;}
        if(record.find("[SYSTEM] notifyFinishInit: static data ready")!=std::string::npos){init=record;initAt=recordIndex;}
        if(record.find("[SYSTEM] notifyCaughtUp: history caught up account=")!=std::string::npos&&logAccountIs(record,account)){caughtUp=record;caughtUpAt=recordIndex;}
        if(record.find("[SYSTEM] YD API destroyed")!=std::string::npos){destroyed=record;destroyedAt=recordIndex;}

        const auto orderRef=logField(record,"orderRef");
        const auto ref=logField(record,"ref");
        const auto orderRequestsSubmitted=logIntegerField(record,"orderRequestsSubmitted");
        if(record.find("[MONITOR]")!=std::string::npos&&record.find("activity=ORDER_API_REQUEST")!=std::string::npos
            &&logAccountIs(record,account)&&logFlag(record,"apiReturned")&&orderRef&&orderRequestsSubmitted&&*orderRequestsSubmitted>0)submittedRefs.insert(*orderRef);

        const auto errorNo=logIntegerField(record,"errorNo");
        const bool cabinetOrderState=record.find("status=ACCEPTED(")!=std::string::npos||record.find("status=QUEUING(")!=std::string::npos
            ||record.find("status=ALL_TRADED(")!=std::string::npos||record.find("status=CANCELED(")!=std::string::npos;
        if(record.find("[ORDER] notifyOrder account=")!=std::string::npos&&logHasAccount(record)&&ref&&errorNo&&*errorNo==0
            &&cabinetOrderState&&record.find("instrument=")!=std::string::npos)orderCallbacks[*ref]=record;
        if(record.find("[ORDER_RESULT]")!=std::string::npos&&logHasAccount(record)&&orderRef&&errorNo&&*errorNo==0
            &&record.find("instrument=")!=std::string::npos)orderResults[*orderRef]=record;
        if(record.find("[ORDER]")!=std::string::npos&&record.find("event=BATCH_CANCEL_RESULT")!=std::string::npos&&logHasAccount(record)&&orderRef
            &&record.find("instrument=")!=std::string::npos&&record.find("status=CANCELED")!=std::string::npos)batchCancelResults[*orderRef]=record;
        const auto tradeId=logIntegerField(record,"tradeId");
        const auto orderSysId=logIntegerField(record,"orderSysId");
        if(record.find("[TRADE] notifyTrade account=")!=std::string::npos&&logHasAccount(record)&&ref&&tradeId&&*tradeId>0&&orderSysId&&*orderSysId>0)tradeCallbacks[*ref]=record;

        const auto cancelRequestsSubmitted=logIntegerField(record,"cancelRequestsSubmitted");
        if(record.find("[MONITOR]")!=std::string::npos&&logAccountIs(record,account)
            &&(record.find("activity=CANCEL_API_REQUEST")!=std::string::npos||record.find("activity=BATCH_CANCEL_API_REQUEST")!=std::string::npos)
            &&logFlag(record,"apiReturned")&&cancelRequestsSubmitted&&*cancelRequestsSubmitted>0)cancelRequestSubmitted=true;
        const auto confirmedCancellations=logIntegerField(record,"confirmedCancellations");
        if(record.find("[MONITOR]")!=std::string::npos&&record.find("activity=CANCELLATION_CONFIRMED")!=std::string::npos
            &&logHasAccount(record)&&orderRef&&confirmedCancellations&&*confirmedCancellations>0)confirmedCancelRefs.insert(*orderRef);

        const auto orderCount=logIntegerField(record,"orderCount");
        const auto cancelCount=logIntegerField(record,"cancelCount");
        if(record.find("[MONITOR]")!=std::string::npos&&record.find("event=RISK_STATISTICS")!=std::string::npos&&logAccountIs(record,account)
            &&orderCount&&*orderCount>0&&cancelCount&&*cancelCount>0)riskStatistics=record;

        const auto failedCancelCallbacks=logIntegerField(record,"failedCancelCallbacks");
        const auto callbackValidationFailures=logIntegerField(record,"callbackValidationFailures");
        const auto unexpectedTradeVolume=logIntegerField(record,"unexpectedTradeVolume");
        const auto orderApiRequests=logIntegerField(record,"orderApiRequests");
        const auto uniqueAcceptedOrders=logIntegerField(record,"uniqueAcceptedOrders");
        const auto cancelApiRequests=logIntegerField(record,"cancelApiRequests");
        const auto cleanupOrderApiRequests=logIntegerField(record,"cleanupOrderApiRequests");
        const auto cleanupCancelApiRequests=logIntegerField(record,"cleanupCancelApiRequests");
        const auto unresolvedRefs=logField(record,"unresolvedRefs");
        const bool commonSafeStatistics=logHasAccount(record)&&logFlag(record,"cleanupRestored")&&logFlag(record,"streamStableThroughStop")
            &&logFlag(record,"callbackStreamQuiet")&&logFlag(record,"tradeVolumeConsistent")
            &&orderRequestsSubmitted&&*orderRequestsSubmitted>0&&cancelRequestsSubmitted&&*cancelRequestsSubmitted>0
            &&confirmedCancellations&&*confirmedCancellations>0&&failedCancelCallbacks&&*failedCancelCallbacks==0
            &&callbackValidationFailures&&*callbackValidationFailures==0&&unexpectedTradeVolume&&*unexpectedTradeVolume==0
            &&orderApiRequests&&*orderApiRequests==*orderRequestsSubmitted&&uniqueAcceptedOrders&&*uniqueAcceptedOrders==*orderRequestsSubmitted
            &&cancelApiRequests&&*cancelApiRequests==*cancelRequestsSubmitted&&*confirmedCancellations==*cancelRequestsSubmitted
            &&cleanupOrderApiRequests&&*cleanupOrderApiRequests==0&&cleanupCancelApiRequests&&*cleanupCancelApiRequests==0
            &&unresolvedRefs&&*unresolvedRefs=="none";
        const bool countStatistics=record.find("[INFO] [COUNT_RESULT] status=PASS")!=std::string::npos&&commonSafeStatistics;
        const auto batchSize=logIntegerField(record,"batchSize");
        const auto batchApiCalls=logIntegerField(record,"batchApiCalls");
        const auto batchApiCallsSubmitted=logIntegerField(record,"batchApiCallsSubmitted");
        const auto batchTargetOrders=logIntegerField(record,"batchTargetOrders");
        const auto batchTargetOrdersSubmitted=logIntegerField(record,"batchTargetOrdersSubmitted");
        const auto canceledOrders=logIntegerField(record,"canceledOrders");
        const auto ownedNetLong=logIntegerField(record,"ownedNetLong");
        const bool batchStatistics=record.find("[INFO] [BATCH_CANCEL]")!=std::string::npos&&record.find("event=BATCH_CANCEL_STATISTICS")!=std::string::npos
            &&commonSafeStatistics&&logFlag(record,"batchApiReturned")&&batchSize&&*batchSize==2&&batchApiCalls&&*batchApiCalls==1
            &&batchApiCallsSubmitted&&*batchApiCallsSubmitted==1&&batchTargetOrders&&*batchTargetOrders==2
            &&batchTargetOrdersSubmitted&&*batchTargetOrdersSubmitted==2&&orderRequestsSubmitted&&*orderRequestsSubmitted==2
            &&cancelRequestsSubmitted&&*cancelRequestsSubmitted==2&&confirmedCancellations&&*confirmedCancellations==2
            &&canceledOrders&&*canceledOrders==2&&logFlag(record,"noWorkingOrders")&&ownedNetLong&&*ownedNetLong==0&&logFlag(record,"positionRestored");
        if(countStatistics||batchStatistics)safeLiveFinal=record;

        const auto receivedErrors=logIntegerField(record,"receivedErrors");
        const auto lastErrorNo=logIntegerField(record,"lastErrorNo");
        const auto statisticsAccount=logField(record,"account");
        if(record.find("[INFO] [ERROR_MONITOR]")!=std::string::npos&&record.find("event=ORDER_ERROR_STATISTICS")!=std::string::npos&&statisticsAccount&&receivedErrors&&*receivedErrors>0
            &&lastErrorNo&&*lastErrorNo>0&&record.find("source=notifyOrder")!=std::string::npos
            &&logFlag(record,"noWorkingOrders")&&logFlag(record,"streamStableThroughStop")&&logFlag(record,"accountStateNormal")){errorFinal=record;finalErrorNo=*lastErrorNo;finalErrorAccount=*statisticsAccount;}
        if(record.find("event=ORDER_REJECTED source=notifyOrder")!=std::string::npos&&logHasAccount(record)&&ref&&errorNo&&*errorNo>0
            &&record.find("instrument=")!=std::string::npos)errorCallbacks[*ref]={*errorNo,record};
    }

    const bool lifecycleComplete=!containsExample&&apiStartedAt<tradeConnectedAt&&tradeConnectedAt<loginAt&&loginAt<initAt&&initAt<caughtUpAt&&caughtUpAt<destroyedAt;
    if(!lifecycleComplete)return;
    chooseLatest(scan.lifecycle,file,apiStarted,login,destroyed,modified);

    if(tradingArchiveId(testId)&&!manualActionRequired&&!safeLiveFinal.empty()){
        for(const auto& submittedRef:submittedRefs){
            const auto& value=submittedRef;
            std::string businessRecord;
            if(const auto found=tradeCallbacks.find(value);found!=tradeCallbacks.end())businessRecord=found->second;
            else if(const auto found=orderResults.find(value);found!=orderResults.end())businessRecord=found->second;
            else if(const auto found=orderCallbacks.find(value);found!=orderCallbacks.end())businessRecord=found->second;
            else if(const auto found=batchCancelResults.find(value);found!=batchCancelResults.end())businessRecord=found->second;
            if(!businessRecord.empty()){chooseLatest(scan.trade,file,businessRecord,modified);break;}
        }
        bool confirmedSubmittedCancellation=false;
        for(const auto& refValue:confirmedCancelRefs)if(submittedRefs.count(refValue)){confirmedSubmittedCancellation=true;break;}
        if(cancelRequestSubmitted&&confirmedSubmittedCancellation&&!riskStatistics.empty())chooseLatest(scan.monitoring,file,riskStatistics,modified);
    }

    if(errorArchiveId(testId)&&!manualActionRequired&&!errorFinal.empty()){
        for(const auto& callback:errorCallbacks){
            const auto callbackAccount=logField(callback.second.second,"account");
            if(submittedRefs.count(callback.first)&&callback.second.first==finalErrorNo&&callbackAccount&&*callbackAccount==finalErrorAccount){chooseLatest(scan.cabinetError,file,callback.second.second,modified);break;}
        }
    }
}

ArchivedLogScan scanArchivedLogs(const std::filesystem::path& archiveRoot,const std::filesystem::path& currentLog,const std::string& accountDirectory,const std::string& account,const std::string& businessDate){
    ArchivedLogScan scan;
    std::error_code ec;
    const auto currentAbsolute=std::filesystem::absolute(currentLog,ec).lexically_normal();
    ec.clear();
    std::filesystem::recursive_directory_iterator it(archiveRoot,std::filesystem::directory_options::skip_permission_denied,ec),end;
    while(!ec&&it!=end){
        const auto entry=*it;
        it.increment(ec);
        if(ec){ec.clear();continue;}
        const std::string testId=entry.path().parent_path().parent_path().filename().string();
        const std::string runStamp=entry.path().parent_path().parent_path().parent_path().filename().string();
        std::error_code typeError;
        if(entry.is_symlink(typeError)||typeError||!entry.is_regular_file(typeError)||typeError||entry.path().filename()!="test.log"
            ||entry.path().parent_path().filename()!=accountDirectory||!evidenceArchiveId(testId)||!validArchiveRunStamp(runStamp))continue;
        std::error_code absoluteError;
        if(std::filesystem::absolute(entry.path(),absoluteError).lexically_normal()==currentAbsolute)continue;

        std::error_code stateError;
        const auto sizeBefore=std::filesystem::file_size(entry.path(),stateError);
        if(stateError||sizeBefore>64ULL*1024ULL*1024ULL)continue;
        const auto modifiedBefore=std::filesystem::last_write_time(entry.path(),stateError);
        if(stateError)continue;
        std::ifstream input(entry.path());
        if(!input)continue;
        std::vector<std::string> lines;
        std::string line;
        while(std::getline(input,line))lines.push_back(std::move(line));
        const auto sizeAfter=std::filesystem::file_size(entry.path(),stateError);
        if(stateError)continue;
        const auto modifiedAfter=std::filesystem::last_write_time(entry.path(),stateError);
        if(stateError||sizeBefore!=sizeAfter||modifiedBefore!=modifiedAfter)continue;
        std::vector<std::string> session;
        bool inspectedFile=false;
        const auto inspectSession=[&](){
            if(session.empty())return;
            const auto sessionDate=archivedRecordDate(session.front());
            if(sessionDate&&*sessionDate==businessDate){
                inspectedFile=true;
                analyzeArchivedSession(session,entry.path(),testId,account,modifiedAfter,scan);
            }
        };
        for(const auto& record:lines){
            if(record.find("[SYSTEM] YD API started;")!=std::string::npos){
                inspectSession();
                session.clear();
            }
            if(!session.empty()||record.find("[SYSTEM] YD API started;")!=std::string::npos)session.push_back(record);
        }
        inspectSession();
        if(inspectedFile)++scan.filesScanned;
    }
    return scan;
}

std::string snapshotNumber(double value){
    if(!std::isfinite(value)||value==DBL_MAX||value==-DBL_MAX)return "N/A";
    std::ostringstream out;out<<std::fixed<<std::setprecision(10)<<value;
    auto text=out.str();while(text.size()>1&&text.back()=='0')text.pop_back();if(!text.empty()&&text.back()=='.')text.pop_back();
    return text;
}
const char* positionDirectionName(int value){return value==YD_PD_Long?"LONG":(value==YD_PD_Short?"SHORT":"UNKNOWN");}
const char* positionDateName(int value){return value==YD_PSD_Today?"TODAY":(value==YD_PSD_History?"HISTORY":"UNKNOWN");}
const char* hedgeFlagName(int value){
    switch(value){case YD_HF_Speculation:return "SPECULATION/NORMAL";case YD_HF_Arbitrage:return "ARBITRAGE";case YD_HF_Hedge:return "HEDGE/COVERED";case YD_HF_Internal:return "INTERNAL";default:return "UNKNOWN";}
}
const char* directionName(int value){return value==YD_D_Buy?"BUY":(value==YD_D_Sell?"SELL":"UNKNOWN");}
const char* offsetName(int value){
    switch(value){case YD_OF_Open:return "OPEN";case YD_OF_Close:return "CLOSE";case YD_OF_ForceClose:return "FORCE_CLOSE";case YD_OF_CloseToday:return "CLOSE_TODAY";case YD_OF_CloseYesterday:return "CLOSE_YESTERDAY";default:return "UNKNOWN";}
}
const char* orderStatusName(int value){
    switch(value){case YD_OS_Accepted:return "ACCEPTED";case YD_OS_Queuing:return "QUEUING";case YD_OS_Canceled:return "CANCELED";case YD_OS_AllTraded:return "ALL_TRADED";case YD_OS_Rejected:return "REJECTED";default:return "UNKNOWN";}
}

bool usablePrice(double value){return std::isfinite(value)&&value!=DBL_MAX&&value!=-DBL_MAX&&value>0;}
bool boundedLegalPrice(double requested,const YDInstrument* instrument,const YDMarketData& md,double& out){
    if(!instrument||!usablePrice(requested)||!usablePrice(instrument->Tick))return false;
    double minUnits=-DBL_MAX,maxUnits=DBL_MAX;
    if(usablePrice(md.LowerLimitPrice))minUnits=std::ceil(md.LowerLimitPrice/instrument->Tick-1e-9);
    if(usablePrice(md.UpperLimitPrice))maxUnits=std::floor(md.UpperLimitPrice/instrument->Tick+1e-9);
    if(minUnits>maxUnits)return false;
    const double units=std::max(minUnits,std::min(maxUnits,std::round(requested/instrument->Tick)));
    out=units*instrument->Tick;
    const double tolerance=instrument->Tick*1e-6;
    return usablePrice(out)&&(!usablePrice(md.LowerLimitPrice)||out>=md.LowerLimitPrice-tolerance)&&(!usablePrice(md.UpperLimitPrice)||out<=md.UpperLimitPrice+tolerance);
}
bool usableMarket(const YDInstrument* instrument,const YDMarketData& md,std::string& reason){
    if(!instrument||!usablePrice(instrument->Tick)){reason="invalid minimum price tick";return false;}
    if((md.MarketDataFlag&YD_MDF_PauseTrading)!=0){reason="market is paused";return false;}
    if(!usablePrice(md.BidPrice)||!usablePrice(md.AskPrice)){reason="bid/ask price unavailable";return false;}
    if(md.BidPrice>md.AskPrice+instrument->Tick*1e-6){reason="bid price is above ask price";return false;}
    return true;
}

std::string assignedIdText(long long value){return isAssignedYdId(value)?std::to_string(value):std::string("N/A");}
std::string orderSystemId(const YDOrder& order){return assignedIdText(preferredSystemOrderId(order.OrderSysID,order.LongOrderSysID));}
std::string tradeId(const YDTrade& trade){return assignedIdText(isAssignedYdId(trade.LongTradeID)?trade.LongTradeID:static_cast<long long>(trade.TradeID));}

struct LiveTicket {
    const char* phase="";
    int sequence=0;
    int orderRef=0;
    int direction=YD_D_Buy;
    int offset=YD_OF_Open;
    double orderPrice=0;
    int volume=0;
    int total=2;
};

struct LongPositionSnapshot {
    int today=0;
    int history=0;
    int other=0;
    int total()const{return today+history+other;}
};

bool separatesTodayPosition(const YDInstrument* instrument){
    return instrument&&instrument->m_pExchange&&instrument->m_pExchange->UseTodayPosition;
}

bool baselineLongPositionCanBePreserved(const LongPositionSnapshot& position,const YDInstrument* instrument,bool allowExistingTodayPosition){
    if(position.total()==0)return true;
    if(!separatesTodayPosition(instrument)||position.other!=0)return false;
    if(position.today==0&&position.history>0)return true;
    return allowExistingTodayPosition&&position.today>0;
}

bool queryLongSpeculationPosition(YDExtendedApi* api,const YDAccount* account,const YDInstrument* instrument,LongPositionSnapshot& out){
    if(!api||!account||!instrument)return false;
    YDExtendedPositionFilter filter{};
    filter.PositionDate=-1;filter.PositionDirection=YD_PD_Long;filter.HedgeFlag=YD_HF_Speculation;filter.pInstrument=instrument;filter.pAccount=account;
    struct Destroy{void operator()(YDQueryResult<YDExtendedPosition>* value)const{if(value)value->destroy();}};
    std::unique_ptr<YDQueryResult<YDExtendedPosition>,Destroy> positions(api->findExtendedPositions(&filter));
    if(!positions)return false;
    LongPositionSnapshot snapshot;
    for(int index=0;index<positions->getCount();++index){
        const YDExtendedPosition* position=positions->get(index);
        if(!position||position->getAccount()!=account||position->getInstrument()!=instrument||position->PositionDirection!=YD_PD_Long||position->HedgeFlag!=YD_HF_Speculation||position->Position<=0)continue;
        if(position->PositionDate==YD_PSD_Today)snapshot.today+=position->Position;
        else if(position->PositionDate==YD_PSD_History)snapshot.history+=position->Position;
        else snapshot.other+=position->Position;
    }
    out=snapshot;return true;
}

bool orderMatchesTicket(const LiveTicket& ticket,const YDOrder& order,double tick,std::string& reason){
    if(order.OrderRef!=ticket.orderRef){reason="OrderRef mismatch";return false;}
    if(order.Direction!=ticket.direction){reason="direction mismatch";return false;}
    if(order.OffsetFlag!=ticket.offset){reason="offset mismatch";return false;}
    if(order.OrderVolume!=ticket.volume){reason="order volume mismatch";return false;}
    if(!std::isfinite(order.Price)||std::fabs(order.Price-ticket.orderPrice)>std::max(1e-9,tick*1e-6)){reason="accepted order price mismatch";return false;}
    if(!hasAssignedSystemOrderId(order.OrderSysID,order.LongOrderSysID)){reason="system order ID unavailable";return false;}
    return true;
}

bool tradeMatchesTicket(const LiveTicket& ticket,const YDOrder& order,const YDTrade& trade,std::string& reason){
    if(trade.OrderRef!=ticket.orderRef){reason="trade OrderRef mismatch";return false;}
    if(trade.Direction!=ticket.direction){reason="trade direction mismatch";return false;}
    if(trade.OffsetFlag!=ticket.offset){reason="trade offset mismatch";return false;}
    if(trade.Volume!=ticket.volume){reason="trade volume mismatch";return false;}
    if(!usablePrice(trade.Price)){reason="trade price unavailable";return false;}
    if(!isAssignedYdId(trade.LongTradeID)&&!isAssignedYdId(trade.TradeID)){reason="trade ID unavailable";return false;}
    if(!compatibleAssignedSystemOrderIds(order.OrderSysID,order.LongOrderSysID,trade.OrderSysID,trade.LongOrderSysID)){reason="system order ID mismatch";return false;}
    return true;
}

void logFillResult(Logger& log,const std::string& account,const std::string& instrumentId,const LiveTicket& ticket,const YDOrder& order,const YDTrade& trade){
    std::ostringstream line;line<<ticket.phase<<" | seq="<<ticket.sequence<<'/'<<ticket.total<<" | account="<<account
        <<" | instrument="<<instrumentId
        <<" | direction="<<directionName(order.Direction)<<" | offset="<<offsetName(order.OffsetFlag)
        <<" | orderPrice="<<snapshotNumber(order.Price)<<" | tradePrice="<<snapshotNumber(trade.Price)
        <<" | volume="<<order.OrderVolume<<" | filled="<<order.TradeVolume<<'/'<<order.OrderVolume
        <<" | orderRef="<<order.OrderRef<<" | orderSysId="<<orderSystemId(order)<<" | tradeId="<<tradeId(trade)
        <<" | status="<<orderStatusName(order.OrderStatus)<<" | errorNo="<<order.ErrorNo<<" | resultTime="<<timestampText();
    log.info("ORDER_RESULT",line.str());
}

void logCancelResult(Logger& log,const std::string& account,const std::string& instrumentId,const LiveTicket& ticket,const YDOrder& order){
    std::ostringstream line;line<<ticket.phase<<" | seq="<<ticket.sequence<<'/'<<ticket.total<<" | account="<<account
        <<" | instrument="<<instrumentId<<" | direction="<<directionName(order.Direction)<<" | offset="<<offsetName(order.OffsetFlag)
        <<" | orderPrice="<<snapshotNumber(order.Price)<<" | volume="<<order.OrderVolume
        <<" | traded="<<order.TradeVolume<<'/'<<order.OrderVolume<<" | orderRef="<<ticket.orderRef
        <<" | orderSysId="<<orderSystemId(order)<<" | status="<<orderStatusName(order.OrderStatus)<<" | errorNo="<<order.ErrorNo
        <<" | resultTime="<<timestampText();
    log.info("ORDER_RESULT",line.str());
}

std::uint64_t counterDelta(std::uint64_t after,std::uint64_t before){return after>=before?after-before:0;}
OrderActivitySnapshot activityDelta(const OrderActivitySnapshot& after,const OrderActivitySnapshot& before){
    return {
        counterDelta(after.orderApiRequests,before.orderApiRequests),
        counterDelta(after.orderRequestsSubmitted,before.orderRequestsSubmitted),
        counterDelta(after.uniqueAcceptedOrders,before.uniqueAcceptedOrders),
        counterDelta(after.cancelApiRequests,before.cancelApiRequests),
        counterDelta(after.cancelRequestsSubmitted,before.cancelRequestsSubmitted),
        counterDelta(after.confirmedCancellations,before.confirmedCancellations),
        counterDelta(after.failedCancelCallbacks,before.failedCancelCallbacks),
        counterDelta(after.callbackValidationFailures,before.callbackValidationFailures)
    };
}

BatchCancelActivitySnapshot batchCancelActivityDelta(const BatchCancelActivitySnapshot& after,const BatchCancelActivitySnapshot& before){
    return {
        counterDelta(after.apiCalls,before.apiCalls),
        counterDelta(after.apiCallsSubmitted,before.apiCallsSubmitted),
        counterDelta(after.targetOrdersRequested,before.targetOrdersRequested),
        counterDelta(after.targetOrdersSubmitted,before.targetOrdersSubmitted)
    };
}

bool readySession(YdSession& s,TestResult& r,int t,const std::string& account){
    if(!s.start()){r.fail("API start");return false;}
    if(s.waitConnected(t))r.pass("TCP trade connected");else{r.fail("TCP trade connected","timeout");return false;}
    if(!s.waitLogin(t)){r.fail("account login","account="+account+" timeout");return false;}
    if(s.loginError()!=0){r.fail("account login","account="+account+" errorNo="+std::to_string(s.loginError()));return false;}
    r.pass("account login","account="+account+" loginTime="+timestampText());
    if(s.waitInit(t))r.pass("static data ready");else{r.fail("static data ready","notifyFinishInit timeout");return false;}
    if(s.waitCaughtUp(t))r.pass("history caught up");else{r.fail("history caught up","notifyCaughtUp timeout");return false;}
    return true;
}

bool readySessionObserved(YdSession& s,TestResult& r,int t,const std::string& account){
    if(!s.start()){r.fail("API start");return false;}
    if(!s.waitConnected(t)){r.fail("TCP trade connected","timeout");return false;}
    if(!s.waitLogin(t)){r.fail("account login","account="+account+" timeout");return false;}
    if(s.loginError()!=0){r.fail("account login","account="+account+" errorNo="+std::to_string(s.loginError()));return false;}
    if(!s.waitInit(t)){r.fail("static data ready","notifyFinishInit timeout");return false;}
    if(!s.waitCaughtUp(t)){r.fail("history caught up","notifyCaughtUp timeout");return false;}
    r.observe("trading system session ready","account="+account+" loginTime="+timestampText());
    return true;
}

bool marketFor(YdSession& s,const YDInstrument* i,int t,YDMarketData& md){return i&&s.subscribe(i)&&s.waitMarketData(i->InstrumentRef,t,md);}
double legalPrice(double p,double tick){return tick>0?std::round(p/tick)*tick:p;}
bool terminal(const YDOrder& o){return o.OrderStatus==YD_OS_Canceled||o.OrderStatus==YD_OS_AllTraded||o.OrderStatus==YD_OS_Rejected;}
bool cancelableOrTerminal(const YDOrder& o){return terminal(o)||(o.OrderStatus==YD_OS_Queuing&&hasAssignedSystemOrderId(o.OrderSysID,o.LongOrderSysID));}

int closeOffset(const YDInstrument* i){return separatesTodayPosition(i)?YD_OF_CloseToday:YD_OF_Close;}

int runEach(const RunOptions& o,const std::string& id,const std::function<void(const Account&,const Config&,Logger&,TestResult&)>& fn,bool stopAccountsOnFailure=false,bool systemMonitorPresentation=false){
    Env e=makeEnv(o);int rc=0;for(const auto& a:e.accounts){Logger log(accountLog(e,id,a));if(systemMonitorPresentation)log.info("SYSTEM","trading risk-control system started account="+a.username);else log.info("TEST",id+" account="+a.username);TestResult r(id,a.username,log);try{fn(a,e.cfg,log,r);}catch(const std::exception& ex){r.fail("unhandled exception",ex.what());}r.writeSummary(e.root/"summary.csv");const bool accountFailed=r.failed();rc=combine(rc,accountFailed?1:0);if(stopAccountsOnFailure&&accountFailed){log.error("ALERT","LIVE account sequence stopped after failure account="+a.username+"; no later accounts were started");break;}}std::cout<<"Output: "<<e.root.string()<<std::endl;return rc;
}
}

int runTest01Connect(const RunOptions& o){return runEach(o,"2.1_connect",[&](const Account&a,const Config&c,Logger&l,TestResult&r){YdSession s(o.ydConfig,a.username,a.password,l,false,monitorThresholds(c));readySession(s,r,timeout(c),a.username);});}

int runTest12MarketPosition(const RunOptions& o){return runEach(o,"1.2_market_position",[&](const Account&a,const Config&c,Logger&l,TestResult&r){
    l.info("SYSTEM","READ_ONLY snapshot test: no order or cancel API will be called");
    const int snapshotTimeout=c.getInt("Snapshot.TimeoutSeconds",120);
    YdSession s(o.ydConfig,a.username,a.password,l,true,monitorThresholds(c));if(!readySession(s,r,snapshotTimeout,a.username))return;
    auto* api=s.extendedApi();if(!api){r.fail("extended API available","required for read-only position query");return;}

    const YDAccount* ydAccount=api->getMyAccount();
    if(!ydAccount){r.fail("account identity","getMyAccount returned null");return;}
    const std::string accountId=ydAccount->AccountID[0]?std::string(ydAccount->AccountID):a.username;
    const std::string snapshotTime=timestampText();
    l.info("ACCOUNT","account="+accountId+" snapshotTime="+snapshotTime+" accountRef="+std::to_string(ydAccount->AccountRef)+" loginCount="+std::to_string(ydAccount->LoginCount)+" tradingRight="+std::to_string(ydAccount->TradingRight));
    r.pass("account identity","account="+accountId+" snapshotTime="+snapshotTime);

    bool marketAvailable=false;
    const std::string instrumentId=instID(o,c);
    const YDInstrument* instrument=s.instrument(instrumentId);
    if(!instrument){
        l.warn("MARKET","instrument not found: "+instrumentId);r.skip("market snapshot","instrument not found="+instrumentId);
    }else{
        YDMarketData md{};
        if(!s.subscribe(instrument)){
            l.warn("MARKET","subscribe returned false account="+accountId+" instrument="+instrumentId);
            r.skip("market snapshot","subscribe returned false instrument="+instrumentId);
        }else if(s.waitMarketData(instrument->InstrumentRef,snapshotTimeout,md)){
            std::ostringstream line;line<<"account="<<accountId<<" snapshotTime="<<timestampText()
                <<" instrument="<<instrumentId<<" tradingDay="<<md.TradingDay
                <<" marketTimeStamp="<<md.TimeStamp<<" paused="<<((md.MarketDataFlag&YD_MDF_PauseTrading)?"true":"false")
                <<" last="<<snapshotNumber(md.LastPrice)<<" bid="<<snapshotNumber(md.BidPrice)<<" bidVolume="<<md.BidVolume
                <<" ask="<<snapshotNumber(md.AskPrice)<<" askVolume="<<md.AskVolume
                <<" volume="<<md.Volume<<" openInterest="<<snapshotNumber(md.OpenInterest)
                <<" upperLimit="<<snapshotNumber(md.UpperLimitPrice)<<" lowerLimit="<<snapshotNumber(md.LowerLimitPrice);
            l.info("MARKET",line.str());r.pass("market snapshot","account="+accountId+" instrument="+instrumentId);marketAvailable=true;
        }else{
            l.warn("MARKET","market data unavailable account="+accountId+" instrument="+instrumentId);
            r.skip("market snapshot","subscribe/market-data timeout instrument="+instrumentId);
        }
    }

    YDExtendedPositionFilter filter{};filter.PositionDate=-1;filter.PositionDirection=-1;filter.HedgeFlag=-1;filter.pAccount=ydAccount;
    struct PositionQueryDestroy{void operator()(YDQueryResult<YDExtendedPosition>* value)const{if(value)value->destroy();}};
    std::unique_ptr<YDQueryResult<YDExtendedPosition>,PositionQueryDestroy> positions(api->findExtendedPositions(&filter));
    int nonZeroPositions=0,shownPositions=0;
    const int maxShown=std::max(1,c.getInt("Snapshot.MaxPositions",10));
    if(positions){
        const int count=positions->getCount();
        for(int index=0;index<count;++index){
            const YDExtendedPosition* position=positions->get(index);
            if(!position||position->Position<=0||!position->m_pAccountInstrumentInfo)continue;
            if(position->getAccount()!=ydAccount)continue;
            const YDInstrument* positionInstrument=position->getInstrument();if(!positionInstrument)continue;
            ++nonZeroPositions;
            if(shownPositions>=maxShown)continue;
            std::ostringstream line;line<<"account="<<accountId<<" snapshotTime="<<timestampText()
                <<" instrument="<<positionInstrument->InstrumentID
                <<" direction="<<positionDirectionName(position->PositionDirection)
                <<" date="<<positionDateName(position->PositionDate)<<" hedge="<<hedgeFlagName(position->HedgeFlag)
                <<" position="<<position->Position<<" openFrozen="<<position->OpenFrozen<<" closeFrozen="<<position->CloseFrozen
                <<" openPrice="<<snapshotNumber(position->getOpenPrice());
            l.info("POSITION",line.str());++shownPositions;
        }
        l.info("POSITION","account="+accountId+" snapshotTime="+timestampText()+" nonZeroPositions="+std::to_string(nonZeroPositions)+" displayed="+std::to_string(shownPositions));
        if(nonZeroPositions>0)r.pass("position snapshot","account="+accountId+" nonZeroPositions="+std::to_string(nonZeroPositions));
        else r.skip("position snapshot","account="+accountId+" has no non-zero position");
    }else{
        l.warn("POSITION","position query unavailable account="+accountId);r.skip("position snapshot","query unavailable");
    }

    r.pass("read-only safety","no order/cancel requests sent");
    if(marketAvailable||nonZeroPositions>0)r.pass("market or position evidence","account="+accountId+" market="+(marketAvailable?std::string("available"):std::string("unavailable"))+" nonZeroPositions="+std::to_string(nonZeroPositions));
    else r.fail("market or position evidence","no market snapshot and no non-zero position; use an active --instrument");
});}

enum class LiveWorkflowMode { BasicTrade, OrderCancelCount, BatchCancel, SellAg2610 };

int runLiveOrderWorkflow(const RunOptions& o,const std::string& testId,LiveWorkflowMode mode){
const bool batchMode=mode==LiveWorkflowMode::BatchCancel;
const bool sellMode=mode==LiveWorkflowMode::SellAg2610;
return runEach(o,testId,[&](const Account&a,const Config&c,Logger&l,TestResult&r){
    const bool countMode=mode==LiveWorkflowMode::OrderCancelCount;
    // This gate must remain before YdSession construction: without --live this test performs no order/cancel API call.
    if(!o.live){r.skip(sellMode?"live sell round trip":(countMode?"live order/cancel monitoring":(batchMode?"live batch cancellation":"live trading")),"rerun with --live in a broker-approved test environment");return;}

    const int requiredCount=sellMode?1:2;
    constexpr int orderVolume=1;
    const int sessionTimeout=std::max(1,c.getInt("Trade.SessionTimeoutSeconds",120));
    const int actionTimeout=std::max(1,c.getInt("Trade.ActionTimeoutSeconds",30));
    const int callbackQuietMilliseconds=std::max(1,c.getInt("Trade.CallbackQuietMilliseconds",2000));
    const int aggressiveTicks=std::max(0,c.getInt("Trade.AggressiveTicks",2));
    const int workingOffsetTicks=std::max(1,c.getInt(batchMode?"BatchCancel.WorkingOrderOffsetTicks":"Trade.WorkingOrderOffsetTicks",c.getInt("Trade.WorkingOrderOffsetTicks",50)));

    YdSession s(o.ydConfig,a.username,a.password,l,true,monitorThresholds(c),o.live);
    if(!(batchMode?readySessionObserved(s,r,sessionTimeout,a.username):readySession(s,r,sessionTimeout,a.username)))return;
    const std::string requestedInstrument=sellMode&&o.instrument.empty()?std::string("ag2610"):instID(o,c);
    const YDInstrument* instrument=s.instrument(requestedInstrument);
    if(!instrument){r.fail("instrument exists","instrument="+requestedInstrument);return;}
    const std::string instrumentId=instrument->InstrumentID;
    if(batchMode)r.observe("instrument available","instrument="+instrumentId);else r.pass("instrument exists","instrument="+instrumentId);

    const YDAccount* ydAccount=s.api()?s.api()->getMyAccount():nullptr;
    if(!ydAccount){r.fail("live trading preflight","getMyAccount returned null; no order sent");return;}
    const std::string accountId=ydAccount->AccountID[0]?std::string(ydAccount->AccountID):a.username;
    const YDAccountInstrumentInfo* accountInstrument=s.api()->getAccountInstrumentInfo(instrument);
    if(ydAccount->TradingRight!=YD_TR_Allow){r.fail("live trading preflight","account="+accountId+" tradingRight="+std::to_string(ydAccount->TradingRight)+"; no order sent");return;}
    if(!accountInstrument||accountInstrument->TradingRight!=YD_TR_Allow){
        r.fail("live trading preflight","account="+accountId+" instrument="+instrumentId+" instrumentTradingRight="+(accountInstrument?std::to_string(accountInstrument->TradingRight):std::string("N/A"))+"; no order sent");return;
    }
    if(!usablePrice(instrument->Tick)||orderVolume<instrument->MinLimitOrderVolume||orderVolume>instrument->MaxLimitOrderVolume){
        r.fail("live trading preflight","account="+accountId+" instrument="+instrumentId+" tick="+snapshotNumber(instrument->Tick)+" volume=1 allowedVolume="+std::to_string(instrument->MinLimitOrderVolume)+".."+std::to_string(instrument->MaxLimitOrderVolume)+"; no order sent");return;
    }
    LongPositionSnapshot baselinePosition;
    if(!queryLongSpeculationPosition(s.extendedApi(),ydAccount,instrument,baselinePosition)){
        r.fail("live trading preflight","cannot query baseline long position account="+accountId+" instrument="+instrumentId+"; no order sent");return;
    }
    l.info("POSITION_BASELINE","account="+accountId+" snapshotTime="+timestampText()+" instrument="+instrumentId+" direction=LONG hedge=SPECULATION today="+std::to_string(baselinePosition.today)+" history="+std::to_string(baselinePosition.history)+" other="+std::to_string(baselinePosition.other)+" total="+std::to_string(baselinePosition.total()));
    if(!baselineLongPositionCanBePreserved(baselinePosition,instrument,o.allowExistingTodayPosition)){
        r.fail("live trading preflight","account="+accountId+" instrument="+instrumentId
            +" existing LONG SPECULATION position cannot be isolated from this test"
            +" today="+std::to_string(baselinePosition.today)
            +" history="+std::to_string(baselinePosition.history)
            +" other="+std::to_string(baselinePosition.other)
            +" useTodayPosition="+(separatesTodayPosition(instrument)?std::string("true"):std::string("false"))
            +"; existing history is allowed when the exchange supports CLOSE_TODAY and baseline today/other are zero"
            +"; an intentional non-zero today baseline additionally requires --allow-existing-today-position; no order sent");return;
    }
    const bool historicalBaselineProtected=baselinePosition.history>0;
    const bool existingTodayBaselineAccepted=baselinePosition.today>0;
    if(historicalBaselineProtected){
        l.info("POSITION_PROTECTION","account="+accountId+" instrument="+instrumentId
            +" mode=HISTORICAL_BASELINE_PRESERVED baselineToday="+std::to_string(baselinePosition.today)+" baselineHistory="+std::to_string(baselinePosition.history)
            +" closeOffset=CLOSE_TODAY");
    }
    if(existingTodayBaselineAccepted){
        l.warn("POSITION_PROTECTION","account="+accountId+" instrument="+instrumentId
            +" mode=EXISTING_TODAY_QUANTITY_BASELINE baselineToday="+std::to_string(baselinePosition.today)
            +" baselineHistory="+std::to_string(baselinePosition.history)
            +" closeOffset=CLOSE_TODAY originalTodayLotIdentityNotGuaranteed=true");
    }

    if(!s.subscribe(instrument)){r.fail("live trading preflight","market subscription failed account="+accountId+" instrument="+instrumentId+"; no order sent");return;}
    std::uint64_t marketVersion=0;
    YDMarketData initialMarket{};
    if(!s.waitNextMarketData(instrument->InstrumentRef,marketVersion,sessionTimeout,initialMarket)){r.fail("live trading preflight","market data timeout account="+accountId+" instrument="+instrumentId+"; no order sent");return;}
    std::string marketReason;
    if(!usableMarket(instrument,initialMarket,marketReason)){r.fail("live trading preflight","account="+accountId+" instrument="+instrumentId+" reason="+marketReason+"; no order sent");return;}
    if(batchMode)r.observe("batch cancellation preflight","account="+accountId+" instrument="+instrumentId+" bid="+snapshotNumber(initialMarket.BidPrice)+" ask="+snapshotNumber(initialMarket.AskPrice)+" volumePerOrder=1");
    else r.pass("live trading preflight","account="+accountId+" instrument="+instrumentId+" bid="+snapshotNumber(initialMarket.BidPrice)+" ask="+snapshotNumber(initialMarket.AskPrice)+" volumePerOrder=1");
    const OrderActivitySnapshot activityStart=s.orderActivity();
    const BatchCancelActivitySnapshot batchActivityStart=s.batchCancelActivity();
    std::vector<LiveTicket> tickets;
    tickets.reserve(16);
    int openCompleted=0,cancelCompleted=0,closeCompleted=0,batchCancelCompleted=0;
    bool sellSetupPositionVerified=false;
    bool batchCallSubmitted=false;
    std::string stepFailure;
    auto noteFailure=[&](const std::string& text){if(stepFailure.empty())stepFailure=text;l.error("ORDER_RESULT",text);};
    auto currentMarket=[&](YDMarketData& md,std::string& reason){
        if(!s.waitConnected(actionTimeout)){reason="trade connection unavailable";return false;}
        if(!s.waitCaughtUp(actionTimeout)){reason="trade session not caught up after reconnect";return false;}
        if(!s.waitNextMarketData(instrument->InstrumentRef,marketVersion,actionTimeout,md)){reason="no newer market-data callback before timeout";return false;}
        return usableMarket(instrument,md,reason);
    };
    auto sendFill=[&](const char* phase,int sequence,int direction,int offset){
        YDMarketData md{};std::string reason;
        if(!currentMarket(md,reason)){noteFailure(std::string(phase)+" #"+std::to_string(sequence)+" market unavailable: "+reason);return false;}
        const double requested=direction==YD_D_Buy?md.AskPrice+aggressiveTicks*instrument->Tick:md.BidPrice-aggressiveTicks*instrument->Tick;
        double price=0;
        if(!boundedLegalPrice(requested,instrument,md,price)){noteFailure(std::string(phase)+" #"+std::to_string(sequence)+" cannot create legal limit price");return false;}
        LiveTicket ticket{phase,sequence,0,direction,offset,price,orderVolume,requiredCount};
        tickets.push_back(ticket);
        ticket.orderRef=s.sendLimitOrder(instrument,direction,offset,price,orderVolume);
        if(ticket.orderRef<0){tickets.pop_back();noteFailure(std::string(phase)+" #"+std::to_string(sequence)+" insertOrder returned false");return false;}
        tickets.back().orderRef=ticket.orderRef;

        YDOrder finalOrder{};
        if(!s.waitOrder(ticket.orderRef,actionTimeout,terminal,finalOrder)){
            noteFailure(std::string(phase)+" #"+std::to_string(sequence)+" terminal order callback timeout ref="+std::to_string(ticket.orderRef));return false;
        }
        if(finalOrder.ErrorNo!=0||finalOrder.OrderStatus!=YD_OS_AllTraded||finalOrder.TradeVolume!=orderVolume||finalOrder.OrderVolume!=orderVolume){
            noteFailure(std::string(phase)+" #"+std::to_string(sequence)+" unexpected final state ref="+std::to_string(ticket.orderRef)+" status="+orderStatusName(finalOrder.OrderStatus)+" errorNo="+std::to_string(finalOrder.ErrorNo)+" traded="+std::to_string(finalOrder.TradeVolume)+"/"+std::to_string(finalOrder.OrderVolume));return false;
        }
        std::string mismatch;
        if(!orderMatchesTicket(ticket,finalOrder,instrument->Tick,mismatch)){
            noteFailure(std::string(phase)+" #"+std::to_string(sequence)+" order callback mismatch ref="+std::to_string(ticket.orderRef)+" reason="+mismatch);return false;
        }
        YDTrade trade{};
        if(!s.waitTrade(ticket.orderRef,actionTimeout,trade)){
            noteFailure(std::string(phase)+" #"+std::to_string(sequence)+" trade callback timeout ref="+std::to_string(ticket.orderRef));return false;
        }
        if(!tradeMatchesTicket(ticket,finalOrder,trade,mismatch)){
            noteFailure(std::string(phase)+" #"+std::to_string(sequence)+" trade callback mismatch ref="+std::to_string(ticket.orderRef)+" reason="+mismatch);return false;
        }
        logFillResult(l,accountId,instrumentId,ticket,finalOrder,trade);
        return true;
    };
    auto sendAndCancel=[&](int sequence){
        YDMarketData md{};std::string reason;
        if(!currentMarket(md,reason)){noteFailure("CANCEL #"+std::to_string(sequence)+" market unavailable: "+reason);return false;}
        double price=0;
        const double requested=md.BidPrice-(workingOffsetTicks+sequence-1)*instrument->Tick;
        if(!boundedLegalPrice(requested,instrument,md,price)){noteFailure("CANCEL #"+std::to_string(sequence)+" cannot create legal passive price");return false;}
        const double tolerance=instrument->Tick*1e-6;
        if(price>=md.BidPrice-tolerance||price>=md.AskPrice-tolerance){
            noteFailure("CANCEL #"+std::to_string(sequence)+" passive price is not below current bid/ask; no order sent");return false;
        }
        LiveTicket ticket{"CANCEL",sequence,0,YD_D_Buy,YD_OF_Open,price,orderVolume,requiredCount};
        tickets.push_back(ticket);
        ticket.orderRef=s.sendLimitOrder(instrument,ticket.direction,ticket.offset,ticket.orderPrice,ticket.volume);
        if(ticket.orderRef<0){tickets.pop_back();noteFailure("CANCEL #"+std::to_string(sequence)+" insertOrder returned false");return false;}
        tickets.back().orderRef=ticket.orderRef;

        YDOrder working{};
        if(!s.waitOrder(ticket.orderRef,actionTimeout,[](const YDOrder& order){return cancelableOrTerminal(order);},working)){
            noteFailure("CANCEL #"+std::to_string(sequence)+" working/terminal order callback timeout ref="+std::to_string(ticket.orderRef));return false;
        }
        if(working.ErrorNo!=0||working.OrderStatus!=YD_OS_Queuing){
            noteFailure("CANCEL #"+std::to_string(sequence)+" did not become cancelable ref="+std::to_string(ticket.orderRef)+" status="+orderStatusName(working.OrderStatus)+" errorNo="+std::to_string(working.ErrorNo)+" traded="+std::to_string(working.TradeVolume));return false;
        }
        if(!hasAssignedSystemOrderId(working.OrderSysID,working.LongOrderSysID)){
            noteFailure("CANCEL #"+std::to_string(sequence)+" queued order has no system order ID ref="+std::to_string(ticket.orderRef));return false;
        }
        const bool cancelSubmitted=s.cancelOrder(instrument,working);
        if(!cancelSubmitted){noteFailure("CANCEL #"+std::to_string(sequence)+" cancelOrder returned false ref="+std::to_string(ticket.orderRef));return false;}
        YDOrder finalOrder{};
        const int cancelResult=s.waitCancelTerminal(ticket.orderRef,actionTimeout,finalOrder);
        if(cancelResult<0){noteFailure("CANCEL #"+std::to_string(sequence)+" terminal cancel callback timeout ref="+std::to_string(ticket.orderRef));return false;}
        if(cancelResult>0){noteFailure("CANCEL #"+std::to_string(sequence)+" failed-cancel callback ref="+std::to_string(ticket.orderRef)+" errorNo="+std::to_string(cancelResult));return false;}
        if(finalOrder.ErrorNo!=0||finalOrder.OrderStatus!=YD_OS_Canceled||finalOrder.TradeVolume!=0||finalOrder.OrderVolume!=ticket.volume){
            noteFailure("CANCEL #"+std::to_string(sequence)+" unexpected final state ref="+std::to_string(ticket.orderRef)+" status="+orderStatusName(finalOrder.OrderStatus)+" errorNo="+std::to_string(finalOrder.ErrorNo)+" traded="+std::to_string(finalOrder.TradeVolume)+"/"+std::to_string(finalOrder.OrderVolume));return false;
        }
        std::string mismatch;
        if(!orderMatchesTicket(ticket,finalOrder,instrument->Tick,mismatch)){
            noteFailure("CANCEL #"+std::to_string(sequence)+" order callback mismatch ref="+std::to_string(ticket.orderRef)+" reason="+mismatch);return false;
        }
        logCancelResult(l,accountId,instrumentId,ticket,finalOrder);
        return true;
    };
    std::vector<std::pair<const YDInstrument*,YDOrder>> batchWorkingOrders;
    batchWorkingOrders.reserve(requiredCount);
    auto prepareBatchCancelOrder=[&](int sequence){
        YDMarketData md{};std::string reason;
        if(!currentMarket(md,reason)){noteFailure("BATCH_CANCEL #"+std::to_string(sequence)+" market unavailable: "+reason);return false;}
        double price=0;
        const double requested=md.BidPrice-(workingOffsetTicks+sequence-1)*instrument->Tick;
        if(!boundedLegalPrice(requested,instrument,md,price)){noteFailure("BATCH_CANCEL #"+std::to_string(sequence)+" cannot create legal passive price");return false;}
        const double tolerance=instrument->Tick*1e-6;
        if(price>=md.BidPrice-tolerance||price>=md.AskPrice-tolerance){
            noteFailure("BATCH_CANCEL #"+std::to_string(sequence)+" passive price is not below current bid/ask; no order sent");return false;
        }
        LiveTicket ticket{"BATCH_CANCEL",sequence,0,YD_D_Buy,YD_OF_Open,price,orderVolume,requiredCount};
        tickets.push_back(ticket);
        ticket.orderRef=s.sendLimitOrder(instrument,ticket.direction,ticket.offset,ticket.orderPrice,ticket.volume);
        if(ticket.orderRef<0){tickets.pop_back();noteFailure("BATCH_CANCEL #"+std::to_string(sequence)+" insertOrder returned false");return false;}
        tickets.back().orderRef=ticket.orderRef;

        YDOrder working{};
        if(!s.waitOrder(ticket.orderRef,actionTimeout,[](const YDOrder& order){return cancelableOrTerminal(order);},working)){
            noteFailure("BATCH_CANCEL #"+std::to_string(sequence)+" working/terminal callback timeout ref="+std::to_string(ticket.orderRef));return false;
        }
        if(working.ErrorNo!=0||working.OrderStatus!=YD_OS_Queuing||working.TradeVolume!=0){
            noteFailure("BATCH_CANCEL #"+std::to_string(sequence)+" did not remain wholly unfilled and cancelable ref="+std::to_string(ticket.orderRef)
                +" status="+orderStatusName(working.OrderStatus)+" errorNo="+std::to_string(working.ErrorNo)
                +" traded="+std::to_string(working.TradeVolume)+"/"+std::to_string(working.OrderVolume));return false;
        }
        if(!hasAssignedSystemOrderId(working.OrderSysID,working.LongOrderSysID)){
            noteFailure("BATCH_CANCEL #"+std::to_string(sequence)+" queued order has no system order ID ref="+std::to_string(ticket.orderRef));return false;
        }
        std::string mismatch;
        if(!orderMatchesTicket(ticket,working,instrument->Tick,mismatch)){
            noteFailure("BATCH_CANCEL #"+std::to_string(sequence)+" order callback mismatch ref="+std::to_string(ticket.orderRef)+" reason="+mismatch);return false;
        }
        batchWorkingOrders.push_back({instrument,working});
        l.info("ORDER","account="+accountId+" event=BATCH_CANCEL_TARGET_READY sequence="+std::to_string(sequence)+"/2 instrument="+instrumentId
            +" orderRef="+std::to_string(ticket.orderRef)+" orderSysId="+orderSystemId(working)+" status=QUEUING traded=0/"+std::to_string(ticket.volume));
        return true;
    };

    bool executionException=false;
    std::string executionExceptionDetail;
    try{
        if(batchMode){
            stepFailure.clear();
            for(int sequence=1;sequence<=requiredCount;++sequence){if(!prepareBatchCancelOrder(sequence))break;}
            if(batchWorkingOrders.size()==requiredCount){
                const OrderStreamSnapshot readySnapshot=s.orderStreamSnapshot();
                for(std::size_t n=0;n<batchWorkingOrders.size();++n){
                    const int orderRef=batchWorkingOrders[n].second.OrderRef;
                    const auto latest=readySnapshot.orders.find(orderRef);
                    if(latest==readySnapshot.orders.end()||latest->second.ErrorNo!=0||latest->second.OrderStatus!=YD_OS_Queuing||latest->second.TradeVolume!=0){
                        noteFailure("BATCH_CANCEL target changed before batch request ref="+std::to_string(orderRef));
                        batchWorkingOrders.clear();break;
                    }
                    batchWorkingOrders[n].second=latest->second;
                }
            }
            if(batchWorkingOrders.size()==requiredCount){
                batchCallSubmitted=s.cancelMulti(batchWorkingOrders);
                if(!batchCallSubmitted)noteFailure("cancelMultiOrders returned false for targetCount=2");
            }
            if(batchCallSubmitted){
                for(std::size_t n=0;n<batchWorkingOrders.size();++n){
                    const LiveTicket& ticket=tickets[n];
                    YDOrder finalOrder{};
                    const int cancelResult=s.waitCancelTerminal(ticket.orderRef,actionTimeout,finalOrder);
                    if(cancelResult<0){noteFailure("BATCH_CANCEL terminal callback timeout ref="+std::to_string(ticket.orderRef));continue;}
                    if(cancelResult>0){noteFailure("BATCH_CANCEL failed-cancel callback ref="+std::to_string(ticket.orderRef)+" errorNo="+std::to_string(cancelResult));continue;}
                    std::string mismatch;
                    if(finalOrder.ErrorNo!=0||finalOrder.OrderStatus!=YD_OS_Canceled||finalOrder.TradeVolume!=0||finalOrder.OrderVolume!=ticket.volume){
                        noteFailure("BATCH_CANCEL unexpected final state ref="+std::to_string(ticket.orderRef)+" status="+orderStatusName(finalOrder.OrderStatus)
                            +" errorNo="+std::to_string(finalOrder.ErrorNo)+" traded="+std::to_string(finalOrder.TradeVolume)+"/"+std::to_string(finalOrder.OrderVolume));continue;
                    }
                    if(!orderMatchesTicket(ticket,finalOrder,instrument->Tick,mismatch)){
                        noteFailure("BATCH_CANCEL final callback mismatch ref="+std::to_string(ticket.orderRef)+" reason="+mismatch);continue;
                    }
                    ++batchCancelCompleted;
                    l.info("ORDER","account="+accountId+" event=BATCH_CANCEL_RESULT sequence="+std::to_string(n+1)+"/2 instrument="+instrumentId
                        +" orderRef="+std::to_string(ticket.orderRef)+" orderSysId="+orderSystemId(finalOrder)+" status=CANCELED traded=0/"+std::to_string(ticket.volume));
                }
            }
        }else if(countMode){
            stepFailure.clear();
            for(int sequence=1;sequence<=requiredCount;++sequence){if(!sendAndCancel(sequence))break;++cancelCompleted;}
            if(cancelCompleted==requiredCount)r.pass("monitored order cancellations","account="+accountId+" instrument="+instrumentId+" completed=2/2");
            else r.fail("monitored order cancellations","account="+accountId+" instrument="+instrumentId+" completed="+std::to_string(cancelCompleted)+"/2 reason="+stepFailure);
        }else if(sellMode){
            stepFailure.clear();
            if(sendFill("SETUP_OPEN",1,YD_D_Buy,YD_OF_Open)){
                openCompleted=1;
                LongPositionSnapshot setupPosition;
                bool setupPositionAvailable=false;
                const auto positionDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(actionTimeout);
                // Extended position queries expose no update callback/generation. Poll the
                // exact expected snapshot for a bounded interval after the validated fill.
                do{
                    setupPositionAvailable=queryLongSpeculationPosition(s.extendedApi(),ydAccount,instrument,setupPosition);
                    sellSetupPositionVerified=setupPositionAvailable&&(separatesTodayPosition(instrument)
                        ? setupPosition.today==baselinePosition.today+1&&setupPosition.history==baselinePosition.history&&setupPosition.other==baselinePosition.other
                        : setupPosition.total()==baselinePosition.total()+1);
                    if(sellSetupPositionVerified||std::chrono::steady_clock::now()>=positionDeadline)break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }while(true);
                if(setupPositionAvailable){
                    l.info("POSITION_VERIFY","account="+accountId+" snapshotTime="+timestampText()+" instrument="+instrumentId
                        +" phase=AFTER_SELL_SETUP baselineToday="+std::to_string(baselinePosition.today)+" currentToday="+std::to_string(setupPosition.today)
                        +" baselineHistory="+std::to_string(baselinePosition.history)+" currentHistory="+std::to_string(setupPosition.history)
                        +" baselineOther="+std::to_string(baselinePosition.other)+" currentOther="+std::to_string(setupPosition.other)
                        +" baselineTotal="+std::to_string(baselinePosition.total())+" currentTotal="+std::to_string(setupPosition.total())
                        +" expectedIncrease=1 positionIncreaseVerified="+(sellSetupPositionVerified?std::string("true"):std::string("false")));
                }
                if(sellSetupPositionVerified)r.pass("sell test setup","account="+accountId+" instrument="+instrumentId+" buyOpenFills=1/1 positionIncreaseVerified=true");
                else r.fail("sell test setup","account="+accountId+" instrument="+instrumentId+" buyOpenFills=1/1 positionIncreaseVerified=false; normal sell is blocked and cleanup will restore the owned exposure");
            }else{
                r.fail("sell test setup","account="+accountId+" instrument="+instrumentId+" buyOpenFills=0/1 reason="+stepFailure);
            }
            if(sellSetupPositionVerified){
                stepFailure.clear();
                const int offset=closeOffset(instrument);
                if(sendFill("SELL",1,YD_D_Sell,offset)){
                    closeCompleted=1;
                    r.pass("sell order","account="+accountId+" instrument="+instrumentId+" sellFills=1/1 offset="+offsetName(offset));
                }else{
                    r.fail("sell order","account="+accountId+" instrument="+instrumentId+" sellFills=0/1 offset="+offsetName(offset)+" reason="+stepFailure);
                }
            }else r.skip("sell order","blocked because the test-owned setup position was not verified; cleanup will settle orders and restore any owned exposure");
        }else{
            stepFailure.clear();
            for(int sequence=1;sequence<=requiredCount;++sequence){if(!sendFill("OPEN",sequence,YD_D_Buy,YD_OF_Open))break;++openCompleted;}
            if(openCompleted==requiredCount)r.pass("opening trades","account="+accountId+" instrument="+instrumentId+" completed=2/2");
            else r.fail("opening trades","account="+accountId+" instrument="+instrumentId+" completed="+std::to_string(openCompleted)+"/2 reason="+stepFailure);

            if(openCompleted==requiredCount){
                stepFailure.clear();
                for(int sequence=1;sequence<=requiredCount;++sequence){if(!sendAndCancel(sequence))break;++cancelCompleted;}
                if(cancelCompleted==requiredCount)r.pass("order cancellations","account="+accountId+" instrument="+instrumentId+" completed=2/2 cancelRequests=2");
                else r.fail("order cancellations","account="+accountId+" instrument="+instrumentId+" completed="+std::to_string(cancelCompleted)+"/2 reason="+stepFailure);
            }else r.skip("order cancellations","blocked by incomplete opening stage");

            if(openCompleted==requiredCount&&cancelCompleted==requiredCount){
                stepFailure.clear();
                const int offset=closeOffset(instrument);
                for(int sequence=1;sequence<=requiredCount;++sequence){if(!sendFill("CLOSE",sequence,YD_D_Sell,offset))break;++closeCompleted;}
                if(closeCompleted==requiredCount)r.pass("closing trades","account="+accountId+" instrument="+instrumentId+" completed=2/2 offset="+offsetName(offset));
                else r.fail("closing trades","account="+accountId+" instrument="+instrumentId+" completed="+std::to_string(closeCompleted)+"/2 reason="+stepFailure);
            }else r.skip("closing trades","blocked by incomplete opening/cancellation stage; cleanup will flatten newly opened exposure");
        }
    }catch(const std::exception& ex){
        executionException=true;executionExceptionDetail=ex.what();l.error("ALERT","exception after live test started; entering cleanup account="+accountId+" reason="+executionExceptionDetail);
    }catch(...){
        executionException=true;executionExceptionDetail="unknown exception";l.error("ALERT","unknown exception after live test started; entering cleanup account="+accountId);
    }
    const OrderActivitySnapshot activityBeforeCleanup=s.orderActivity();
    const OrderActivitySnapshot measuredActivity=activityDelta(activityBeforeCleanup,activityStart);
    const BatchCancelActivitySnapshot batchActivityBeforeCleanup=s.batchCancelActivity();
    const BatchCancelActivitySnapshot measuredBatchActivity=batchCancelActivityDelta(batchActivityBeforeCleanup,batchActivityStart);

    // From the first successful insert onward, all exits converge here. Settle working orders first,
    // then derive this test's net exposure from final order and validated trade callbacks and flatten it.
    std::unordered_map<int,YDOrder> finalOrders;
    finalOrders.reserve(16);
    std::unordered_map<int,std::int64_t> observedTradeVolumes;
    observedTradeVolumes.reserve(16);
    std::vector<int> unresolvedRefs;
    unresolvedRefs.reserve(16);
    auto settleAll=[&](){
        finalOrders.clear();unresolvedRefs.clear();
        for(const LiveTicket& ticket:tickets){
            if(!s.waitConnected(actionTimeout)||!s.waitCaughtUp(actionTimeout)){
                unresolvedRefs.push_back(ticket.orderRef);l.error("CLEANUP","trade session did not recover before cleanup ref="+std::to_string(ticket.orderRef));continue;
            }
            YDOrder state{};
            if(!s.waitOrder(ticket.orderRef,actionTimeout,[](const YDOrder& order){return cancelableOrTerminal(order);},state)){
                unresolvedRefs.push_back(ticket.orderRef);l.error("CLEANUP","no cancelable/terminal state ref="+std::to_string(ticket.orderRef));continue;
            }
            if(!terminal(state)){
                l.warn("CLEANUP","canceling outstanding order account="+accountId+" instrument="+instrumentId+" ref="+std::to_string(ticket.orderRef)+" phase="+ticket.phase);
                if(!hasAssignedSystemOrderId(state.OrderSysID,state.LongOrderSysID)){
                    unresolvedRefs.push_back(ticket.orderRef);l.error("CLEANUP","queued order has no system order ID ref="+std::to_string(ticket.orderRef));continue;
                }
                const bool cancelSubmitted=s.cancelOrder(instrument,state);
                if(!cancelSubmitted){
                    unresolvedRefs.push_back(ticket.orderRef);l.error("CLEANUP","cancelOrder returned false ref="+std::to_string(ticket.orderRef));continue;
                }
                YDOrder terminalOrder{};
                const int cancelResult=s.waitCancelTerminal(ticket.orderRef,actionTimeout,terminalOrder);
                if(cancelResult!=0){
                    unresolvedRefs.push_back(ticket.orderRef);l.error("CLEANUP","cancel did not reach terminal order state ref="+std::to_string(ticket.orderRef)+" cancelSubmitted="+(cancelSubmitted?std::string("true"):std::string("false"))+" result="+std::to_string(cancelResult));continue;
                }
                state=terminalOrder;
            }
            finalOrders[ticket.orderRef]=state;
        }
        const OrderStreamSnapshot current=s.orderStreamSnapshot();
        finalOrders.clear();unresolvedRefs.clear();observedTradeVolumes.clear();
        for(const LiveTicket& ticket:tickets){
            const auto order=current.orders.find(ticket.orderRef);
            if(order==current.orders.end()){unresolvedRefs.push_back(ticket.orderRef);continue;}
            finalOrders[ticket.orderRef]=order->second;
            if(!terminal(order->second))unresolvedRefs.push_back(ticket.orderRef);
            const auto trades=current.tradeVolumeByOrderRef.find(ticket.orderRef);
            const std::int64_t callbackTradeVolume=trades==current.tradeVolumeByOrderRef.end()?0:trades->second;
            observedTradeVolumes[ticket.orderRef]=std::max<std::int64_t>(std::max(0,order->second.TradeVolume),std::max<std::int64_t>(0,callbackTradeVolume));
        }
        return unresolvedRefs.empty();
    };
    auto calculateNet=[&](bool& known){
        known=true;int net=0;
        for(const LiveTicket& ticket:tickets){
            const auto found=finalOrders.find(ticket.orderRef);const auto observed=observedTradeVolumes.find(ticket.orderRef);
            if(found==finalOrders.end()||observed==observedTradeVolumes.end()||found->second.TradeVolume<0||observed->second<0||observed->second>ticket.volume){known=false;continue;}
            if(ticket.direction==YD_D_Buy&&ticket.offset==YD_OF_Open)net+=static_cast<int>(observed->second);
            else if(ticket.direction==YD_D_Sell&&ticket.offset!=YD_OF_Open)net-=static_cast<int>(observed->second);
        }
        return net;
    };

    int cleanupSequence=0;
    bool allSettled=false;
    bool netKnown=false;
    int netTestPosition=0;
    auto restoreOwnedExposure=[&](){
        allSettled=settleAll();netTestPosition=calculateNet(netKnown);
        int noProgressAttempts=0;
        while(allSettled&&netKnown&&netTestPosition>0&&cleanupSequence<8){
            const int before=netTestPosition;
            YDMarketData md{};std::string reason;double price=0;
            if(!currentMarket(md,reason)){
                l.error("CLEANUP","cannot price residual close account="+accountId+" instrument="+instrumentId+" reason="+reason);break;
            }
            const double freshCleanupRequested=usablePrice(md.LowerLimitPrice)?md.LowerLimitPrice:md.BidPrice-(aggressiveTicks+cleanupSequence*10)*instrument->Tick;
            if(!boundedLegalPrice(freshCleanupRequested,instrument,md,price)){
                l.error("CLEANUP","cannot create legal residual close price account="+accountId+" instrument="+instrumentId);break;
            }
            LiveTicket cleanup{"CLEANUP_CLOSE",++cleanupSequence,0,YD_D_Sell,closeOffset(instrument),price,orderVolume};
            tickets.push_back(cleanup);
            cleanup.orderRef=s.sendLimitOrder(instrument,cleanup.direction,cleanup.offset,cleanup.orderPrice,cleanup.volume);
            if(cleanup.orderRef<0){tickets.pop_back();l.error("CLEANUP","residual close insertOrder returned false account="+accountId+" instrument="+instrumentId);break;}
            tickets.back().orderRef=cleanup.orderRef;
            YDOrder cleanupOrder{};
            if(s.waitOrder(cleanup.orderRef,actionTimeout,terminal,cleanupOrder)&&cleanupOrder.OrderStatus==YD_OS_AllTraded){
                YDTrade cleanupTrade{};
                if(s.waitTrade(cleanup.orderRef,actionTimeout,cleanupTrade)){
                    std::ostringstream line;line<<"residual position closed | account="<<accountId<<" | instrument="<<instrumentId
                        <<" | orderRef="<<cleanup.orderRef<<" | orderPrice="<<snapshotNumber(cleanup.orderPrice)
                        <<" | tradePrice="<<snapshotNumber(cleanupTrade.Price)<<" | volume="<<cleanupOrder.TradeVolume
                        <<" | status="<<orderStatusName(cleanupOrder.OrderStatus)<<" | cleanupTime="<<timestampText();l.warn("CLEANUP",line.str());
                }
            }
            allSettled=settleAll();netTestPosition=calculateNet(netKnown);
            if(!netKnown)break;
            if(netTestPosition<before)noProgressAttempts=0;
            else if(++noProgressAttempts>=2)break;
        }
        allSettled=settleAll();netTestPosition=calculateNet(netKnown);
    };

    bool measurementTradeKnown=false;
    bool tradeVolumeConsistent=false;
    std::int64_t unexpectedTradeVolume=0;
    auto analyzeFinalSnapshot=[&](const OrderStreamSnapshot& snapshot){
        finalOrders.clear();unresolvedRefs.clear();observedTradeVolumes.clear();allSettled=true;measurementTradeKnown=true;tradeVolumeConsistent=true;unexpectedTradeVolume=0;
        for(const LiveTicket& ticket:tickets){
            const auto order=snapshot.orders.find(ticket.orderRef);
            if(order==snapshot.orders.end()){
                allSettled=false;measurementTradeKnown=false;tradeVolumeConsistent=false;unresolvedRefs.push_back(ticket.orderRef);continue;
            }
            finalOrders[ticket.orderRef]=order->second;
            if(!terminal(order->second)){allSettled=false;unresolvedRefs.push_back(ticket.orderRef);}
            const auto trades=snapshot.tradeVolumeByOrderRef.find(ticket.orderRef);
            const std::int64_t callbackTradeVolume=trades==snapshot.tradeVolumeByOrderRef.end()?0:trades->second;
            if(order->second.TradeVolume<0||order->second.TradeVolume>ticket.volume||callbackTradeVolume<0||callbackTradeVolume>ticket.volume){
                measurementTradeKnown=false;tradeVolumeConsistent=false;
            }else if(callbackTradeVolume!=order->second.TradeVolume){
                tradeVolumeConsistent=false;
            }
            const std::int64_t observedVolume=std::max<std::int64_t>(std::max(0,order->second.TradeVolume),std::max<std::int64_t>(0,callbackTradeVolume));
            observedTradeVolumes[ticket.orderRef]=observedVolume;
            if(std::string(ticket.phase)=="CANCEL"||std::string(ticket.phase)=="BATCH_CANCEL")unexpectedTradeVolume+=observedVolume;
        }
        netTestPosition=calculateNet(netKnown);
    };

    LongPositionSnapshot finalPosition;
    bool positionQueryAvailable=false;
    const bool comparePositionDates=instrument->m_pExchange&&instrument->m_pExchange->UseTodayPosition;
    bool positionRestored=false;
    bool callbackStreamQuiet=false;
    bool streamStableThroughStop=false;
    OrderStreamSnapshot finalSnapshot;
    constexpr int finalizationAttempts=3;
    for(int attempt=1;attempt<=finalizationAttempts&&!finalSnapshot.destroyed;++attempt){
        restoreOwnedExposure();
        OrderStreamSnapshot quietSnapshot;
        callbackStreamQuiet=s.waitOrderActivityQuiet(callbackQuietMilliseconds,actionTimeout,quietSnapshot);
        if(!callbackStreamQuiet){
            l.warn("FINALIZE","owned-order stream was not quiet while the trade session was ready account="+accountId+" instrument="+instrumentId+" attempt="+std::to_string(attempt)+"/"+std::to_string(finalizationAttempts));continue;
        }
        analyzeFinalSnapshot(quietSnapshot);
        positionQueryAvailable=queryLongSpeculationPosition(s.extendedApi(),ydAccount,instrument,finalPosition);
        positionRestored=positionQueryAvailable&&(comparePositionDates
            ? finalPosition.today==baselinePosition.today&&finalPosition.history==baselinePosition.history&&finalPosition.other==baselinePosition.other
            : finalPosition.total()==baselinePosition.total());
        if(positionQueryAvailable){
            l.info("POSITION_VERIFY","account="+accountId+" snapshotTime="+timestampText()+" instrument="+instrumentId
                +" baselineToday="+std::to_string(baselinePosition.today)+" finalToday="+std::to_string(finalPosition.today)
                +" baselineHistory="+std::to_string(baselinePosition.history)+" finalHistory="+std::to_string(finalPosition.history)
                +" baselineOther="+std::to_string(baselinePosition.other)+" finalOther="+std::to_string(finalPosition.other)
                +" baselineTotal="+std::to_string(baselinePosition.total())+" finalTotal="+std::to_string(finalPosition.total())
                +" historicalBaselineProtected="+(historicalBaselineProtected?std::string("true"):std::string("false"))
                +" existingTodayBaselineAccepted="+(existingTodayBaselineAccepted?std::string("true"):std::string("false"))
                +" quantityRestored="+(positionRestored?std::string("true"):std::string("false")));
        }
        if(!allSettled||!netKnown||netTestPosition!=0||!positionRestored){
            l.warn("FINALIZE","final snapshot is not yet safe account="+accountId+" instrument="+instrumentId+" attempt="+std::to_string(attempt)+"/"+std::to_string(finalizationAttempts));callbackStreamQuiet=false;continue;
        }
        streamStableThroughStop=s.stopIfOrderStreamUnchanged(quietSnapshot.activityGeneration,quietSnapshot.sessionGeneration,finalSnapshot);
        if(!finalSnapshot.destroyed){
            l.warn("FINALIZE","order or session activity changed during final verification account="+accountId+" instrument="+instrumentId+" attempt="+std::to_string(attempt)+"/"+std::to_string(finalizationAttempts));callbackStreamQuiet=false;continue;
        }
        analyzeFinalSnapshot(finalSnapshot);
        if(!streamStableThroughStop){
            positionRestored=false;
            l.error("ALERT","owned-order activity arrived while the API was being destroyed; final position must be checked account="+accountId+" instrument="+instrumentId);
        }
    }
    if(!finalSnapshot.destroyed){
        l.error("ALERT","could not seal a stable owned-order snapshot before shutdown account="+accountId+" instrument="+instrumentId);
        s.stop();finalSnapshot=s.orderStreamSnapshot();analyzeFinalSnapshot(finalSnapshot);positionRestored=false;
    }
    const OrderActivitySnapshot activityEnd=finalSnapshot.activity;
    const OrderActivitySnapshot cleanupActivity=activityDelta(activityEnd,activityBeforeCleanup);
    const OrderActivitySnapshot totalActivity=activityDelta(activityEnd,activityStart);
    const BatchCancelActivitySnapshot batchActivityEnd=s.batchCancelActivity();
    const BatchCancelActivitySnapshot cleanupBatchActivity=batchCancelActivityDelta(batchActivityEnd,batchActivityBeforeCleanup);
    const BatchCancelActivitySnapshot totalBatchActivity=batchCancelActivityDelta(batchActivityEnd,batchActivityStart);
    const bool measurementCountsMatch=measuredActivity.orderApiRequests==requiredCount
        &&measuredActivity.orderRequestsSubmitted==requiredCount
        &&measuredActivity.uniqueAcceptedOrders==requiredCount
        &&measuredActivity.cancelApiRequests==requiredCount
        &&measuredActivity.cancelRequestsSubmitted==requiredCount
        &&measuredActivity.confirmedCancellations==requiredCount
        &&measuredActivity.failedCancelCallbacks==0
        &&measuredActivity.callbackValidationFailures==0;
    const bool cleanupRestored=streamStableThroughStop&&allSettled&&netKnown&&netTestPosition==0&&positionRestored;
    const bool cleanupUsedNoTradeApi=cleanupActivity.orderApiRequests==0
        &&cleanupActivity.orderRequestsSubmitted==0
        &&cleanupActivity.cancelApiRequests==0
        &&cleanupActivity.cancelRequestsSubmitted==0;
    const bool totalCountsStable=totalActivity.orderApiRequests==requiredCount
        &&totalActivity.orderRequestsSubmitted==requiredCount
        &&totalActivity.uniqueAcceptedOrders==requiredCount
        &&totalActivity.cancelApiRequests==requiredCount
        &&totalActivity.cancelRequestsSubmitted==requiredCount
        &&totalActivity.confirmedCancellations==requiredCount
        &&totalActivity.failedCancelCallbacks==0
        &&totalActivity.callbackValidationFailures==0;
    const bool sellCountsStable=sellMode
        &&totalActivity.orderApiRequests==2
        &&totalActivity.orderRequestsSubmitted==2
        &&totalActivity.uniqueAcceptedOrders==2
        &&totalActivity.cancelApiRequests==0
        &&totalActivity.cancelRequestsSubmitted==0
        &&totalActivity.confirmedCancellations==0
        &&totalActivity.failedCancelCallbacks==0
        &&totalActivity.callbackValidationFailures==0;
    const bool measurementBatchCountsMatch=measuredBatchActivity.apiCalls==1
        &&measuredBatchActivity.apiCallsSubmitted==1
        &&measuredBatchActivity.targetOrdersRequested==requiredCount
        &&measuredBatchActivity.targetOrdersSubmitted==requiredCount;
    const bool totalBatchCountsStable=totalBatchActivity.apiCalls==1
        &&totalBatchActivity.apiCallsSubmitted==1
        &&totalBatchActivity.targetOrdersRequested==requiredCount
        &&totalBatchActivity.targetOrdersSubmitted==requiredCount
        &&cleanupBatchActivity.apiCalls==0
        &&cleanupBatchActivity.targetOrdersRequested==0;
    const bool monitoringPass=countMode&&!executionException&&cancelCompleted==requiredCount&&measurementCountsMatch
        &&totalCountsStable&&callbackStreamQuiet&&streamStableThroughStop&&measurementTradeKnown&&tradeVolumeConsistent&&unexpectedTradeVolume==0&&cleanupRestored&&cleanupUsedNoTradeApi;
    const bool batchCancelSucceeded=batchMode&&!executionException&&stepFailure.empty()&&batchCallSubmitted&&batchCancelCompleted==requiredCount
        &&measurementCountsMatch&&measurementBatchCountsMatch&&totalCountsStable&&totalBatchCountsStable
        &&callbackStreamQuiet&&streamStableThroughStop&&measurementTradeKnown&&tradeVolumeConsistent&&unexpectedTradeVolume==0&&cleanupRestored&&cleanupUsedNoTradeApi;
    if(cleanupRestored){
        if(batchMode)r.observe("batch cancellation cleanup","account="+accountId+" instrument="+instrumentId+" noWorkingOrders=true ownedOrderNetPosition=0 baselineQuantityRestored=true");
        else r.pass("live cleanup","account="+accountId+" instrument="+instrumentId+" noWorkingOrders=true ownedOrderNetPosition=0 baselineQuantityRestored=true");
    }else{
        std::ostringstream detail;detail<<"MANUAL ACTION REQUIRED account="<<accountId<<" instrument="<<instrumentId
            <<" ownedOrderNetPosition="<<(netKnown?std::to_string(netTestPosition):std::string("UNKNOWN"))
            <<" baselineQuantityRestored="<<(positionQueryAvailable?(positionRestored?"true":"false"):"UNKNOWN")
            <<" streamStableThroughStop="<<(streamStableThroughStop?"true":"false")<<" unresolvedRefs=";
        if(unresolvedRefs.empty())detail<<"none";else for(std::size_t n=0;n<unresolvedRefs.size();++n){if(n)detail<<',';detail<<unresolvedRefs[n];}
        l.error("ALERT",detail.str());r.fail("live cleanup",detail.str());
    }

    if(countMode){
        std::ostringstream unresolved;
        if(unresolvedRefs.empty())unresolved<<"none";else for(std::size_t n=0;n<unresolvedRefs.size();++n){if(n)unresolved<<',';unresolved<<unresolvedRefs[n];}
        std::ostringstream line;line<<"status="<<(monitoringPass?"PASS":"FAIL")<<" | account="<<accountId<<" | countTime="<<timestampText()<<" | instrument="<<instrumentId
            <<" | orderApiRequests="<<measuredActivity.orderApiRequests
            <<" | orderRequestsSubmitted="<<measuredActivity.orderRequestsSubmitted
            <<" | uniqueAcceptedOrders="<<measuredActivity.uniqueAcceptedOrders
            <<" | cancelApiRequests="<<measuredActivity.cancelApiRequests
            <<" | cancelRequestsSubmitted="<<measuredActivity.cancelRequestsSubmitted
            <<" | confirmedCancellations="<<measuredActivity.confirmedCancellations
            <<" | failedCancelCallbacks="<<measuredActivity.failedCancelCallbacks
            <<" | callbackValidationFailures="<<measuredActivity.callbackValidationFailures
            <<" | unexpectedTradeVolume="<<(measurementTradeKnown?std::to_string(unexpectedTradeVolume):std::string("UNKNOWN"))
            <<" | tradeVolumeConsistent="<<(tradeVolumeConsistent?"true":"false")
            <<" | callbackStreamQuiet="<<(callbackStreamQuiet?"true":"false")
            <<" | streamStableThroughStop="<<(streamStableThroughStop?"true":"false")
            <<" | cleanupRestored="<<(cleanupRestored?"true":"false")
            <<" | unresolvedRefs="<<unresolved.str()
            <<" | cleanupOrderApiRequests="<<cleanupActivity.orderApiRequests
            <<" | cleanupCancelApiRequests="<<cleanupActivity.cancelApiRequests
            <<" | totalOrderApiRequests="<<totalActivity.orderApiRequests
            <<" | totalAcceptedOrders="<<totalActivity.uniqueAcceptedOrders
            <<" | totalCancelApiRequests="<<totalActivity.cancelApiRequests
            <<" | totalConfirmedCancellations="<<totalActivity.confirmedCancellations
            <<" | totalFailedCancelCallbacks="<<totalActivity.failedCancelCallbacks
            <<" | totalCallbackValidationFailures="<<totalActivity.callbackValidationFailures;
        if(monitoringPass)l.info("COUNT_RESULT",line.str());else l.error("COUNT_RESULT",line.str());
    }

    if(batchMode){
        std::ostringstream unresolved;
        if(unresolvedRefs.empty())unresolved<<"none";else for(std::size_t n=0;n<unresolvedRefs.size();++n){if(n)unresolved<<',';unresolved<<unresolvedRefs[n];}
        std::ostringstream line;line<<"account="<<accountId<<" event=BATCH_CANCEL_STATISTICS statisticsTime="<<timestampText()<<" instrument="<<instrumentId
            <<" batchSize="<<requiredCount
            <<" batchApiReturned="<<(batchCallSubmitted?"true":"false")
            <<" batchApiCalls="<<measuredBatchActivity.apiCalls
            <<" batchApiCallsSubmitted="<<measuredBatchActivity.apiCallsSubmitted
            <<" batchTargetOrders="<<measuredBatchActivity.targetOrdersRequested
            <<" batchTargetOrdersSubmitted="<<measuredBatchActivity.targetOrdersSubmitted
            <<" orderApiRequests="<<measuredActivity.orderApiRequests
            <<" orderRequestsSubmitted="<<measuredActivity.orderRequestsSubmitted
            <<" uniqueAcceptedOrders="<<measuredActivity.uniqueAcceptedOrders
            <<" cancelApiRequests="<<measuredActivity.cancelApiRequests
            <<" cancelRequestsSubmitted="<<measuredActivity.cancelRequestsSubmitted
            <<" confirmedCancellations="<<measuredActivity.confirmedCancellations
            <<" canceledOrders="<<batchCancelCompleted
            <<" failedCancelCallbacks="<<measuredActivity.failedCancelCallbacks
            <<" callbackValidationFailures="<<measuredActivity.callbackValidationFailures
            <<" unexpectedTradeVolume="<<(measurementTradeKnown?std::to_string(unexpectedTradeVolume):std::string("UNKNOWN"))
            <<" tradeVolumeConsistent="<<(tradeVolumeConsistent?"true":"false")
            <<" noWorkingOrders="<<(allSettled?"true":"false")
            <<" ownedNetLong="<<(netKnown?std::to_string(netTestPosition):std::string("UNKNOWN"))
            <<" positionRestored="<<(positionQueryAvailable?(positionRestored?"true":"false"):"UNKNOWN")
            <<" callbackStreamQuiet="<<(callbackStreamQuiet?"true":"false")
            <<" streamStableThroughStop="<<(streamStableThroughStop?"true":"false")
            <<" cleanupRestored="<<(cleanupRestored?"true":"false")
            <<" cleanupOrderApiRequests="<<cleanupActivity.orderApiRequests
            <<" cleanupCancelApiRequests="<<cleanupActivity.cancelApiRequests
            <<" unresolvedRefs="<<unresolved.str();
        if(batchCancelSucceeded){l.info("BATCH_CANCEL",line.str());r.observe("batch cancellation snapshot",line.str());}
        else{l.error("BATCH_CANCEL",line.str());r.fail("batch cancellation consistency",line.str()+(stepFailure.empty()?std::string():" reason="+stepFailure));}
    }

    if(executionException){
        r.fail(sellMode?"sell round trip exception":(countMode?"order/cancel monitoring exception":(batchMode?"batch cancellation exception":"basic trading workflow exception")),"account="+accountId+" reason="+executionExceptionDetail+"; cleanup was attempted");
    }else if(sellMode){
        const std::string detail="account="+accountId+" instrument="+instrumentId
            +" buyOpenFills="+std::to_string(openCompleted)
            +" setupPositionVerified="+(sellSetupPositionVerified?std::string("true"):std::string("false"))
            +" sellFills="+std::to_string(closeCompleted)
            +" sellOffset="+offsetName(closeOffset(instrument))
            +" ownedOrderNetPosition="+(netKnown?std::to_string(netTestPosition):std::string("UNKNOWN"))
            +" callbackStreamQuiet="+(callbackStreamQuiet?std::string("true"):std::string("false"))
            +" streamStableThroughStop="+(streamStableThroughStop?std::string("true"):std::string("false"))
            +" baselineQuantityRestored="+(positionQueryAvailable&&positionRestored?std::string("true"):std::string("false"));
        if(openCompleted==1&&sellSetupPositionVerified&&closeCompleted==1&&sellCountsStable&&cleanupUsedNoTradeApi
            &&callbackStreamQuiet&&streamStableThroughStop&&measurementTradeKnown&&tradeVolumeConsistent
            &&cleanupRestored)r.pass("sell contract round trip",detail);
        else r.fail("sell contract round trip",detail);
    }else if(countMode){
        const std::string detail="account="+accountId+" instrument="+instrumentId
            +" orderApiRequests="+std::to_string(measuredActivity.orderApiRequests)
            +" uniqueAcceptedOrders="+std::to_string(measuredActivity.uniqueAcceptedOrders)
            +" cancelApiRequests="+std::to_string(measuredActivity.cancelApiRequests)
            +" confirmedCancellations="+std::to_string(measuredActivity.confirmedCancellations)
            +" failedCancelCallbacks="+std::to_string(measuredActivity.failedCancelCallbacks)
            +" callbackValidationFailures="+std::to_string(measuredActivity.callbackValidationFailures)
            +" unexpectedTradeVolume="+(measurementTradeKnown?std::to_string(unexpectedTradeVolume):std::string("UNKNOWN"))
            +" tradeVolumeConsistent="+(tradeVolumeConsistent?std::string("true"):std::string("false"))
            +" streamStableThroughStop="+(streamStableThroughStop?std::string("true"):std::string("false"));
        if(monitoringPass)r.pass("order/cancel count monitoring",detail);
        else r.fail("order/cancel count monitoring",detail);
    }else if(!batchMode&&openCompleted==requiredCount&&cancelCompleted==requiredCount&&closeCompleted==requiredCount
        &&callbackStreamQuiet&&streamStableThroughStop&&measurementTradeKnown&&tradeVolumeConsistent&&unexpectedTradeVolume==0
        &&totalActivity.failedCancelCallbacks==0&&totalActivity.callbackValidationFailures==0&&cleanupRestored){
        r.pass("basic trading workflow","account="+accountId+" instrument="+instrumentId+" openFills=2 cancellations=2 closeFills=2 ownedOrderNetPosition=0 baselineToday="+std::to_string(baselinePosition.today)+" baselineHistory="+std::to_string(baselinePosition.history)+" baselineQuantityRestored=true");
    }else if(!batchMode)r.fail("basic trading workflow","account="+accountId+" instrument="+instrumentId+" openFills="+std::to_string(openCompleted)+" cancellations="+std::to_string(cancelCompleted)+" closeFills="+std::to_string(closeCompleted)+" callbackStreamQuiet="+(callbackStreamQuiet?std::string("true"):std::string("false"))+" streamStableThroughStop="+(streamStableThroughStop?std::string("true"):std::string("false")));
},true,batchMode);
}

int runTest02BasicTrade(const RunOptions& o){return runLiveOrderWorkflow(o,"2.2_basic_trade",LiveWorkflowMode::BasicTrade);}

int runTest13SellAg2610(const RunOptions& o){return runLiveOrderWorkflow(o,"13_sell_ag2610",LiveWorkflowMode::SellAg2610);}

int runTest03Reconnect(const RunOptions& o){return runEach(o,"2.3_reconnect",[&](const Account&a,const Config&c,Logger&l,TestResult&r){
    const int sessionTimeout=std::max(1,c.getInt("Reconnect.SessionTimeoutSeconds",120));
    const int disconnectTimeout=std::max(1,c.getInt("Reconnect.DisconnectTimeoutSeconds",30));
    const int reconnectTimeout=std::max(1,c.getInt("Reconnect.ReconnectTimeoutSeconds",120));
    YdSession s(o.ydConfig,a.username,a.password,l,false,monitorThresholds(c));if(!readySession(s,r,sessionTimeout,a.username))return;
    const YDAccount* ydAccount=s.api()?s.api()->getMyAccount():nullptr;
    const std::string accountId=ydAccount&&ydAccount->AccountID[0]?std::string(ydAccount->AccountID):a.username;
    const SessionEventGenerations baseline=s.eventGenerations();
    l.info("CONNECTION","account="+accountId+" channel=TRADE phase=NORMAL state=READY connectedTime="+baseline.tradeConnectedTime
        +" loginTime="+baseline.loginTime+" caughtUpTime="+baseline.caughtUpTime
        +" connectedGeneration="+std::to_string(baseline.tradeConnected)+" loginGeneration="+std::to_string(baseline.login)+" caughtUpGeneration="+std::to_string(baseline.caughtUp));
    r.pass("connection normal","account="+accountId+" state=READY caughtUpTime="+baseline.caughtUpTime);

    std::uint64_t disconnectRequestGeneration=0;
    if(!s.disconnectNow(disconnectRequestGeneration)){r.fail("connection abnormal","account="+accountId+" disconnect request was not issued from a ready session");return;}
    if(!s.waitTradeDisconnectedAfter(disconnectRequestGeneration,disconnectTimeout)){r.fail("connection abnormal","account="+accountId+" new disconnect event timeout");return;}
    const SessionEventGenerations disconnected=s.eventGenerations();
    l.info("CONNECTION","account="+accountId+" channel=TRADE phase=ABNORMAL state=DISCONNECTED eventTime="+disconnected.tradeDisconnectedTime+" generation="+std::to_string(disconnected.tradeDisconnected));
    r.pass("connection abnormal","account="+accountId+" state=DISCONNECTED eventTime="+disconnected.tradeDisconnectedTime+" generation="+std::to_string(disconnected.tradeDisconnected));

    if(!s.waitTradeConnectedAfter(disconnected.tradeDisconnected,reconnectTimeout)){r.fail("connection reconnected","account="+accountId+" new connected event timeout");return;}
    const SessionEventGenerations reconnected=s.eventGenerations();
    if(!reconnected.tradeConnectedState||reconnected.tradeConnected<=reconnected.tradeDisconnected){r.fail("connection reconnected","account="+accountId+" connection dropped again before reconnect validation");return;}
    l.info("CONNECTION","account="+accountId+" channel=TRADE phase=RECONNECTED state=CONNECTED eventTime="+reconnected.tradeConnectedTime+" generation="+std::to_string(reconnected.tradeConnected));
    r.pass("connection reconnected","account="+accountId+" state=CONNECTED eventTime="+reconnected.tradeConnectedTime+" generation="+std::to_string(reconnected.tradeConnected));

    if(!s.waitLoginAfter(reconnected.tradeConnected,reconnectTimeout)){r.fail("reconnect login","account="+accountId+" new login callback timeout");return;}
    const SessionEventGenerations relogged=s.eventGenerations();
    if(relogged.loginError!=0){r.fail("reconnect login","account="+accountId+" errorNo="+std::to_string(relogged.loginError)+" generation="+std::to_string(relogged.login));return;}
    r.pass("reconnect login","account="+accountId+" errorNo=0 loginTime="+relogged.loginTime+" generation="+std::to_string(relogged.login));

    const std::uint64_t readyAfter=std::max(reconnected.tradeConnected,relogged.login);
    if(!s.waitCaughtUpAfter(readyAfter,reconnectTimeout)){r.fail("reconnect ready","account="+accountId+" new caught-up callback timeout");return;}
    const SessionEventGenerations ready=s.eventGenerations();
    const bool orderedReady=ready.tradeDisconnected>disconnectRequestGeneration
        &&ready.tradeConnected>ready.tradeDisconnected
        &&ready.login>ready.tradeConnected
        &&ready.caughtUp>std::max(ready.tradeConnected,ready.login)
        &&ready.tradeConnectedState&&ready.loginCompleted&&ready.loginError==0&&ready.caughtUpState;
    if(!orderedReady){
        r.fail("reconnect ready","account="+accountId+" final session state/order invalid disconnectGeneration="+std::to_string(ready.tradeDisconnected)
            +" connectedGeneration="+std::to_string(ready.tradeConnected)+" loginGeneration="+std::to_string(ready.login)
            +" caughtUpGeneration="+std::to_string(ready.caughtUp)+" connected="+(ready.tradeConnectedState?std::string("true"):std::string("false"))
            +" loginError="+std::to_string(ready.loginError)+" caughtUp="+(ready.caughtUpState?std::string("true"):std::string("false")));return;
    }
    l.info("CONNECTION","account="+accountId+" channel=TRADE phase=RECONNECTED state=READY connectedTime="+ready.tradeConnectedTime
        +" loginTime="+ready.loginTime+" caughtUpTime="+ready.caughtUpTime
        +" connectedGeneration="+std::to_string(ready.tradeConnected)+" loginGeneration="+std::to_string(ready.login)+" caughtUpGeneration="+std::to_string(ready.caughtUp));
    r.pass("reconnect ready","account="+accountId+" state=READY caughtUpTime="+ready.caughtUpTime+" generation="+std::to_string(ready.caughtUp));
});}

int runTest04OrderCancelCount(const RunOptions& o){return runLiveOrderWorkflow(o,"2.4_order_cancel_count",LiveWorkflowMode::OrderCancelCount);}

int runTest05Duplicate(const RunOptions& o){return runEach(o,"2.5_duplicate",[&](const Account&a,const Config&c,Logger&l,TestResult&r){
    const int repeatCount=std::max(2,c.getInt("Duplicate.RepeatCount",2));
    const int volume=std::max(1,c.getInt("Duplicate.Volume",3));
    const double price=c.getDouble("Duplicate.Price",500.0);
    Monitor m(l,a.username);
    OrderIntent open{instID(o,c),YD_D_Buy,YD_OF_Open,price,volume,false};
    OrderIntent close{instID(o,c),YD_D_Sell,YD_OF_Close,price,volume,false};
    OrderIntent cancel{instID(o,c),YD_D_Buy,YD_OF_Open,price,volume,true};
    for(int n=0;n<repeatCount;++n)m.recordOrder(open);
    for(int n=0;n<repeatCount;++n)m.recordOrder(close);
    for(int n=0;n<repeatCount;++n)m.recordCancel(cancel);
    m.logDuplicateStatistics();

    const DuplicateStatistics& stats=m.duplicateStatistics();
    const int expectedDuplicates=repeatCount-1;
    const std::string detail="account="+a.username+" instrument="+open.instrument
        +" openInstructionCount="+std::to_string(stats.openInstructions)+" openDuplicateCount="+std::to_string(stats.duplicateOpenInstructions)
        +" closeInstructionCount="+std::to_string(stats.closeInstructions)+" closeDuplicateCount="+std::to_string(stats.duplicateCloseInstructions)
        +" cancelInstructionCount="+std::to_string(stats.cancelInstructions)+" cancelDuplicateCount="+std::to_string(stats.duplicateCancelInstructions);
    r.observe("duplicate order monitoring snapshot",detail);
    if(stats.openInstructions!=repeatCount||stats.closeInstructions!=repeatCount||stats.cancelInstructions!=repeatCount
        ||stats.duplicateOpenInstructions!=expectedDuplicates||stats.duplicateCloseInstructions!=expectedDuplicates
        ||stats.duplicateCancelInstructions!=expectedDuplicates||m.duplicateCount()!=expectedDuplicates*3){
        r.fail("duplicate monitor internal consistency",detail);
    }
},false,true);}

int runTest06Threshold(const RunOptions& o){return runEach(o,"2.6_threshold",[&](const Account&a,const Config&c,Logger&l,TestResult&r){
    const MonitorThresholds thresholds=monitorThresholds(c);
    const std::string configuration="account="+a.username
        +" orderCountThreshold="+std::to_string(thresholds.orderCount)
        +" cancelCountThreshold="+std::to_string(thresholds.cancelCount)
        +" duplicateCountThreshold="+std::to_string(thresholds.duplicateCount);
    if(thresholds.orderCount<=0||thresholds.cancelCount<=0||thresholds.duplicateCount<=0){r.fail("risk threshold configuration",configuration+"; all thresholds must be positive for this run");return;}

    Monitor m(l,a.username,thresholds);
    OrderIntent instruction{instID(o,c),YD_D_Buy,YD_OF_Open,100.0,1,false};
    for(int n=0;n<thresholds.orderCount;++n){auto unique=instruction;unique.price+=n;m.recordOrder(unique);}
    for(int n=0;n<thresholds.cancelCount;++n){auto unique=instruction;unique.cancel=true;unique.price+=n;m.recordCancel(unique);}
    for(int n=0;n<thresholds.duplicateCount;++n)m.recordOrder(instruction);
    m.logRiskStatistics();

    const std::string statistics=configuration
        +" orderCount="+std::to_string(m.orderCount())
        +" cancelCount="+std::to_string(m.cancelCount())
        +" duplicateCount="+std::to_string(m.duplicateCount())
        +" orderAlerted="+(m.orderAlerted()?std::string("true"):std::string("false"))
        +" cancelAlerted="+(m.cancelAlerted()?std::string("true"):std::string("false"))
        +" duplicateAlerted="+(m.duplicateAlerted()?std::string("true"):std::string("false"));
    r.observe("risk threshold monitoring snapshot",statistics);
    if(!m.orderAlerted()||!m.cancelAlerted()||!m.duplicateAlerted())r.fail("risk threshold monitor internal consistency",statistics);
},false,true);}

namespace {
enum class InstructionCheckMode { All, InvalidInstrument, InvalidPrice, InvalidVolume };

int runInstructionCheck(const RunOptions& o,const std::string& id,InstructionCheckMode mode){return runEach(o,id,[&](const Account&a,const Config&c,Logger&l,TestResult&r){
    YdSession s(o.ydConfig,a.username,a.password,l,false,monitorThresholds(c),false);
    if(!readySessionObserved(s,r,timeout(c),a.username))return;
    const bool checkInstrument=mode==InstructionCheckMode::All||mode==InstructionCheckMode::InvalidInstrument;
    const bool checkPrice=mode==InstructionCheckMode::All||mode==InstructionCheckMode::InvalidPrice;
    const bool checkVolume=mode==InstructionCheckMode::All||mode==InstructionCheckMode::InvalidVolume;
    const std::string modeName=mode==InstructionCheckMode::All?"ALL":(mode==InstructionCheckMode::InvalidInstrument?"INVALID_INSTRUMENT":(mode==InstructionCheckMode::InvalidPrice?"INVALID_PRICE":"INVALID_VOLUME"));
    const std::string invalidInstrumentId=checkInstrument?(o.instrument.empty()?c.get("Validation.InvalidInstrument","au2617"):o.instrument):std::string();
    const std::string referenceInstrumentId=(checkPrice||checkVolume)?((!o.instrument.empty()&&mode!=InstructionCheckMode::All)?o.instrument:c.get("Validation.ReferenceInstrument",c.get("Test.Instrument","au2612"))):std::string();

    if(checkInstrument){
        if(invalidInstrumentId.empty()){r.fail("invalid instrument precondition","configure Validation.InvalidInstrument or pass --instrument");return;}
        if(s.instrument(invalidInstrumentId)){r.fail("invalid instrument precondition","instrument="+invalidInstrumentId+" exists at the counter; choose a nonexistent contract code");return;}
    }
    const YDInstrument* instrument=nullptr;
    if(checkPrice||checkVolume){
        instrument=s.instrument(referenceInstrumentId);
        if(!instrument){r.fail("reference instrument exists","instrument="+referenceInstrumentId+"; use an existing contract");return;}
        if(instrument->Tick<=0){r.fail("reference instrument tick","instrument="+referenceInstrumentId+" tick must be positive");return;}
    }
    if(checkVolume&&instrument->MaxLimitOrderVolume>=INT_MAX){r.fail("construct over-limit volume","instrument maximum is INT_MAX");return;}

    const int repeatCount=std::max(1,c.getInt("Validation.RepeatCount",3));
    const int validVolume=instrument?std::max(1,instrument->MinLimitOrderVolume):1;
    if(instrument&&validVolume>instrument->MaxLimitOrderVolume){r.fail("reference instrument volume","no valid limit-order volume");return;}
    const double validPrice=instrument?instrument->Tick*1000.0:1.0;
    const double badPrice=instrument?validPrice+instrument->Tick*0.5:0.0;
    const int badVolume=instrument?instrument->MaxLimitOrderVolume+1:0;
    const OrderActivitySnapshot activityBefore=s.orderActivity();
    const InstructionValidationSnapshot validationBefore=s.instructionValidation();
    bool allReturnedRejected=true;

    if(checkInstrument)for(int n=0;n<repeatCount;++n)allReturnedRejected=s.sendLimitOrder(invalidInstrumentId,YD_D_Buy,YD_OF_Open,validPrice,validVolume)<0&&allReturnedRejected;
    if(checkPrice)for(int n=0;n<repeatCount;++n)allReturnedRejected=s.sendLimitOrder(instrument,YD_D_Buy,YD_OF_Open,badPrice,validVolume)<0&&allReturnedRejected;
    if(checkVolume)for(int n=0;n<repeatCount;++n)allReturnedRejected=s.sendLimitOrder(instrument,YD_D_Buy,YD_OF_Open,validPrice,badVolume)<0&&allReturnedRejected;

    const OrderActivitySnapshot activityAfter=s.orderActivity();
    const InstructionValidationSnapshot validationAfter=s.instructionValidation();
    const std::uint64_t instrumentRejected=validationAfter.invalidInstrument-validationBefore.invalidInstrument;
    const std::uint64_t priceRejected=validationAfter.invalidLimitPrice-validationBefore.invalidLimitPrice;
    const std::uint64_t volumeRejected=validationAfter.invalidLimitVolume-validationBefore.invalidLimitVolume;
    const std::uint64_t expectedInstrument=checkInstrument?static_cast<std::uint64_t>(repeatCount):0;
    const std::uint64_t expectedPrice=checkPrice?static_cast<std::uint64_t>(repeatCount):0;
    const std::uint64_t expectedVolume=checkVolume?static_cast<std::uint64_t>(repeatCount):0;
    const std::uint64_t apiCalls=activityAfter.orderApiRequests-activityBefore.orderApiRequests;
    const std::uint64_t apiSubmissions=activityAfter.orderRequestsSubmitted-activityBefore.orderRequestsSubmitted;
    const std::string statistics="account="+a.username+" event=INSTRUCTION_CHECK_STATISTICS testPoint="+modeName
        +" invalidInstrument="+(invalidInstrumentId.empty()?std::string("N/A"):invalidInstrumentId)
        +" referenceInstrument="+(referenceInstrumentId.empty()?std::string("N/A"):referenceInstrumentId)
        +" repeatCount="+std::to_string(repeatCount)
        +" invalidInstrumentRejected="+std::to_string(instrumentRejected)
        +" invalidPriceRejected="+std::to_string(priceRejected)
        +" invalidVolumeRejected="+std::to_string(volumeRejected)
        +" orderApiRequests="+std::to_string(apiCalls)
        +" orderRequestsSubmitted="+std::to_string(apiSubmissions)
        +" apiCalled="+(apiCalls==0?std::string("false"):std::string("true"));
    l.info("VALIDATION",statistics);
    r.observe("trading instruction validation snapshot",statistics);
    if(!allReturnedRejected||instrumentRejected!=expectedInstrument||priceRejected!=expectedPrice||volumeRejected!=expectedVolume
        ||apiCalls!=0||apiSubmissions!=0)r.fail("trading instruction validation consistency",statistics);
},false,true);}
}

int runTest071InvalidInstrument(const RunOptions& o){return runInstructionCheck(o,"2.7.1_invalid_instrument",InstructionCheckMode::InvalidInstrument);}
int runTest072InvalidPrice(const RunOptions& o){return runInstructionCheck(o,"2.7.2_invalid_price",InstructionCheckMode::InvalidPrice);}
int runTest073InvalidVolume(const RunOptions& o){return runInstructionCheck(o,"2.7.3_invalid_volume",InstructionCheckMode::InvalidVolume);}
int runTest07InstructionCheck(const RunOptions& o){return runInstructionCheck(o,"2.7_instruction_check",InstructionCheckMode::All);}

namespace {
enum class ErrorMessageMode { InsufficientFunds, NoPosition, MarketState };

const char* ydErrorName(int errorNo){
    switch(errorNo){
        case YD_ERROR_NoError:return "NO_ERROR";
        case YD_ERROR_NoPositionToClose:return "NO_POSITION_TO_CLOSE";
        case YD_ERROR_NoMoneyToOpen:return "NO_MONEY_TO_OPEN";
        case YD_ERROR_InstrumentCanNotTrade:return "INSTRUMENT_CANNOT_TRADE";
        case YD_ERROR_NotProperTime:return "NOT_PROPER_TIME";
        case YD_ERROR_InstrumentTradingPaused:return "INSTRUMENT_TRADING_PAUSED";
        case YD_ERROR_CannotTradeInCurrentSegment:return "CANNOT_TRADE_IN_CURRENT_SEGMENT";
        case YD_ERROR_SSEATPGatewayNoTradingTime:return "NO_TRADING_TIME";
        default:return "UNCLASSIFIED_YD_ERROR";
    }
}

std::set<int> expectedErrorNumbers(ErrorMessageMode mode){
    if(mode==ErrorMessageMode::InsufficientFunds)return {YD_ERROR_NoMoneyToOpen};
    if(mode==ErrorMessageMode::NoPosition)return {YD_ERROR_NoPositionToClose};
    return {YD_ERROR_InstrumentCanNotTrade,YD_ERROR_NotProperTime,YD_ERROR_InstrumentTradingPaused,YD_ERROR_CannotTradeInCurrentSegment,YD_ERROR_SSEATPGatewayNoTradingTime};
}

struct OwnedOrderSettleResult {
    bool callbackObserved=false;
    bool settled=false;
    YDOrder finalOrder{};
};

bool orderHasTerminalEvidence(const YDOrder& order){return order.ErrorNo!=0||terminal(order);}

OwnedOrderSettleResult settleOwnedOrder(YdSession& session,const YDInstrument* instrument,int orderRef,int actionTimeout,Logger& log,const std::string& purpose){
    OwnedOrderSettleResult result;
    YDOrder state{};
    if(!session.waitOrder(orderRef,actionTimeout,[](const YDOrder& order){return order.ErrorNo!=0||cancelableOrTerminal(order);},state)){
        log.error("CLEANUP","order state unavailable context="+purpose+" orderRef="+std::to_string(orderRef));
        return result;
    }
    result.callbackObserved=true;result.finalOrder=state;
    if(orderHasTerminalEvidence(state)){result.settled=true;return result;}
    if(state.OrderStatus!=YD_OS_Queuing||!hasAssignedSystemOrderId(state.OrderSysID,state.LongOrderSysID)){
        log.error("CLEANUP","working order cannot be canceled context="+purpose+" orderRef="+std::to_string(orderRef)+" status="+orderStatusName(state.OrderStatus));
        return result;
    }
    log.warn("CLEANUP","canceling working order during account-state reconciliation context="+purpose+" orderRef="+std::to_string(orderRef));
    if(!session.cancelOrder(instrument,state)){
        log.error("CLEANUP","cancelOrder returned false context="+purpose+" orderRef="+std::to_string(orderRef));
        return result;
    }
    YDOrder finalOrder{};
    const int cancelResult=session.waitCancelTerminal(orderRef,actionTimeout,finalOrder);
    if(cancelResult!=0){
        log.error("CLEANUP","cancel did not reach terminal state context="+purpose+" orderRef="+std::to_string(orderRef)+" result="+std::to_string(cancelResult));
        return result;
    }
    result.finalOrder=finalOrder;result.settled=orderHasTerminalEvidence(finalOrder);return result;
}

OwnedOrderSettleResult waitCleanupClose(YdSession& session,const YDInstrument* instrument,int orderRef,int actionTimeout,Logger& log){
    OwnedOrderSettleResult result;YDOrder state{};
    if(session.waitOrder(orderRef,actionTimeout,[](const YDOrder& order){return orderHasTerminalEvidence(order);},state)){
        result.callbackObserved=true;result.settled=true;result.finalOrder=state;return result;
    }
    log.warn("CLEANUP","position-restoring close did not finish before timeout orderRef="+std::to_string(orderRef));
    return settleOwnedOrder(session,instrument,orderRef,actionTimeout,log,"POSITION_RESTORE");
}

bool ownedTradeVolumeConsistent(const OrderStreamSnapshot& snapshot,const std::vector<LiveTicket>& tickets,std::size_t ticketCount,std::int64_t& totalTradeVolume){
    totalTradeVolume=0;
    if(ticketCount>tickets.size())return false;
    for(std::size_t index=0;index<ticketCount;++index){
        const LiveTicket& ticket=tickets[index];
        const auto order=snapshot.orders.find(ticket.orderRef);if(order==snapshot.orders.end())return false;
        const auto trade=snapshot.tradeVolumeByOrderRef.find(ticket.orderRef);
        const std::int64_t callbackVolume=trade==snapshot.tradeVolumeByOrderRef.end()?0:trade->second;
        if(order->second.TradeVolume<0||callbackVolume<0||order->second.TradeVolume!=callbackVolume||callbackVolume>ticket.volume)return false;
        if(totalTradeVolume>LLONG_MAX-callbackVolume)return false;
        totalTradeVolume+=callbackVolume;
    }
    return true;
}

bool ownedNetLong(const OrderStreamSnapshot& snapshot,const std::vector<LiveTicket>& tickets,int& netLong){
    std::int64_t total=0;
    for(const LiveTicket& ticket:tickets){
        const auto order=snapshot.orders.find(ticket.orderRef);if(order==snapshot.orders.end())return false;
        const auto trade=snapshot.tradeVolumeByOrderRef.find(ticket.orderRef);
        const std::int64_t callbackVolume=trade==snapshot.tradeVolumeByOrderRef.end()?0:trade->second;
        if(order->second.TradeVolume<0||callbackVolume<0||order->second.TradeVolume!=callbackVolume||callbackVolume>ticket.volume)return false;
        if(ticket.direction==YD_D_Buy&&ticket.offset==YD_OF_Open)total+=callbackVolume;
        else if(ticket.direction==YD_D_Sell&&ticket.offset!=YD_OF_Open)total-=callbackVolume;
        if(total<INT_MIN||total>INT_MAX)return false;
    }
    netLong=static_cast<int>(total);return true;
}

bool allOwnedOrdersSettled(const OrderStreamSnapshot& snapshot,const std::vector<LiveTicket>& tickets){
    for(const LiveTicket& ticket:tickets){const auto order=snapshot.orders.find(ticket.orderRef);if(order==snapshot.orders.end()||!orderHasTerminalEvidence(order->second))return false;}return true;
}

bool sameLongPosition(const LongPositionSnapshot& before,const LongPositionSnapshot& after,const YDInstrument* instrument){
    return instrument&&instrument->m_pExchange&&instrument->m_pExchange->UseTodayPosition
        ? before.today==after.today&&before.history==after.history&&before.other==after.other
        : before.total()==after.total();
}

bool cleanupClosePrice(const YDInstrument* instrument,const YDMarketData& market,double& price){
    if(!instrument)return false;
    const double requested=usablePrice(market.LowerLimitPrice)?market.LowerLimitPrice:market.BidPrice-instrument->Tick*2.0;
    return boundedLegalPrice(requested,instrument,market,price);
}

ErrorMessageMode configuredErrorMode(const RunOptions& options,bool& known){
    known=true;
    if(options.caseName.empty()||options.caseName=="no-position")return ErrorMessageMode::NoPosition;
    if(options.caseName=="insufficient-funds")return ErrorMessageMode::InsufficientFunds;
    if(options.caseName=="market-state")return ErrorMessageMode::MarketState;
    known=false;return ErrorMessageMode::NoPosition;
}

int runErrorMessageCase(const RunOptions& o,const std::string& id,ErrorMessageMode mode){return runEach(o,id,[&](const Account&a,const Config&c,Logger&l,TestResult&r){
    // This gate must remain before YdSession construction: without --live no order/cancel API can be called.
    if(!o.live){r.skip("live exchange-error monitoring","requires --live in a broker-approved test environment");return;}
    const int sessionTimeout=std::max(1,c.getInt("Trade.SessionTimeoutSeconds",120));
    const int actionTimeout=std::max(1,c.getInt("ErrorTest.ActionTimeoutSeconds",c.getInt("Trade.ActionTimeoutSeconds",30)));
    const int callbackQuietMilliseconds=std::max(1,c.getInt("Trade.CallbackQuietMilliseconds",2000));
    const int passiveOffsetTicks=std::max(1,c.getInt("ErrorTest.PassiveOffsetTicks",50));
    const bool cumulativeFundsMode=mode==ErrorMessageMode::InsufficientFunds;
    constexpr int hardMaxFundsOrders=100;
    constexpr int hardMaxFundsRestoreAttempts=100;
    constexpr int minimumFundsOrderIntervalMilliseconds=100;
    constexpr int maximumFundsOrderIntervalMilliseconds=60000;
    const int maxTriggerOrders=cumulativeFundsMode?c.getInt("ErrorTest.InsufficientFundsMaxOrders",hardMaxFundsOrders):1;
    if(cumulativeFundsMode&&(maxTriggerOrders<2||maxTriggerOrders>hardMaxFundsOrders)){
        r.fail("error-message configuration","ErrorTest.InsufficientFundsMaxOrders="+std::to_string(maxTriggerOrders)
            +" allowed=[2,"+std::to_string(hardMaxFundsOrders)+"]; no order sent");return;
    }
    const int maxRestoreAttempts=cumulativeFundsMode
        ?c.getInt("ErrorTest.InsufficientFundsMaxRestoreAttempts",maxTriggerOrders)
        :std::max(1,c.getInt("ErrorTest.MaxRestoreAttempts",3));
    if(cumulativeFundsMode&&(maxRestoreAttempts<1||maxRestoreAttempts>hardMaxFundsRestoreAttempts)){
        r.fail("error-message configuration","ErrorTest.InsufficientFundsMaxRestoreAttempts="+std::to_string(maxRestoreAttempts)
            +" allowed=[1,"+std::to_string(hardMaxFundsRestoreAttempts)+"]; no order sent");return;
    }
    const int fundsOrderIntervalMilliseconds=cumulativeFundsMode?c.getInt("ErrorTest.InsufficientFundsOrderIntervalMilliseconds",500):0;
    if(cumulativeFundsMode&&(fundsOrderIntervalMilliseconds<minimumFundsOrderIntervalMilliseconds||fundsOrderIntervalMilliseconds>maximumFundsOrderIntervalMilliseconds)){
        r.fail("error-message configuration","ErrorTest.InsufficientFundsOrderIntervalMilliseconds="+std::to_string(fundsOrderIntervalMilliseconds)
            +" allowed=["+std::to_string(minimumFundsOrderIntervalMilliseconds)+","+std::to_string(maximumFundsOrderIntervalMilliseconds)+"]; no order sent");return;
    }
    const std::set<int> expected=expectedErrorNumbers(mode);

    YdSession s(o.ydConfig,a.username,a.password,l,true,monitorThresholds(c),o.live);
    if(!readySessionObserved(s,r,sessionTimeout,a.username))return;
    const std::string requestedInstrument=instID(o,c);
    const YDInstrument* instrument=s.instrument(requestedInstrument);
    if(!instrument){r.fail("error-message preflight","instrument="+requestedInstrument+" does not exist; no order sent");return;}
    const YDAccount* ydAccount=s.api()?s.api()->getMyAccount():nullptr;
    if(!ydAccount){r.fail("error-message preflight","account unavailable; no order sent");return;}
    const std::string accountId=ydAccount->AccountID[0]?std::string(ydAccount->AccountID):a.username;
    const YDAccountInstrumentInfo* accountInstrument=s.api()->getAccountInstrumentInfo(instrument);
    if(ydAccount->TradingRight!=YD_TR_Allow||!accountInstrument||accountInstrument->TradingRight!=YD_TR_Allow){
        r.fail("error-message preflight","account/instrument trading right is not ALLOW; no order sent");return;
    }
    LongPositionSnapshot baselinePosition;
    if(!queryLongSpeculationPosition(s.extendedApi(),ydAccount,instrument,baselinePosition)){
        r.fail("error-message preflight","cannot query baseline long position; no order sent");return;
    }
    l.info("POSITION","account="+accountId+" event=POSITION_SNAPSHOT instrument="+requestedInstrument+" direction=LONG hedge=SPECULATION today="+std::to_string(baselinePosition.today)+" history="+std::to_string(baselinePosition.history)+" other="+std::to_string(baselinePosition.other)+" total="+std::to_string(baselinePosition.total()));
    const bool existingLongBaselineAllowed=mode==ErrorMessageMode::InsufficientFunds;
    if(!existingLongBaselineAllowed&&baselinePosition.total()!=0){
        r.fail("error-message preflight","instrument has existing long speculation position="+std::to_string(baselinePosition.total())+"; choose a zero-position contract; no order sent");return;
    }
    if(existingLongBaselineAllowed&&baselinePosition.total()!=0){
        l.info("POSITION_PROTECTION","account="+accountId+" instrument="+requestedInstrument
            +" mode=QUANTITY_BASELINE_PRESERVED baselineToday="+std::to_string(baselinePosition.today)
            +" baselineHistory="+std::to_string(baselinePosition.history)
            +" baselineOther="+std::to_string(baselinePosition.other)
            +" baselineTotal="+std::to_string(baselinePosition.total())
            +" cleanupScope=OWNED_NET_LONG_ONLY");
    }
    if(!usablePrice(instrument->Tick)||instrument->MinLimitOrderVolume<1||instrument->MaxLimitOrderVolume<instrument->MinLimitOrderVolume){
        r.fail("error-message preflight","invalid instrument tick or limit-order volume range; no order sent");return;
    }

    const int direction=mode==ErrorMessageMode::NoPosition?YD_D_Sell:YD_D_Buy;
    const int offset=mode==ErrorMessageMode::NoPosition?closeOffset(instrument):YD_OF_Open;
    const int volume=cumulativeFundsMode?c.getInt("ErrorTest.InsufficientFundsOrderVolume",instrument->MinLimitOrderVolume):instrument->MinLimitOrderVolume;
    if(volume<instrument->MinLimitOrderVolume||volume>instrument->MaxLimitOrderVolume){
        r.fail("error-message preflight","configured volume="+std::to_string(volume)+" allowed=["+std::to_string(instrument->MinLimitOrderVolume)+","+std::to_string(instrument->MaxLimitOrderVolume)+"]; no order sent");return;
    }
    if(cumulativeFundsMode){
        const std::int64_t maximumPotentialOpenVolume=static_cast<std::int64_t>(maxTriggerOrders)*volume;
        const std::int64_t maximumAutomaticRestoreVolume=static_cast<std::int64_t>(maxRestoreAttempts)*instrument->MaxLimitOrderVolume;
        if(maximumPotentialOpenVolume>maximumAutomaticRestoreVolume){
            r.fail("error-message preflight","configured cumulative open volume="+std::to_string(maximumPotentialOpenVolume)
                +" exceeds automatic restore capacity="+std::to_string(maximumAutomaticRestoreVolume)+"; reduce ErrorTest.InsufficientFundsOrderVolume/MaxOrders or increase ErrorTest.InsufficientFundsMaxRestoreAttempts; no order sent");return;
        }
    }

    YDMarketData market{};const bool marketAvailable=s.subscribe(instrument)&&s.waitMarketData(instrument->InstrumentRef,actionTimeout,market);
    const double configuredPrice=c.getDouble("ErrorTest.OrderPrice",0.0);
    double price=0;
    if(configuredPrice>0){
        const double legalConfigured=legalPrice(configuredPrice,instrument->Tick);
        if(marketAvailable){if(!boundedLegalPrice(legalConfigured,instrument,market,price)){r.fail("error-message preflight","configured price cannot be made legal; no order sent");return;}}
        else price=legalConfigured;
    }else{
        if(!marketAvailable||!usablePrice(market.BidPrice)||!usablePrice(market.AskPrice)){
            r.fail("error-message preflight","market price unavailable; set ErrorTest.OrderPrice to a legal price before an after-hours run; no order sent");return;
        }
        const double requested=direction==YD_D_Buy?market.BidPrice-passiveOffsetTicks*instrument->Tick:market.AskPrice+passiveOffsetTicks*instrument->Tick;
        if(!boundedLegalPrice(requested,instrument,market,price)){
            r.fail("error-message preflight","cannot construct a legal passive limit price; no order sent");return;
        }
        const double tolerance=instrument->Tick*1e-6;
        if((direction==YD_D_Buy&&price>=market.AskPrice-tolerance)||(direction==YD_D_Sell&&price<=market.BidPrice+tolerance)){
            r.fail("error-message preflight","constructed order is not passive; no order sent");return;
        }
    }
    if(!usablePrice(price)){
        r.fail("error-message preflight","configured/constructed order price is not positive; no order sent");return;
    }
    if(cumulativeFundsMode){
        if(!marketAvailable||!usablePrice(market.BidPrice)||!usablePrice(market.AskPrice)){
            r.fail("error-message preflight","cumulative insufficient-funds orders require a live bid/ask snapshot; no order sent");return;
        }
        const double tolerance=instrument->Tick*1e-6;
        if(price>=market.BidPrice-tolerance||price>=market.AskPrice-tolerance){
            r.fail("error-message preflight","cumulative insufficient-funds BUY OPEN price must remain below the current bid and ask; no order sent");return;
        }
    }

    l.info("SYSTEM","account="+accountId+" component=ORDER_ERROR_MONITOR state=ACTIVE instrument="+requestedInstrument
        +" submissionMode="+(cumulativeFundsMode?std::string("CUMULATIVE_WORKING_ORDERS"):std::string("SINGLE_ORDER"))
        +" maxOrderAttempts="+std::to_string(maxTriggerOrders)+" volumePerOrder="+std::to_string(volume)
        +" orderIntervalMilliseconds="+std::to_string(fundsOrderIntervalMilliseconds));
    r.observe("order error monitor active","account="+accountId+" instrument="+requestedInstrument);

    const OrderActivitySnapshot activityBefore=s.orderActivity();
    std::vector<LiveTicket> tickets;tickets.reserve(static_cast<std::size_t>(maxTriggerOrders+maxRestoreAttempts));
    int triggerOrderAttempts=0;
    int peakWorkingOrders=0;
    int receivedErrors=0;
    int actualErrorNo=0;
    int errorOrderRef=0;
    bool errorCallbackReceived=false;
    bool expectedErrorReceived=false;
    bool restoreBlocked=false;
    std::set<int> measurementWorkingRefs;
    std::set<int> initialCancelRequestedRefs;
    OrderActivitySnapshot cleanupStart=activityBefore;
    if(cumulativeFundsMode){
        std::vector<int> heldWorkingRefs;
        heldWorkingRefs.reserve(static_cast<std::size_t>(maxTriggerOrders));
        std::set<int> recordedErrorRefs;
        bool triggerStageFailed=false;
        std::uint64_t marketVersion=0;
        bool hasLastOrderSubmissionTime=false;
        std::chrono::steady_clock::time_point lastOrderSubmissionTime{};
        auto recordCabinetError=[&](int orderRef,int errorNo){
            if(recordedErrorRefs.insert(orderRef).second)++receivedErrors;
            if(errorOrderRef==0)errorOrderRef=orderRef;
            actualErrorNo=errorNo;errorCallbackReceived=true;expectedErrorReceived=expected.count(errorNo)>0;
            l.error("ERROR_MONITOR","account="+accountId+" event=ORDER_LOAD_STOPPED reason=CABINET_REJECTED orderRef="+std::to_string(orderRef)
                +" errorNo="+std::to_string(errorNo)+" errorName="+ydErrorName(errorNo)+" orderAttempts="+std::to_string(triggerOrderAttempts));
        };
        auto auditHeldOrders=[&](){
            const OrderStreamSnapshot snapshot=s.orderStreamSnapshot();
            for(const int orderRef:heldWorkingRefs){
                const auto ticket=std::find_if(tickets.begin(),tickets.end(),[&](const LiveTicket& value){return value.orderRef==orderRef;});
                const auto state=snapshot.orders.find(orderRef);
                if(ticket==tickets.end()||state==snapshot.orders.end()){
                    l.error("ERROR_MONITOR","account="+accountId+" event=ORDER_LOAD_STOPPED reason=WORKING_ORDER_STATE_UNAVAILABLE orderRef="+std::to_string(orderRef));
                    return false;
                }
                if(state->second.ErrorNo!=0){recordCabinetError(orderRef,state->second.ErrorNo);return false;}
                std::string mismatch;
                if(!orderMatchesTicket(*ticket,state->second,instrument->Tick,mismatch)){
                    l.error("ERROR_MONITOR","account="+accountId+" event=ORDER_LOAD_STOPPED reason=WORKING_ORDER_IDENTITY_INVALID orderRef="+std::to_string(orderRef)+" detail="+mismatch);
                    return false;
                }
                const auto trade=snapshot.tradeVolumeByOrderRef.find(orderRef);
                const std::int64_t callbackVolume=trade==snapshot.tradeVolumeByOrderRef.end()?0:trade->second;
                if(state->second.OrderStatus!=YD_OS_Queuing||state->second.TradeVolume!=0||callbackVolume!=0){
                    l.error("ERROR_MONITOR","account="+accountId+" event=ORDER_LOAD_STOPPED reason=WORKING_ORDER_CHANGED orderRef="+std::to_string(orderRef)
                        +" status="+orderStatusName(state->second.OrderStatus)+" orderTradeVolume="+std::to_string(state->second.TradeVolume)
                        +" callbackTradeVolume="+std::to_string(callbackVolume));
                    return false;
                }
            }
            return true;
        };
        try{
            for(int sequence=1;sequence<=maxTriggerOrders;++sequence){
                if(hasLastOrderSubmissionTime){
                    const auto nextSubmissionTime=lastOrderSubmissionTime+std::chrono::milliseconds(fundsOrderIntervalMilliseconds);
                    if(const auto now=std::chrono::steady_clock::now();now<nextSubmissionTime)std::this_thread::sleep_until(nextSubmissionTime);
                }
                if(!s.waitConnected(actionTimeout)){
                    l.error("ERROR_MONITOR","account="+accountId+" event=ORDER_LOAD_STOPPED reason=TRADE_CONNECTION_UNAVAILABLE orderAttempts="+std::to_string(triggerOrderAttempts));
                    triggerStageFailed=true;break;
                }
                if(!s.waitCaughtUp(actionTimeout)){
                    l.error("ERROR_MONITOR","account="+accountId+" event=ORDER_LOAD_STOPPED reason=HISTORY_NOT_CAUGHT_UP orderAttempts="+std::to_string(triggerOrderAttempts));
                    triggerStageFailed=true;break;
                }
                YDMarketData latestMarket{};
                if(!s.waitNextMarketData(instrument->InstrumentRef,marketVersion,actionTimeout,latestMarket)){
                    l.error("ERROR_MONITOR","account="+accountId+" event=ORDER_LOAD_STOPPED reason=FRESH_MARKET_DATA_UNAVAILABLE orderAttempts="+std::to_string(triggerOrderAttempts));
                    triggerStageFailed=true;break;
                }
                std::string marketReason;
                if(!usableMarket(instrument,latestMarket,marketReason)){
                    l.error("ERROR_MONITOR","account="+accountId+" event=ORDER_LOAD_STOPPED reason="+marketReason+" orderAttempts="+std::to_string(triggerOrderAttempts));
                    triggerStageFailed=true;break;
                }
                double checkedPrice=0;
                const double tolerance=instrument->Tick*1e-6;
                if(!boundedLegalPrice(price,instrument,latestMarket,checkedPrice)||std::fabs(checkedPrice-price)>tolerance){
                    l.error("ERROR_MONITOR","account="+accountId+" event=ORDER_LOAD_STOPPED reason=FIXED_PRICE_NOT_LEGAL orderAttempts="+std::to_string(triggerOrderAttempts));
                    triggerStageFailed=true;break;
                }
                if(price>=latestMarket.BidPrice-tolerance||price>=latestMarket.AskPrice-tolerance){
                    l.error("ERROR_MONITOR","account="+accountId+" event=ORDER_LOAD_STOPPED reason=FIXED_PRICE_NOT_PASSIVE orderAttempts="+std::to_string(triggerOrderAttempts));
                    triggerStageFailed=true;break;
                }
                if(!auditHeldOrders()){
                    if(!errorCallbackReceived)triggerStageFailed=true;
                    break;
                }

                const int triggerRef=s.sendLimitOrder(instrument,YD_D_Buy,YD_OF_Open,price,volume);
                lastOrderSubmissionTime=std::chrono::steady_clock::now();hasLastOrderSubmissionTime=true;
                if(triggerRef<0){
                    l.error("ERROR_MONITOR","account="+accountId+" event=ORDER_LOAD_STOPPED reason=INSERT_ORDER_NOT_SUBMITTED orderAttempts="+std::to_string(triggerOrderAttempts));
                    triggerStageFailed=true;break;
                }
                ++triggerOrderAttempts;
                tickets.push_back({"FUNDS_LOAD",sequence,triggerRef,YD_D_Buy,YD_OF_Open,price,volume,maxTriggerOrders});
                YDOrder state{};
                if(!s.waitOrder(triggerRef,actionTimeout,[](const YDOrder& order){return order.ErrorNo!=0||cancelableOrTerminal(order);},state)){
                    l.error("ERROR_MONITOR","account="+accountId+" event=ORDER_LOAD_STOPPED reason=ORDER_CALLBACK_TIMEOUT orderRef="+std::to_string(triggerRef)+" orderAttempts="+std::to_string(triggerOrderAttempts));
                    triggerStageFailed=true;break;
                }
                if(state.ErrorNo!=0){recordCabinetError(triggerRef,state.ErrorNo);break;}
                if(state.OrderStatus==YD_OS_Queuing&&hasAssignedSystemOrderId(state.OrderSysID,state.LongOrderSysID)){
                    heldWorkingRefs.push_back(triggerRef);
                    measurementWorkingRefs.insert(triggerRef);
                    peakWorkingOrders=std::max(peakWorkingOrders,static_cast<int>(heldWorkingRefs.size()));
                    if(!auditHeldOrders()){
                        if(!errorCallbackReceived)triggerStageFailed=true;
                        break;
                    }
                    const OrderActivitySnapshot progress=activityDelta(s.orderActivity(),activityBefore);
                    l.info("MONITOR","account="+accountId+" activity=ORDER_LOAD_PROGRESS activityTime="+timestampText()
                        +" instrument="+requestedInstrument+" orderAttempts="+std::to_string(triggerOrderAttempts)
                        +" workingOrders="+std::to_string(heldWorkingRefs.size())+" volumePerOrder="+std::to_string(volume)
                        +" orderPrice="+snapshotNumber(price)+" orderApiRequests="+std::to_string(progress.orderApiRequests)
                        +" cancelApiRequests="+std::to_string(progress.cancelApiRequests));
                    continue;
                }
                l.error("ERROR_MONITOR","account="+accountId+" event=ORDER_LOAD_STOPPED reason=ORDER_NOT_WORKING_UNFILLED orderRef="+std::to_string(triggerRef)
                    +" status="+orderStatusName(state.OrderStatus)+" traded="+std::to_string(state.TradeVolume)+"/"+std::to_string(state.OrderVolume));
                triggerStageFailed=true;break;
            }
        }catch(const std::exception& ex){
            l.error("ERROR_MONITOR","account="+accountId+" event=ORDER_LOAD_STOPPED reason=EXECUTION_EXCEPTION detail="+std::string(ex.what())+" orderAttempts="+std::to_string(triggerOrderAttempts));
            triggerStageFailed=true;
        }catch(...){
            l.error("ERROR_MONITOR","account="+accountId+" event=ORDER_LOAD_STOPPED reason=UNKNOWN_EXECUTION_EXCEPTION orderAttempts="+std::to_string(triggerOrderAttempts));
            triggerStageFailed=true;
        }
        if(!expectedErrorReceived&&!triggerStageFailed&&triggerOrderAttempts==maxTriggerOrders){
            l.error("ERROR_MONITOR","account="+accountId+" event=ORDER_LOAD_LIMIT_REACHED orderAttempts="+std::to_string(triggerOrderAttempts)
                +" workingOrders="+std::to_string(heldWorkingRefs.size()));
        }

        cleanupStart=s.orderActivity();
        const OrderStreamSnapshot cancelSnapshot=s.orderStreamSnapshot();
        for(const LiveTicket& ticket:tickets){
            const auto latest=cancelSnapshot.orders.find(ticket.orderRef);
            if(latest==cancelSnapshot.orders.end()||latest->second.ErrorNo!=0||orderHasTerminalEvidence(latest->second))continue;
            std::string mismatch;
            if(!orderMatchesTicket(ticket,latest->second,instrument->Tick,mismatch)){
                l.error("CLEANUP","order identity invalid before cancel orderRef="+std::to_string(ticket.orderRef)+" detail="+mismatch);
                restoreBlocked=true;continue;
            }
            if(latest->second.OrderStatus!=YD_OS_Queuing)continue;
            try{
                if(s.cancelOrder(instrument,latest->second))initialCancelRequestedRefs.insert(ticket.orderRef);
                else l.warn("CLEANUP","initial cancel request was not submitted orderRef="+std::to_string(ticket.orderRef)+"; reconciling latest state");
            }catch(const std::exception& ex){
                l.error("CLEANUP","cancel request exception orderRef="+std::to_string(ticket.orderRef)+" detail="+std::string(ex.what()));restoreBlocked=true;
            }catch(...){l.error("CLEANUP","unknown cancel request exception orderRef="+std::to_string(ticket.orderRef));restoreBlocked=true;}
        }
        for(const LiveTicket& ticket:tickets){
            bool settled=false;
            try{
                if(initialCancelRequestedRefs.count(ticket.orderRef)>0){
                    YDOrder finalOrder{};
                    const int cancelResult=s.waitCancelTerminal(ticket.orderRef,actionTimeout,finalOrder);
                    settled=cancelResult==0&&orderHasTerminalEvidence(finalOrder);
                    if(!settled)l.error("CLEANUP","initial cancel did not reach terminal state orderRef="+std::to_string(ticket.orderRef)+" result="+std::to_string(cancelResult));
                }
                if(!settled){
                    const OwnedOrderSettleResult reconciled=settleOwnedOrder(s,instrument,ticket.orderRef,actionTimeout,l,"FUNDS_LOAD");
                    settled=reconciled.settled;
                }
            }catch(const std::exception& ex){
                l.error("CLEANUP","order reconciliation exception orderRef="+std::to_string(ticket.orderRef)+" detail="+std::string(ex.what()));
            }catch(...){l.error("CLEANUP","unknown order reconciliation exception orderRef="+std::to_string(ticket.orderRef));}
            if(!settled)restoreBlocked=true;
        }
    }else{
        const int triggerRef=s.sendLimitOrder(instrument,direction,offset,price,volume);
        if(triggerRef<0){r.fail("exchange-error callback","insertOrder was not submitted; no cabinet notifyOrder error was received");return;}
        ++triggerOrderAttempts;
        tickets.push_back({"ORDER_REQUEST",1,triggerRef,direction,offset,price,volume});
        const OwnedOrderSettleResult trigger=settleOwnedOrder(s,instrument,triggerRef,actionTimeout,l,"ORDER_REQUEST");
        actualErrorNo=trigger.callbackObserved?trigger.finalOrder.ErrorNo:0;
        errorCallbackReceived=trigger.callbackObserved&&actualErrorNo!=0;
        receivedErrors=errorCallbackReceived?1:0;
        errorOrderRef=errorCallbackReceived?triggerRef:0;
        expectedErrorReceived=errorCallbackReceived&&expected.count(actualErrorNo)>0;
        if(!trigger.settled)restoreBlocked=true;
        cleanupStart=s.orderActivity();
    }
    const std::size_t triggerTicketCount=static_cast<std::size_t>(triggerOrderAttempts);
    for(int attempt=1;attempt<=maxRestoreAttempts;++attempt){
        OrderStreamSnapshot snapshot;
        s.waitOrderActivityQuiet(callbackQuietMilliseconds,actionTimeout,snapshot);
        if(!allOwnedOrdersSettled(snapshot,tickets)){
            l.error("CLEANUP","owned working orders remain; automatic position close is blocked until every order is terminal");restoreBlocked=true;break;
        }
        int netLong=0;
        if(!ownedNetLong(snapshot,tickets,netLong)){l.error("CLEANUP","cannot calculate owned net position from callbacks");restoreBlocked=true;break;}
        LongPositionSnapshot currentPosition;
        if(!queryLongSpeculationPosition(s.extendedApi(),ydAccount,instrument,currentPosition)){l.error("CLEANUP","cannot query current long position");restoreBlocked=true;break;}
        if(netLong==0)break;
        const bool positionMatchesOwnedIncrease=separatesTodayPosition(instrument)
            ? currentPosition.today==baselinePosition.today+netLong&&currentPosition.history==baselinePosition.history&&currentPosition.other==baselinePosition.other
            : currentPosition.total()==baselinePosition.total()+netLong;
        if(netLong<0||!positionMatchesOwnedIncrease){
            l.error("CLEANUP","owned callback volume and account position are inconsistent ownedNetLong="+std::to_string(netLong)
                +" baselineToday="+std::to_string(baselinePosition.today)+" currentToday="+std::to_string(currentPosition.today)
                +" baselineHistory="+std::to_string(baselinePosition.history)+" currentHistory="+std::to_string(currentPosition.history)
                +" baselineOther="+std::to_string(baselinePosition.other)+" currentOther="+std::to_string(currentPosition.other));restoreBlocked=true;break;
        }
        YDMarketData cleanupMarket{};
        if(!s.waitMarketData(instrument->InstrumentRef,actionTimeout,cleanupMarket)){
            l.error("CLEANUP","market data unavailable for position restore");restoreBlocked=true;break;
        }
        double closePrice=0;
        if(!cleanupClosePrice(instrument,cleanupMarket,closePrice)){
            l.error("CLEANUP","cannot construct legal position-restoring close price");restoreBlocked=true;break;
        }
        const int closeVolume=std::min(netLong,instrument->MaxLimitOrderVolume);
        if(closeVolume<instrument->MinLimitOrderVolume){l.error("CLEANUP","residual position is below minimum close volume");restoreBlocked=true;break;}
        const int closeRef=s.sendLimitOrder(instrument,YD_D_Sell,closeOffset(instrument),closePrice,closeVolume);
        if(closeRef<0){l.error("CLEANUP","position-restoring close was not submitted");restoreBlocked=true;break;}
        tickets.push_back({"RESIDUAL_POSITION_CLOSE",attempt,closeRef,YD_D_Sell,closeOffset(instrument),closePrice,closeVolume});
        l.warn("CLEANUP","account="+accountId+" event=RESIDUAL_POSITION_CLOSE_SUBMITTED instrument="+requestedInstrument+" orderRef="+std::to_string(closeRef)+" volume="+std::to_string(closeVolume));
        const OwnedOrderSettleResult close=waitCleanupClose(s,instrument,closeRef,actionTimeout,l);
        if(!close.settled){restoreBlocked=true;break;}
        if(close.finalOrder.ErrorNo!=0){l.error("CLEANUP","position-restoring close was rejected errorNo="+std::to_string(close.finalOrder.ErrorNo));restoreBlocked=true;break;}
    }

    OrderStreamSnapshot quietSnapshot;
    const bool callbackStreamQuiet=s.waitOrderActivityQuiet(callbackQuietMilliseconds,actionTimeout,quietSnapshot);
    int quietOwnedNetLong=0;
    const bool quietOwnedNetKnown=ownedNetLong(quietSnapshot,tickets,quietOwnedNetLong);
    std::int64_t quietTotalTradeVolume=0;
    const bool quietTradeVolumeConsistent=ownedTradeVolumeConsistent(quietSnapshot,tickets,tickets.size(),quietTotalTradeVolume);
    const bool quietNoWorkingOrders=allOwnedOrdersSettled(quietSnapshot,tickets);
    LongPositionSnapshot finalPosition;
    const bool finalPositionKnown=queryLongSpeculationPosition(s.extendedApi(),ydAccount,instrument,finalPosition);
    const bool positionRestored=finalPositionKnown&&sameLongPosition(baselinePosition,finalPosition,instrument);
    if(finalPositionKnown){
        l.info("POSITION_VERIFY","account="+accountId+" snapshotTime="+timestampText()+" instrument="+requestedInstrument
            +" baselineToday="+std::to_string(baselinePosition.today)+" finalToday="+std::to_string(finalPosition.today)
            +" baselineHistory="+std::to_string(baselinePosition.history)+" finalHistory="+std::to_string(finalPosition.history)
            +" baselineOther="+std::to_string(baselinePosition.other)+" finalOther="+std::to_string(finalPosition.other)
            +" baselineTotal="+std::to_string(baselinePosition.total())+" finalTotal="+std::to_string(finalPosition.total())
            +" quantityRestored="+(positionRestored?std::string("true"):std::string("false")));
    }
    const bool safeBeforeStop=!restoreBlocked&&callbackStreamQuiet&&quietOwnedNetKnown&&quietTradeVolumeConsistent
        &&quietOwnedNetLong==0&&quietNoWorkingOrders&&positionRestored;
    OrderStreamSnapshot finalSnapshot=quietSnapshot;
    bool streamStableThroughStop=false;
    if(safeBeforeStop)streamStableThroughStop=s.stopIfOrderStreamUnchanged(quietSnapshot.activityGeneration,quietSnapshot.sessionGeneration,finalSnapshot);
    if(!finalSnapshot.destroyed){s.stop();finalSnapshot=s.orderStreamSnapshot();}
    int finalOwnedNetLong=0;
    const bool ownedNetKnown=ownedNetLong(finalSnapshot,tickets,finalOwnedNetLong);
    std::int64_t totalTradeVolume=0;
    const bool allTradeVolumeConsistent=ownedTradeVolumeConsistent(finalSnapshot,tickets,tickets.size(),totalTradeVolume);
    std::int64_t unexpectedTradeVolume=0;
    const bool triggerTradeVolumeConsistent=ownedTradeVolumeConsistent(finalSnapshot,tickets,triggerTicketCount,unexpectedTradeVolume);
    const bool tradeVolumeConsistent=allTradeVolumeConsistent&&triggerTradeVolumeConsistent;
    const bool noWorkingOrders=allOwnedOrdersSettled(finalSnapshot,tickets);
    bool triggerStatesKnown=triggerTicketCount>0&&triggerTicketCount<=tickets.size();
    bool triggerFinalStatesConsistent=triggerStatesKnown;
    int finalTriggerErrorCount=0;
    int finalTriggerErrorNo=0;
    int finalTriggerErrorRef=0;
    for(std::size_t index=0;index<triggerTicketCount&&index<tickets.size();++index){
        const LiveTicket& ticket=tickets[index];
        const auto found=finalSnapshot.orders.find(ticket.orderRef);
        if(found==finalSnapshot.orders.end()){triggerStatesKnown=false;triggerFinalStatesConsistent=false;continue;}
        const YDOrder& order=found->second;
        const auto trade=finalSnapshot.tradeVolumeByOrderRef.find(ticket.orderRef);
        const std::int64_t callbackVolume=trade==finalSnapshot.tradeVolumeByOrderRef.end()?0:trade->second;
        const bool basicFieldsMatch=order.OrderRef==ticket.orderRef&&order.Direction==ticket.direction&&order.OffsetFlag==ticket.offset
            &&order.OrderVolume==ticket.volume&&std::isfinite(order.Price)
            &&std::fabs(order.Price-ticket.orderPrice)<=std::max(1e-9,instrument->Tick*1e-6);
        if(order.ErrorNo!=0){
            ++finalTriggerErrorCount;finalTriggerErrorNo=order.ErrorNo;finalTriggerErrorRef=ticket.orderRef;
            if(!basicFieldsMatch||order.TradeVolume!=0||callbackVolume!=0||!orderHasTerminalEvidence(order))triggerFinalStatesConsistent=false;
        }else if(cumulativeFundsMode){
            if(!basicFieldsMatch||order.TradeVolume!=0||callbackVolume!=0){triggerFinalStatesConsistent=false;continue;}
            std::string mismatch;
            if(order.OrderStatus!=YD_OS_Canceled||!orderMatchesTicket(ticket,order,instrument->Tick,mismatch))triggerFinalStatesConsistent=false;
        }else if(!basicFieldsMatch||order.TradeVolume!=0||callbackVolume!=0){
            triggerFinalStatesConsistent=false;
        }
    }
    const bool finalErrorConsistent=triggerStatesKnown&&finalTriggerErrorCount==1&&finalTriggerErrorRef==errorOrderRef
        &&expected.count(finalTriggerErrorNo)>0
        &&(!cumulativeFundsMode||(finalTriggerErrorNo==YD_ERROR_NoMoneyToOpen
            &&triggerTicketCount>0&&finalTriggerErrorRef==tickets[triggerTicketCount-1].orderRef));
    triggerFinalStatesConsistent=triggerFinalStatesConsistent&&finalErrorConsistent;
    const int reportedReceivedErrors=triggerStatesKnown?finalTriggerErrorCount:receivedErrors;
    const int reportedErrorNo=triggerStatesKnown&&finalTriggerErrorCount>0?finalTriggerErrorNo:actualErrorNo;
    const int reportedErrorRef=triggerStatesKnown&&finalTriggerErrorCount>0?finalTriggerErrorRef:errorOrderRef;
    const bool initialCancelCoverage=!cumulativeFundsMode||(measurementWorkingRefs.size()==static_cast<std::size_t>(peakWorkingOrders)
        &&initialCancelRequestedRefs==measurementWorkingRefs);
    std::ostringstream unresolved;
    bool firstUnresolved=true;
    for(const LiveTicket& ticket:tickets){
        const auto order=finalSnapshot.orders.find(ticket.orderRef);
        if(order!=finalSnapshot.orders.end()&&orderHasTerminalEvidence(order->second))continue;
        if(!firstUnresolved)unresolved<<',';
        unresolved<<ticket.orderRef;firstUnresolved=false;
    }
    if(firstUnresolved)unresolved<<"none";
    const OrderActivitySnapshot activity=activityDelta(finalSnapshot.activity,activityBefore);
    const OrderActivitySnapshot triggerActivity=activityDelta(cleanupStart,activityBefore);
    const OrderActivitySnapshot cleanupActivity=activityDelta(finalSnapshot.activity,cleanupStart);
    const bool cleanupRestored=safeBeforeStop&&streamStableThroughStop&&ownedNetKnown&&finalOwnedNetLong==0
        &&tradeVolumeConsistent&&noWorkingOrders&&positionRestored&&activity.failedCancelCallbacks==0&&activity.callbackValidationFailures==0;
    const bool triggerRequestsConsistent=triggerOrderAttempts>=1
        &&triggerActivity.orderApiRequests==static_cast<std::uint64_t>(triggerOrderAttempts)
        &&triggerActivity.orderRequestsSubmitted==static_cast<std::uint64_t>(triggerOrderAttempts);
    const bool cumulativeCountsConsistent=!cumulativeFundsMode||(triggerOrderAttempts>=2&&reportedReceivedErrors==1
        &&peakWorkingOrders==triggerOrderAttempts-1
        &&triggerActivity.uniqueAcceptedOrders==static_cast<std::uint64_t>(peakWorkingOrders)
        &&triggerActivity.cancelApiRequests==0&&triggerActivity.cancelRequestsSubmitted==0
        &&cleanupActivity.orderApiRequests==0
        &&activity.cancelApiRequests==static_cast<std::uint64_t>(peakWorkingOrders)
        &&activity.cancelRequestsSubmitted==static_cast<std::uint64_t>(peakWorkingOrders)
        &&activity.confirmedCancellations==static_cast<std::uint64_t>(peakWorkingOrders)
        &&initialCancelCoverage);
    const bool monitoringSucceeded=expectedErrorReceived&&cleanupRestored&&triggerRequestsConsistent&&cumulativeCountsConsistent
        &&triggerFinalStatesConsistent&&unexpectedTradeVolume==0;
    const std::string statistics="account="+accountId+" event=ORDER_ERROR_STATISTICS"
        +" receivedErrors="+std::to_string(reportedReceivedErrors)
        +" source=notifyOrder lastErrorNo="+std::to_string(reportedErrorNo)+" lastErrorName="+ydErrorName(reportedErrorNo)
        +" errorOrderRef="+std::to_string(reportedErrorRef)
        +" triggerOrderAttempts="+std::to_string(triggerOrderAttempts)
        +" peakWorkingOrders="+std::to_string(peakWorkingOrders)
        +" maxOrderAttempts="+std::to_string(maxTriggerOrders)
        +" volumePerOrder="+std::to_string(volume)
        +" orderIntervalMilliseconds="+std::to_string(fundsOrderIntervalMilliseconds)
        +" orderApiRequests="+std::to_string(triggerActivity.orderApiRequests)
        +" orderRequestsSubmitted="+std::to_string(triggerActivity.orderRequestsSubmitted)
        +" uniqueAcceptedOrders="+std::to_string(triggerActivity.uniqueAcceptedOrders)
        +" cancelApiRequests="+std::to_string(activity.cancelApiRequests)
        +" cancelRequestsSubmitted="+std::to_string(activity.cancelRequestsSubmitted)
        +" confirmedCancellations="+std::to_string(activity.confirmedCancellations)
        +" failedCancelCallbacks="+std::to_string(activity.failedCancelCallbacks)
        +" callbackValidationFailures="+std::to_string(activity.callbackValidationFailures)
        +" cleanupOrderApiRequests="+std::to_string(cleanupActivity.orderApiRequests)
        +" cleanupCancelApiRequests="+std::to_string(cleanupActivity.cancelApiRequests)
        +" unexpectedTradeVolume="+(triggerTradeVolumeConsistent?std::to_string(unexpectedTradeVolume):std::string("UNKNOWN"))
        +" tradeVolumeConsistent="+(tradeVolumeConsistent?std::string("true"):std::string("false"))
        +" triggerFinalStatesConsistent="+(triggerFinalStatesConsistent?std::string("true"):std::string("false"))
        +" initialCancelCoverage="+(initialCancelCoverage?std::string("true"):std::string("false"))
        +" ownedNetLong="+(ownedNetKnown?std::to_string(finalOwnedNetLong):std::string("UNKNOWN"))
        +" noWorkingOrders="+(noWorkingOrders?std::string("true"):std::string("false"))
        +" positionConsistent="+(positionRestored?std::string("true"):std::string("false"))
        +" positionRestored="+(positionRestored?std::string("true"):std::string("false"))
        +" callbackStreamQuiet="+(callbackStreamQuiet?std::string("true"):std::string("false"))
        +" streamStableThroughStop="+(streamStableThroughStop?std::string("true"):std::string("false"))
        +" cleanupRestored="+(cleanupRestored?std::string("true"):std::string("false"))
        +" unresolvedRefs="+unresolved.str()
        +" accountStateNormal="+(cleanupRestored?std::string("true"):std::string("false"));
    if(monitoringSucceeded){l.info("ERROR_MONITOR",statistics);r.observe("order error monitor snapshot",statistics);}
    else{
        l.error("ERROR_MONITOR",statistics);
        if(!cleanupRestored)l.error("ALERT","MANUAL ACTION REQUIRED account="+accountId+" instrument="+requestedInstrument+"; verify working orders and positions in the broker terminal");
        r.fail("order error monitor consistency",statistics);
    }
},true,true);}
}

int runTest081InsufficientFunds(const RunOptions& o){return runErrorMessageCase(o,"2.8.1_insufficient_funds",ErrorMessageMode::InsufficientFunds);}
int runTest082NoPosition(const RunOptions& o){return runErrorMessageCase(o,"2.8.2_no_position",ErrorMessageMode::NoPosition);}
int runTest083MarketState(const RunOptions& o){return runErrorMessageCase(o,"2.8.3_market_state",ErrorMessageMode::MarketState);}
int runTest08ErrorMessage(const RunOptions& o){bool known=false;const ErrorMessageMode mode=configuredErrorMode(o,known);if(!known){std::cerr<<"ERROR: use --case no-position|insufficient-funds|market-state"<<std::endl;return 2;}return runErrorMessageCase(o,"2.8_error_message",mode);}

int runTest14ManualPriceOrder(const RunOptions& o){return runEach(o,"14_manual_price_order",[&](const Account&a,const Config&c,Logger&l,TestResult&r){
    // This test submits one real order at a fixed price without any market-data
    // subscription. It exists for environments that have no market feed.
    if(!o.live){r.skip("manual fixed-price order","requires --live; no order sent");return;}
    const int sessionTimeout=std::max(1,c.getInt("Trade.SessionTimeoutSeconds",120));
    const int actionTimeout=std::max(1,c.getInt("Trade.ActionTimeoutSeconds",30));
    YdSession s(o.ydConfig,a.username,a.password,l,true,monitorThresholds(c),o.live);
    if(!readySessionObserved(s,r,sessionTimeout,a.username))return;

    const std::string requestedInstrument=instID(o,c);
    const YDInstrument* instrument=s.instrument(requestedInstrument);
    if(!instrument){r.fail("instrument exists","instrument="+requestedInstrument+"; no order sent");return;}
    const std::string instrumentId=instrument->InstrumentID;
    const YDAccount* ydAccount=s.api()?s.api()->getMyAccount():nullptr;
    if(!ydAccount){r.fail("account identity","getMyAccount returned null; no order sent");return;}
    const std::string accountId=ydAccount->AccountID[0]?std::string(ydAccount->AccountID):a.username;

    std::string directionText=o.direction;for(auto& ch:directionText)ch=static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::string offsetText=o.offset;for(auto& ch:offsetText)ch=static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const int direction=(directionText=="sell"||directionText=="s")?YD_D_Sell:YD_D_Buy;
    int offset=YD_OF_Open;
    if(offsetText=="close"||offsetText=="c")offset=closeOffset(instrument);
    else if(offsetText=="closetoday"||offsetText=="closetd"||offsetText=="today")offset=YD_OF_CloseToday;

    const int volume=o.orderVolume;
    if(volume<instrument->MinLimitOrderVolume||volume>instrument->MaxLimitOrderVolume){
        r.fail("manual order preflight","volume="+std::to_string(volume)+" allowed=["+std::to_string(instrument->MinLimitOrderVolume)+","+std::to_string(instrument->MaxLimitOrderVolume)+"]; no order sent");return;
    }
    if(!usablePrice(instrument->Tick)){
        r.fail("manual order preflight","instrument tick is invalid; no order sent");return;
    }
    const double price=legalPrice(o.price,instrument->Tick);
    if(!usablePrice(price)){
        r.fail("manual order preflight","price="+snapshotNumber(o.price)+" is not a positive legal price (tick="+snapshotNumber(instrument->Tick)+"); no order sent");return;
    }

    l.info("SYSTEM","account="+accountId+" event=MANUAL_ORDER source=FIXED_PRICE instrument="+instrumentId
        +" direction="+directionName(direction)+" offset="+offsetName(offset)
        +" price="+snapshotNumber(price)+" volume="+std::to_string(volume)
        +" marketDataRequired=false keepWorking="+(o.keepWorking?std::string("true"):std::string("false")));

    const int orderRef=s.sendLimitOrder(instrument,direction,offset,price,volume);
    if(orderRef<0){r.fail("manual fixed-price order","insertOrder did not return an OrderRef; check the broker terminal");return;}
    l.info("TRADE","account="+accountId+" event=MANUAL_ORDER_SUBMITTED instrument="+instrumentId+" orderRef="+std::to_string(orderRef));

    YDOrder state{};
    if(!s.waitOrder(orderRef,actionTimeout,[](const YDOrder& o){return o.ErrorNo!=0||terminal(o)||o.OrderStatus==YD_OS_Accepted||o.OrderStatus==YD_OS_Queuing;},state)){
        r.fail("manual fixed-price order","no owned order callback before timeout orderRef="+std::to_string(orderRef)+"; check the broker terminal");return;
    }

    if(state.ErrorNo!=0){
        l.info("ORDER_RESULT","account="+accountId+" instrument="+instrumentId+" orderRef="+std::to_string(orderRef)
            +" direction="+directionName(state.Direction)+" offset="+offsetName(state.OffsetFlag)
            +" price="+snapshotNumber(state.Price)+" volume="+std::to_string(state.OrderVolume)
            +" status="+orderStatusName(state.OrderStatus)+" errorNo="+std::to_string(state.ErrorNo));
        r.fail("manual fixed-price order","cabinet rejected orderRef="+std::to_string(orderRef)+" errorNo="+std::to_string(state.ErrorNo));return;
    }

    if(o.keepWorking){
        l.info("ORDER_RESULT","account="+accountId+" instrument="+instrumentId+" orderRef="+std::to_string(orderRef)
            +" direction="+directionName(state.Direction)+" offset="+offsetName(state.OffsetFlag)
            +" price="+snapshotNumber(state.Price)+" volume="+std::to_string(state.OrderVolume)
            +" status="+orderStatusName(state.OrderStatus)+" result=LEFT_WORKING");
        r.observe("manual fixed-price order","orderRef="+std::to_string(orderRef)+" leftWorking=true; cancel it manually in the broker terminal if needed");
        return;
    }

    if(!terminal(state)){
        if(!s.waitOrder(orderRef,actionTimeout,[](const YDOrder& o){return cancelableOrTerminal(o);},state)){
            r.fail("manual fixed-price order","order did not become cancelable/terminal orderRef="+std::to_string(orderRef)+"; check the broker terminal");return;
        }
    }

    if(terminal(state)){
        l.info("ORDER_RESULT","account="+accountId+" instrument="+instrumentId+" orderRef="+std::to_string(orderRef)
            +" direction="+directionName(state.Direction)+" offset="+offsetName(state.OffsetFlag)
            +" price="+snapshotNumber(state.Price)+" volume="+std::to_string(state.OrderVolume)
            +" status="+orderStatusName(state.OrderStatus)+" traded="+std::to_string(state.TradeVolume)+"/"+std::to_string(state.OrderVolume));
        if(state.OrderStatus==YD_OS_AllTraded)
            r.observe("manual fixed-price order","orderRef="+std::to_string(orderRef)+" filled="+std::to_string(state.TradeVolume)+"; verify position in the broker terminal");
        else
            r.observe("manual fixed-price order","orderRef="+std::to_string(orderRef)+" terminal status="+orderStatusName(state.OrderStatus));
        return;
    }

    if(!s.cancelOrder(instrument,state)){
        r.fail("manual fixed-price order","cancelOrder returned false orderRef="+std::to_string(orderRef)+"; check the broker terminal");return;
    }
    YDOrder finalOrder{};
    const int cancelResult=s.waitCancelTerminal(orderRef,actionTimeout,finalOrder);
    if(cancelResult!=0){
        r.fail("manual fixed-price order","cancel did not reach terminal orderRef="+std::to_string(orderRef)+" result="+std::to_string(cancelResult)+"; check the broker terminal");return;
    }
    l.info("ORDER_RESULT","account="+accountId+" instrument="+instrumentId+" orderRef="+std::to_string(orderRef)
        +" direction="+directionName(finalOrder.Direction)+" offset="+offsetName(finalOrder.OffsetFlag)
        +" price="+snapshotNumber(finalOrder.Price)+" volume="+std::to_string(finalOrder.OrderVolume)
        +" status="+orderStatusName(finalOrder.OrderStatus)+" traded="+std::to_string(finalOrder.TradeVolume)+"/"+std::to_string(finalOrder.OrderVolume));
    if(finalOrder.OrderStatus==YD_OS_Canceled&&finalOrder.TradeVolume==0)
        r.pass("manual fixed-price order","orderRef="+std::to_string(orderRef)+" canceled; no residual position");
    else
        r.fail("manual fixed-price order","unexpected final status orderRef="+std::to_string(orderRef)+" status="+orderStatusName(finalOrder.OrderStatus)+" traded="+std::to_string(finalOrder.TradeVolume));
});}

int runTest15ManualBasicTrade(const RunOptions& o){return runEach(o,"15_manual_basic_trade",[&](const Account&a,const Config&c,Logger&l,TestResult&r){
    // No-market-data variant of the 3.2.2 basic-trade flow: 2 buy-open fills,
    // 2 buy orders that queue and are canceled, and 2 sell-close fills that
    // flatten exactly this test's newly opened long position.
    if(!o.live){r.skip("manual basic trading workflow","requires --live; no order sent");return;}
    const int sessionTimeout=std::max(1,c.getInt("Trade.SessionTimeoutSeconds",120));
    const int actionTimeout=std::max(1,c.getInt("Trade.ActionTimeoutSeconds",30));
    YdSession s(o.ydConfig,a.username,a.password,l,true,monitorThresholds(c),o.live);
    if(!readySessionObserved(s,r,sessionTimeout,a.username))return;

    const std::string requestedInstrument=instID(o,c);
    const YDInstrument* instrument=s.instrument(requestedInstrument);
    if(!instrument){r.fail("instrument exists","instrument="+requestedInstrument+"; no order sent");return;}
    const std::string instrumentId=instrument->InstrumentID;
    const YDAccount* ydAccount=s.api()?s.api()->getMyAccount():nullptr;
    if(!ydAccount){r.fail("account identity","getMyAccount returned null; no order sent");return;}
    const std::string accountId=ydAccount->AccountID[0]?std::string(ydAccount->AccountID):a.username;
    const YDAccountInstrumentInfo* accountInstrument=s.api()->getAccountInstrumentInfo(instrument);
    if(ydAccount->TradingRight!=YD_TR_Allow||!accountInstrument||accountInstrument->TradingRight!=YD_TR_Allow){
        r.fail("manual basic trade preflight","account/instrument trading right is not ALLOW; no order sent");return;
    }
    if(!usablePrice(instrument->Tick)){r.fail("manual basic trade preflight","invalid instrument tick; no order sent");return;}
    const int volume=o.orderVolume;
    if(volume<instrument->MinLimitOrderVolume||volume>instrument->MaxLimitOrderVolume){
        r.fail("manual basic trade preflight","volume="+std::to_string(volume)+" allowed=["+std::to_string(instrument->MinLimitOrderVolume)+","+std::to_string(instrument->MaxLimitOrderVolume)+"]; no order sent");return;
    }
    const double openPrice=legalPrice(o.openPrice,instrument->Tick);
    const double passivePrice=legalPrice(o.passivePrice,instrument->Tick);
    const double closePrice=legalPrice(o.closePrice,instrument->Tick);
    if(!usablePrice(openPrice)||!usablePrice(passivePrice)||!usablePrice(closePrice)){
        r.fail("manual basic trade preflight","open/passive/close price must be positive legal prices (tick="+snapshotNumber(instrument->Tick)+"); no order sent");return;
    }

    LongPositionSnapshot baselinePosition;
    if(!queryLongSpeculationPosition(s.extendedApi(),ydAccount,instrument,baselinePosition)){
        r.fail("manual basic trade preflight","cannot query baseline long position; no order sent");return;
    }
    l.info("POSITION_BASELINE","account="+accountId+" snapshotTime="+timestampText()+" instrument="+instrumentId+" direction=LONG hedge=SPECULATION today="+std::to_string(baselinePosition.today)+" history="+std::to_string(baselinePosition.history)+" other="+std::to_string(baselinePosition.other)+" total="+std::to_string(baselinePosition.total()));

    l.info("SYSTEM","account="+accountId+" event=MANUAL_BASIC_TRADE instrument="+instrumentId
        +" volume="+std::to_string(volume)+" openPrice="+snapshotNumber(openPrice)
        +" passivePrice="+snapshotNumber(passivePrice)+" closePrice="+snapshotNumber(closePrice)
        +" marketDataRequired=false");

    struct Ticket{int ref=0;int direction=YD_D_Buy;int offset=YD_OF_Open;double price=0;int volume=0;};
    std::vector<Ticket> tickets;
    int openFills=0,cancellations=0,closeFills=0;
    int passiveFilledVolume=0;
    std::string firstFailure;
    auto fail=[&](const std::string& text){if(firstFailure.empty())firstFailure=text;l.error("ORDER_RESULT",text);};

    // Phase 1: two buy-open fills.
    for(int seq=1;seq<=2;++seq){
        const int ref=s.sendLimitOrder(instrument,YD_D_Buy,YD_OF_Open,openPrice,volume);
        if(ref<0){fail("OPEN #"+std::to_string(seq)+" insertOrder returned false");break;}
        tickets.push_back({ref,YD_D_Buy,YD_OF_Open,openPrice,volume});
        YDOrder od{};
        if(!s.waitOrder(ref,actionTimeout,terminal,od)){fail("OPEN #"+std::to_string(seq)+" terminal callback timeout ref="+std::to_string(ref));break;}
        if(od.ErrorNo!=0||od.OrderStatus!=YD_OS_AllTraded||od.TradeVolume!=volume){fail("OPEN #"+std::to_string(seq)+" unexpected state ref="+std::to_string(ref)+" status="+orderStatusName(od.OrderStatus)+" errorNo="+std::to_string(od.ErrorNo)+" traded="+std::to_string(od.TradeVolume)+"/"+std::to_string(volume));break;}
        ++openFills;
        l.info("ORDER_RESULT","OPEN #"+std::to_string(seq)+" | account="+accountId+" | instrument="+instrumentId+" | direction=BUY | offset=OPEN | orderPrice="+snapshotNumber(od.Price)+" | volume="+std::to_string(od.OrderVolume)+" | filled="+std::to_string(od.TradeVolume)+"/"+std::to_string(od.OrderVolume)+" | orderRef="+std::to_string(ref)+" | status="+orderStatusName(od.OrderStatus)+" | errorNo=0 | resultTime="+timestampText());
    }

    // Phase 2: two buy orders that queue, then cancel each.
    if(openFills==2){
        for(int seq=1;seq<=2;++seq){
            const int ref=s.sendLimitOrder(instrument,YD_D_Buy,YD_OF_Open,passivePrice,volume);
            if(ref<0){fail("CANCEL #"+std::to_string(seq)+" insertOrder returned false");break;}
            tickets.push_back({ref,YD_D_Buy,YD_OF_Open,passivePrice,volume});
            YDOrder working{};
            if(!s.waitOrder(ref,actionTimeout,cancelableOrTerminal,working)){fail("CANCEL #"+std::to_string(seq)+" did not become cancelable/terminal ref="+std::to_string(ref));break;}
            if(working.ErrorNo!=0){fail("CANCEL #"+std::to_string(seq)+" rejected ref="+std::to_string(ref)+" errorNo="+std::to_string(working.ErrorNo));break;}
            if(terminal(working)){
                if(working.OrderStatus==YD_OS_AllTraded){
                    passiveFilledVolume+=working.TradeVolume;
                    l.warn("ORDER_RESULT","CANCEL #"+std::to_string(seq)+" passive order unexpectedly filled ref="+std::to_string(ref)+" traded="+std::to_string(working.TradeVolume)+"/"+std::to_string(working.OrderVolume)+"; will be flattened in the close phase");
                    continue;
                }
                ++cancellations;continue;
            }
            if(working.OrderStatus!=YD_OS_Queuing){fail("CANCEL #"+std::to_string(seq)+" did not remain queuing ref="+std::to_string(ref)+" status="+orderStatusName(working.OrderStatus)+" traded="+std::to_string(working.TradeVolume));break;}
            if(!hasAssignedSystemOrderId(working.OrderSysID,working.LongOrderSysID)){fail("CANCEL #"+std::to_string(seq)+" queued order has no system order ID ref="+std::to_string(ref));break;}
            if(!s.cancelOrder(instrument,working)){fail("CANCEL #"+std::to_string(seq)+" cancelOrder returned false ref="+std::to_string(ref));break;}
            YDOrder final{};
            const int cr=s.waitCancelTerminal(ref,actionTimeout,final);
            if(cr!=0){fail("CANCEL #"+std::to_string(seq)+" cancel did not reach terminal ref="+std::to_string(ref)+" result="+std::to_string(cr));break;}
            if(final.OrderStatus!=YD_OS_Canceled||final.TradeVolume!=0){fail("CANCEL #"+std::to_string(seq)+" unexpected final ref="+std::to_string(ref)+" status="+orderStatusName(final.OrderStatus)+" traded="+std::to_string(final.TradeVolume));break;}
            ++cancellations;
            l.info("ORDER_RESULT","CANCEL #"+std::to_string(seq)+" | account="+accountId+" | instrument="+instrumentId+" | direction=BUY | offset=OPEN | orderPrice="+snapshotNumber(final.Price)+" | volume="+std::to_string(final.OrderVolume)+" | filled=0/"+std::to_string(final.OrderVolume)+" | orderRef="+std::to_string(ref)+" | status="+orderStatusName(final.OrderStatus)+" | errorNo=0 | resultTime="+timestampText());
        }
    }

    // Phase 3: sell-close exactly this test's net long (2 opens plus any surprise passive fill).
    int remainingLongs=openFills*volume+passiveFilledVolume;
    int closeSeq=0;
    while(remainingLongs>0){
        const int closeVol=std::min(remainingLongs,volume);
        const int offset=closeOffset(instrument);
        const int ref=s.sendLimitOrder(instrument,YD_D_Sell,offset,closePrice,closeVol);
        ++closeSeq;
        if(ref<0){fail("CLOSE #"+std::to_string(closeSeq)+" insertOrder returned false");break;}
        tickets.push_back({ref,YD_D_Sell,offset,closePrice,closeVol});
        YDOrder od{};
        if(!s.waitOrder(ref,actionTimeout,terminal,od)){fail("CLOSE #"+std::to_string(closeSeq)+" terminal callback timeout ref="+std::to_string(ref));break;}
        if(od.ErrorNo!=0){fail("CLOSE #"+std::to_string(closeSeq)+" rejected ref="+std::to_string(ref)+" errorNo="+std::to_string(od.ErrorNo));break;}
        if(od.OrderStatus==YD_OS_AllTraded&&od.TradeVolume==closeVol){
            ++closeFills;remainingLongs-=od.TradeVolume;
            l.info("ORDER_RESULT","CLOSE #"+std::to_string(closeSeq)+" | account="+accountId+" | instrument="+instrumentId+" | direction=SELL | offset="+offsetName(offset)+" | orderPrice="+snapshotNumber(od.Price)+" | volume="+std::to_string(od.OrderVolume)+" | filled="+std::to_string(od.TradeVolume)+"/"+std::to_string(od.OrderVolume)+" | orderRef="+std::to_string(ref)+" | status="+orderStatusName(od.OrderStatus)+" | errorNo=0 | resultTime="+timestampText());
        }else{
            fail("CLOSE #"+std::to_string(closeSeq)+" did not fully fill ref="+std::to_string(ref)+" status="+orderStatusName(od.OrderStatus)+" traded="+std::to_string(od.TradeVolume)+"/"+std::to_string(closeVol));
            if(!terminal(od)&&hasAssignedSystemOrderId(od.OrderSysID,od.LongOrderSysID)&&s.cancelOrder(instrument,od)){YDOrder fo{};s.waitCancelTerminal(ref,actionTimeout,fo);}
            break;
        }
    }

    // Defensive cleanup: cancel any of this test's orders still working.
    bool anyWorking=false;
    const OrderStreamSnapshot snap=s.orderStreamSnapshot();
    for(const Ticket& t:tickets){
        const auto it=snap.orders.find(t.ref);
        if(it==snap.orders.end())continue;
        if(terminal(it->second))continue;
        anyWorking=true;
        if(hasAssignedSystemOrderId(it->second.OrderSysID,it->second.LongOrderSysID)&&s.cancelOrder(instrument,it->second)){
            YDOrder fo{};s.waitCancelTerminal(t.ref,actionTimeout,fo);
        }
    }

    LongPositionSnapshot finalPosition;
    const bool positionKnown=queryLongSpeculationPosition(s.extendedApi(),ydAccount,instrument,finalPosition);
    const bool positionRestored=positionKnown&&finalPosition.total()==baselinePosition.total();
    l.info("POSITION_VERIFY","account="+accountId+" snapshotTime="+timestampText()+" instrument="+instrumentId
        +" baselineToday="+std::to_string(baselinePosition.today)+" finalToday="+std::to_string(finalPosition.today)
        +" baselineHistory="+std::to_string(baselinePosition.history)+" finalHistory="+std::to_string(finalPosition.history)
        +" baselineOther="+std::to_string(baselinePosition.other)+" finalOther="+std::to_string(finalPosition.other)
        +" baselineTotal="+std::to_string(baselinePosition.total())+" finalTotal="+std::to_string(finalPosition.total())
        +" quantityRestored="+(positionRestored?std::string("true"):std::string("false")));

    const bool succeeded=openFills==2&&cancellations==2&&remainingLongs==0&&positionRestored&&!anyWorking;
    if(succeeded)r.pass("manual basic trading workflow","account="+accountId+" instrument="+instrumentId+" openFills=2 cancellations=2 closeFills="+std::to_string(closeFills)+" baselineTotal="+std::to_string(baselinePosition.total())+" finalTotal="+std::to_string(finalPosition.total())+" noWorkingOrders=true");
    else{
        if(!positionRestored)l.error("ALERT","MANUAL ACTION REQUIRED account="+accountId+" instrument="+instrumentId+"; verify working orders and positions in the broker terminal");
        r.fail("manual basic trading workflow","account="+accountId+" instrument="+instrumentId+" openFills="+std::to_string(openFills)+"/2 cancellations="+std::to_string(cancellations)+"/2 closeFills="+std::to_string(closeFills)+" remainingLongs="+std::to_string(remainingLongs)+" positionRestored="+(positionRestored?std::string("true"):std::string("false"))+" noWorkingOrders="+(!anyWorking?std::string("true"):std::string("false"))+(firstFailure.empty()?std::string():(" firstFailure="+firstFailure)));
    }
},true,false);}

int runTest16ManualOrderCount(const RunOptions& o){return runEach(o,"16_manual_order_count",[&](const Account&a,const Config&c,Logger&l,TestResult&r){
    // No-market-data variant of the 3.3.1.2 order/cancel count monitoring:
    // submit N passive buy orders at a fixed price, cancel each after it queues,
    // and report the same COUNT_RESULT counters without any market subscription.
    if(!o.live){r.skip("manual order/cancel count monitoring","requires --live; no order sent");return;}
    const int sessionTimeout=std::max(1,c.getInt("Trade.SessionTimeoutSeconds",120));
    const int actionTimeout=std::max(1,c.getInt("Trade.ActionTimeoutSeconds",30));
    YdSession s(o.ydConfig,a.username,a.password,l,true,monitorThresholds(c),o.live);
    if(!readySessionObserved(s,r,sessionTimeout,a.username))return;

    const std::string requestedInstrument=instID(o,c);
    const YDInstrument* instrument=s.instrument(requestedInstrument);
    if(!instrument){r.fail("instrument exists","instrument="+requestedInstrument+"; no order sent");return;}
    const std::string instrumentId=instrument->InstrumentID;
    const YDAccount* ydAccount=s.api()?s.api()->getMyAccount():nullptr;
    if(!ydAccount){r.fail("account identity","getMyAccount returned null; no order sent");return;}
    const std::string accountId=ydAccount->AccountID[0]?std::string(ydAccount->AccountID):a.username;
    const YDAccountInstrumentInfo* accountInstrument=s.api()->getAccountInstrumentInfo(instrument);
    if(ydAccount->TradingRight!=YD_TR_Allow||!accountInstrument||accountInstrument->TradingRight!=YD_TR_Allow){
        r.fail("manual order count preflight","account/instrument trading right is not ALLOW; no order sent");return;
    }
    if(!usablePrice(instrument->Tick)){r.fail("manual order count preflight","invalid instrument tick; no order sent");return;}
    const int volume=o.orderVolume;
    if(volume<instrument->MinLimitOrderVolume||volume>instrument->MaxLimitOrderVolume){
        r.fail("manual order count preflight","volume="+std::to_string(volume)+" allowed=["+std::to_string(instrument->MinLimitOrderVolume)+","+std::to_string(instrument->MaxLimitOrderVolume)+"]; no order sent");return;
    }
    const int requiredCount=std::max(1,o.count);
    const double price=legalPrice(o.countPrice,instrument->Tick);
    if(!usablePrice(price)){
        r.fail("manual order count preflight","count-price="+snapshotNumber(o.countPrice)+" is not a positive legal price (tick="+snapshotNumber(instrument->Tick)+"); no order sent");return;
    }

    l.info("SYSTEM","account="+accountId+" event=MANUAL_ORDER_COUNT instrument="+instrumentId
        +" volume="+std::to_string(volume)+" count="+std::to_string(requiredCount)
        +" price="+snapshotNumber(price)+" marketDataRequired=false");

    const OrderActivitySnapshot activityStart=s.orderActivity();
    int submittedCount=0,cancelledCount=0;
    std::vector<int> workingRefs;
    bool anyFailure=false;
    std::string firstFailure;
    auto fail=[&](const std::string& text){anyFailure=true;if(firstFailure.empty())firstFailure=text;l.error("ORDER_RESULT",text);};

    for(int seq=1;seq<=requiredCount;++seq){
        const int ref=s.sendLimitOrder(instrument,YD_D_Buy,YD_OF_Open,price,volume);
        if(ref<0){fail("ORDER #"+std::to_string(seq)+" insertOrder returned false");break;}
        workingRefs.push_back(ref);
        YDOrder working{};
        if(!s.waitOrder(ref,actionTimeout,cancelableOrTerminal,working)){fail("ORDER #"+std::to_string(seq)+" did not become cancelable/terminal ref="+std::to_string(ref));break;}
        if(working.ErrorNo!=0){fail("ORDER #"+std::to_string(seq)+" rejected ref="+std::to_string(ref)+" errorNo="+std::to_string(working.ErrorNo));break;}
        if(terminal(working)){
            if(working.OrderStatus==YD_OS_AllTraded){
                fail("ORDER #"+std::to_string(seq)+" unexpectedly filled ref="+std::to_string(ref)+" traded="+std::to_string(working.TradeVolume)+"/"+std::to_string(volume)+"; lower --count-price so the order queues instead");
            }
            break;
        }
        if(working.OrderStatus!=YD_OS_Queuing){fail("ORDER #"+std::to_string(seq)+" did not remain queuing ref="+std::to_string(ref)+" status="+orderStatusName(working.OrderStatus)+" traded="+std::to_string(working.TradeVolume));break;}
        if(!hasAssignedSystemOrderId(working.OrderSysID,working.LongOrderSysID)){fail("ORDER #"+std::to_string(seq)+" queued order has no system order ID ref="+std::to_string(ref));break;}
        if(!s.cancelOrder(instrument,working)){fail("ORDER #"+std::to_string(seq)+" cancelOrder returned false ref="+std::to_string(ref));break;}
        YDOrder final{};
        const int cr=s.waitCancelTerminal(ref,actionTimeout,final);
        if(cr!=0){fail("ORDER #"+std::to_string(seq)+" cancel did not reach terminal ref="+std::to_string(ref)+" result="+std::to_string(cr));break;}
        if(final.OrderStatus!=YD_OS_Canceled||final.TradeVolume!=0){fail("ORDER #"+std::to_string(seq)+" unexpected final ref="+std::to_string(ref)+" status="+orderStatusName(final.OrderStatus)+" traded="+std::to_string(final.TradeVolume));break;}
        ++cancelledCount;
        l.info("ORDER_RESULT","ORDER #"+std::to_string(seq)+"/"+std::to_string(requiredCount)+" | account="+accountId+" | instrument="+instrumentId+" | direction=BUY | offset=OPEN | orderPrice="+snapshotNumber(final.Price)+" | volume="+std::to_string(final.OrderVolume)+" | filled=0/"+std::to_string(final.OrderVolume)+" | orderRef="+std::to_string(ref)+" | status="+orderStatusName(final.OrderStatus)+" | errorNo=0 | resultTime="+timestampText());
    }

    // Defensive cleanup: cancel any of this test's orders still working.
    bool anyWorking=false;
    const OrderStreamSnapshot snap=s.orderStreamSnapshot();
    for(const int ref:workingRefs){
        const auto it=snap.orders.find(ref);
        if(it==snap.orders.end())continue;
        if(terminal(it->second))continue;
        anyWorking=true;
        if(hasAssignedSystemOrderId(it->second.OrderSysID,it->second.LongOrderSysID)&&s.cancelOrder(instrument,it->second)){
            YDOrder fo{};s.waitCancelTerminal(ref,actionTimeout,fo);
        }
    }

    const OrderActivitySnapshot activityEnd=s.orderActivity();
    const OrderActivitySnapshot measuredActivity=activityDelta(activityEnd,activityStart);
    const bool monitoringPass=!anyFailure&&cancelledCount==requiredCount
        &&measuredActivity.orderApiRequests==requiredCount
        &&measuredActivity.orderRequestsSubmitted==requiredCount
        &&measuredActivity.uniqueAcceptedOrders==requiredCount
        &&measuredActivity.cancelApiRequests==requiredCount
        &&measuredActivity.cancelRequestsSubmitted==requiredCount
        &&measuredActivity.confirmedCancellations==requiredCount
        &&measuredActivity.failedCancelCallbacks==0
        &&measuredActivity.callbackValidationFailures==0
        &&!anyWorking;

    std::ostringstream line;line<<"status="<<(monitoringPass?"PASS":"FAIL")<<" | account="<<accountId<<" | countTime="<<timestampText()<<" | instrument="<<instrumentId
        <<" | orderApiRequests="<<measuredActivity.orderApiRequests
        <<" | orderRequestsSubmitted="<<measuredActivity.orderRequestsSubmitted
        <<" | uniqueAcceptedOrders="<<measuredActivity.uniqueAcceptedOrders
        <<" | cancelApiRequests="<<measuredActivity.cancelApiRequests
        <<" | cancelRequestsSubmitted="<<measuredActivity.cancelRequestsSubmitted
        <<" | confirmedCancellations="<<measuredActivity.confirmedCancellations
        <<" | failedCancelCallbacks="<<measuredActivity.failedCancelCallbacks
        <<" | callbackValidationFailures="<<measuredActivity.callbackValidationFailures
        <<" | totalOrderApiRequests="<<measuredActivity.orderApiRequests
        <<" | totalAcceptedOrders="<<measuredActivity.uniqueAcceptedOrders
        <<" | totalCancelApiRequests="<<measuredActivity.cancelApiRequests
        <<" | totalConfirmedCancellations="<<measuredActivity.confirmedCancellations
        <<" | noWorkingOrders="<<(!anyWorking?std::string("true"):std::string("false"));
    if(monitoringPass)l.info("COUNT_RESULT",line.str());else l.error("COUNT_RESULT",line.str());

    if(monitoringPass)r.pass("manual order/cancel count monitoring","account="+accountId+" instrument="+instrumentId+" orderApiRequests="+std::to_string(measuredActivity.orderApiRequests)+" confirmedCancellations="+std::to_string(measuredActivity.confirmedCancellations));
    else r.fail("manual order/cancel count monitoring",line.str());
},true,false);}

int runTest09PauseTrade(const RunOptions& o){return runEach(o,"2.9_pause_trade",[&](const Account&a,const Config&c,Logger&l,TestResult&r){
    if(o.live){r.fail("trading control safety","test_09 does not accept --live; no order sent");return;}
    const int sessionTimeout=std::max(1,c.getInt("Trade.SessionTimeoutSeconds",120));
    YdSession s(o.ydConfig,a.username,a.password,l,false,monitorThresholds(c),false);
    if(!readySessionObserved(s,r,sessionTimeout,a.username))return;
    const std::string instrumentId=instID(o,c);
    const YDInstrument* instrument=s.instrument(instrumentId);
    if(!instrument){r.fail("trading control initialization","instrument="+instrumentId+" does not exist; no order sent");return;}
    const int volume=std::max(1,instrument->MinLimitOrderVolume);
    if(!usablePrice(instrument->Tick)||volume>instrument->MaxLimitOrderVolume){r.fail("trading control initialization","instrument has no valid limit-order price/volume; no order sent");return;}
    const double price=instrument->Tick*1000.0;
    const OrderActivitySnapshot activityStart=s.orderActivity();
    const TradingControlSnapshot controlStart=s.tradingControl();
    bool orderIntentObserved=false;
    bool controlConsistent=true;

    auto dispatchStrategyOrder=[&](){
        const OrderActivitySnapshot activityBefore=s.orderActivity();
        const TradingControlSnapshot controlBefore=s.tradingControl();
        l.info("TRADE","account="+a.username+" event=ORDER_INTENT source=STRATEGY instrument="+instrumentId+" direction=BUY offset=OPEN price="+snapshotNumber(price)+" volume="+std::to_string(volume));
        const int orderRef=s.sendLimitOrder(instrument,YD_D_Buy,YD_OF_Open,price,volume);
        const OrderActivitySnapshot activityAfter=s.orderActivity();
        const TradingControlSnapshot controlAfter=s.tradingControl();
        orderIntentObserved=true;
        const bool blocked=orderRef<0
            &&controlAfter.blockedOrderInstructions==controlBefore.blockedOrderInstructions+1
            &&activityAfter.orderApiRequests==activityBefore.orderApiRequests
            &&activityAfter.orderRequestsSubmitted==activityBefore.orderRequestsSubmitted;
        if(!blocked){
            controlConsistent=false;
            l.error("ALERT","account="+a.username+" event=TRADING_CONTROL_INCONSISTENT instrument="+instrumentId+" orderRef="+std::to_string(orderRef));
        }
    };

    auto logStatus=[&](){
        const TradingControlSnapshot control=s.tradingControl();
        l.info("RISK","account="+a.username+" component=TRADING_CONTROL state="+(control.paused?std::string("PAUSED"):std::string("RUNNING"))+" blockedOrderInstructions="+std::to_string(control.blockedOrderInstructions));
    };

    l.info("SYSTEM","account="+a.username+" component=TRADING_CONTROL state=RUNNING newOrdersAllowed=true");
    if(o.interactive){
        const bool marketSubscribed=s.subscribe(instrument);
        if(marketSubscribed)l.info("MARKET","account="+a.username+" event=SUBSCRIPTION_ACTIVE instrument="+instrumentId);
        else l.warn("MARKET","account="+a.username+" event=SUBSCRIPTION_UNAVAILABLE instrument="+instrumentId+" runtimeContinues=true");

        auto normalizeCommand=[](std::string command){
            command=trim(command);
            for(char& ch:command)ch=static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return command;
        };
        auto isStopCommand=[](const std::string& command){return command=="quit"||command=="exit"||command=="q"||command==":q"||command==":wq";};
        std::mutex terminalMu;
        std::condition_variable terminalCv;
        std::vector<std::string> terminalCommands;
        auto pushTerminalCommand=[&](std::string command){
            {
                std::lock_guard<std::mutex> lock(terminalMu);
                terminalCommands.push_back(std::move(command));
            }
            terminalCv.notify_one();
        };
        std::thread terminalReader([&](){
#ifdef _WIN32
            auto normalizeWideCommand=[&](const std::wstring& input){
                std::string command;
                command.reserve(input.size());
                for(wchar_t ch:input){
                    if(ch==L'\uFF1A')command.push_back(':');
                    else if(ch>=L'\uFF10'&&ch<=L'\uFF19')command.push_back(static_cast<char>('0'+(ch-L'\uFF10')));
                    else if(ch>=L'\uFF21'&&ch<=L'\uFF3A')command.push_back(static_cast<char>('A'+(ch-L'\uFF21')));
                    else if(ch>=L'\uFF41'&&ch<=L'\uFF5A')command.push_back(static_cast<char>('a'+(ch-L'\uFF41')));
                    else if(ch>=0&&ch<=0x7f)command.push_back(static_cast<char>(ch));
                }
                return normalizeCommand(std::move(command));
            };
            std::wstring input;
            for(;;){
                const int key=_getwch();
                if(key==WEOF){pushTerminalCommand(":wq");return;}
                if(key==0||key==0xe0){_getwch();continue;}
                if(key==8){if(!input.empty())input.pop_back();continue;}
                if(key==27){input.clear();continue;}
                if(key=='\r'||key=='\n'){
                    std::string command=normalizeWideCommand(input);
                    input.clear();
                    if(command.empty())continue;
                    if(command=="wq")command=":wq";
                    const bool stopReader=isStopCommand(command);
                    pushTerminalCommand(std::move(command));
                    if(stopReader)return;
                    continue;
                }
                if(key>=32)input.push_back(static_cast<wchar_t>(key));
                std::string command=normalizeWideCommand(input);
                if(command=="wq")command=":wq";
                if(isStopCommand(command)){
                    pushTerminalCommand(std::move(command));
                    return;
                }
                if(command=="pause"||command=="resume"||command=="status"||command=="help"){
                    pushTerminalCommand(std::move(command));
                    input.clear();
                }
            }
#else
            for(;;){
                std::string command;
                if(!std::getline(std::cin,command))command=":wq";
                command=normalizeCommand(std::move(command));
                if(command=="wq")command=":wq";
                const bool stopReader=isStopCommand(command);
                pushTerminalCommand(std::move(command));
                if(stopReader)return;
            }
#endif
        });

        std::uint64_t marketVersion=0;
        std::uint64_t marketSnapshots=0;
        auto nextMarketLog=std::chrono::steady_clock::now();
        auto nextHeartbeat=std::chrono::steady_clock::now();
        bool done=false;
        while(!done){
            std::vector<std::string> commands;
            {
                std::unique_lock<std::mutex> lock(terminalMu);
                terminalCv.wait_for(lock,std::chrono::milliseconds(100),[&](){return !terminalCommands.empty();});
                commands.swap(terminalCommands);
            }
            for(const std::string& command:commands){
                if(command=="pause"){
                    if(!s.tradingControl().paused){s.pauseTrading("MANUAL_TERMINAL");dispatchStrategyOrder();}
                    else logStatus();
                }else if(command=="resume"){
                    s.resumeTrading("MANUAL_TERMINAL");
                }else if(command=="status"){
                    logStatus();
                }else if(isStopCommand(command)){
                    if(!s.tradingControl().paused){s.pauseTrading("MANUAL_TERMINAL");dispatchStrategyOrder();}
                    l.info("SYSTEM","account="+a.username+" event=SHUTDOWN_REQUESTED source=MANUAL_TERMINAL input="+command+" newOrdersAllowed=false");
                    done=true;
                    break;
                }else if(command=="help"){
                    std::cout<<"Terminal controls: pause | resume | status | :wq"<<std::endl;
                }else if(!command.empty()){
                    l.warn("SYSTEM","account="+a.username+" component=TRADING_RUNTIME event=UNKNOWN_TERMINAL_INPUT input="+command);
                }
            }
            if(done)break;

            YDMarketData market{};
            if(marketSubscribed&&s.waitNextMarketData(instrument->InstrumentRef,marketVersion,0,market)){
                ++marketSnapshots;
                const auto now=std::chrono::steady_clock::now();
                if(now>=nextMarketLog){
                    l.info("MARKET","account="+a.username+" event=MARKET_SNAPSHOT instrument="+instrumentId
                        +" marketTimeStamp="+std::to_string(market.TimeStamp)
                        +" last="+snapshotNumber(market.LastPrice)+" bid="+snapshotNumber(market.BidPrice)+" ask="+snapshotNumber(market.AskPrice)
                        +" volume="+std::to_string(market.Volume));
                    nextMarketLog=now+std::chrono::seconds(2);
                }
            }
            const auto now=std::chrono::steady_clock::now();
            if(now>=nextHeartbeat){
                const TradingControlSnapshot control=s.tradingControl();
                l.info("SYSTEM","account="+a.username+" component=TRADING_RUNTIME state=ACTIVE tradingState="+(control.paused?std::string("PAUSED"):std::string("RUNNING"))+" marketSnapshots="+std::to_string(marketSnapshots));
                nextHeartbeat=now+std::chrono::seconds(5);
            }
        }
        terminalReader.join();
    }else{
        s.pauseTrading("SYSTEM_CONTROL");
        dispatchStrategyOrder();
        logStatus();
        s.resumeTrading("SYSTEM_CONTROL");
    }

    const OrderActivitySnapshot activityEnd=s.orderActivity();
    const TradingControlSnapshot controlEnd=s.tradingControl();
    const std::uint64_t blockedOrders=controlEnd.blockedOrderInstructions-controlStart.blockedOrderInstructions;
    const std::uint64_t orderApiRequests=activityEnd.orderApiRequests-activityStart.orderApiRequests;
    const std::uint64_t orderRequestsSubmitted=activityEnd.orderRequestsSubmitted-activityStart.orderRequestsSubmitted;
    const bool stateNormal=orderIntentObserved&&controlConsistent&&blockedOrders>0&&orderApiRequests==0&&orderRequestsSubmitted==0;
    const std::string statistics="account="+a.username+" event=TRADING_CONTROL_STATISTICS state="+(controlEnd.paused?std::string("PAUSED"):std::string("RUNNING"))
        +" blockedOrderInstructions="+std::to_string(blockedOrders)
        +" orderApiRequests="+std::to_string(orderApiRequests)
        +" orderRequestsSubmitted="+std::to_string(orderRequestsSubmitted)
        +" apiCalled="+(orderApiRequests==0?std::string("false"):std::string("true"));
    if(stateNormal){l.info("RISK",statistics);r.observe("trading control snapshot",statistics);}
    else{l.error("RISK",statistics);r.fail("trading control consistency",statistics);}
},true,true);}

int runTest10BatchCancel(const RunOptions& o){return runLiveOrderWorkflow(o,"2.10_batch_cancel",LiveWorkflowMode::BatchCancel);}

int runTest11Logging(const RunOptions& o){
    if(o.live){std::cerr<<"ERROR: test_11_logging is a read-only log archive audit and does not accept --live."<<std::endl;return 1;}
    const std::string requestedDate=normalizeArchiveDate(o.logDate);
    return runEach(o,"2.11_logging",[&](const Account&a,const Config&,Logger&l,TestResult&r){
        const std::filesystem::path archiveRoot=o.outputRoot;
        const std::string accountDirectory=a.label.empty()?a.username:a.label;
        const std::string businessDate=requestedDate.empty()?latestArchivedBusinessDate(archiveRoot,l.file(),accountDirectory,a.username):requestedDate;
        l.info("LOG_ARCHIVE","account="+a.username+" component=LOG_ARCHIVE state=SCANNING archiveDate="+(businessDate.empty()?std::string("UNAVAILABLE"):businessDate)+" archiveRoot=\""+absolutePathText(archiveRoot)+"\"");

        ArchivedLogScan archive;
        if(!businessDate.empty())archive=scanArchivedLogs(archiveRoot,l.file(),accountDirectory,a.username,businessDate);
        const auto emitRecord=[&](const std::string& recordType,const ArchivedLogEvidence& evidence){
            if(evidence.found)l.info("LOG_ARCHIVE","account="+a.username+" event=LOG_RECORD_INDEXED recordType="+recordType+" sourceFile=\""+absolutePathText(evidence.file)+"\" sourceRecord=\""+evidence.record+"\"");
            else l.warn("LOG_ARCHIVE","account="+a.username+" event=LOG_RECORD_MISSING recordType="+recordType+" archiveDate="+(businessDate.empty()?std::string("UNAVAILABLE"):businessDate));
        };

        emitRecord("TRADE_ORDER",archive.trade);
        if(archive.lifecycle.found){
            ArchivedLogEvidence started{true,archive.lifecycle.file,archive.lifecycle.started,archive.lifecycle.modified};
            ArchivedLogEvidence login{true,archive.lifecycle.file,archive.lifecycle.login,archive.lifecycle.modified};
            ArchivedLogEvidence shutdown{true,archive.lifecycle.file,archive.lifecycle.shutdown,archive.lifecycle.modified};
            emitRecord("SYSTEM_START",started);
            emitRecord("SYSTEM_LOGIN",login);
            emitRecord("SYSTEM_LOGOUT",shutdown);
        }else{
            l.warn("LOG_ARCHIVE","account="+a.username+" event=LOG_RECORD_MISSING recordType=SYSTEM_LIFECYCLE archiveDate="+(businessDate.empty()?std::string("UNAVAILABLE"):businessDate));
        }
        emitRecord("ORDER_CANCEL_STATISTICS",archive.monitoring);
        emitRecord("CABINET_ERROR",archive.cabinetError);

        std::error_code pathError;
        const bool retentionPathAvailable=std::filesystem::exists(l.file(),pathError)&&!pathError;
        l.info("LOG_ARCHIVE","account="+a.username+" event=LOG_RETENTION_LOCATION archiveRoot=\""+absolutePathText(archiveRoot)+"\" indexFile=\""+absolutePathText(l.file())+"\"");

        std::vector<std::string> missing;
        if(!archive.trade.found)missing.push_back("TRADE_ORDER");
        if(!archive.lifecycle.found)missing.push_back("SYSTEM_LIFECYCLE");
        if(!archive.monitoring.found)missing.push_back("ORDER_CANCEL_STATISTICS");
        if(!archive.cabinetError.found)missing.push_back("CABINET_ERROR");
        if(!retentionPathAvailable)missing.push_back("RETENTION_PATH");
        std::ostringstream missingText;
        if(missing.empty())missingText<<"none";else for(std::size_t index=0;index<missing.size();++index){if(index)missingText<<',';missingText<<missing[index];}
        const bool traceable=missing.empty();
        const std::string statistics="account="+a.username+" event=LOG_ARCHIVE_STATISTICS archiveDate="+(businessDate.empty()?std::string("UNAVAILABLE"):businessDate)
            +" filesScanned="+std::to_string(archive.filesScanned)
            +" tradeLog="+(archive.trade.found?std::string("true"):std::string("false"))
            +" systemRuntimeLog="+(archive.lifecycle.found?std::string("true"):std::string("false"))
            +" monitoringLog="+(archive.monitoring.found?std::string("true"):std::string("false"))
            +" cabinetErrorLog="+(archive.cabinetError.found?std::string("true"):std::string("false"))
            +" retentionPath="+(retentionPathAvailable?std::string("true"):std::string("false"))
            +" traceable="+(traceable?std::string("true"):std::string("false"))
            +" missingRecordTypes="+missingText.str();
        if(traceable){l.info("LOG_ARCHIVE",statistics);r.observe("log archive snapshot",statistics+" indexFile="+absolutePathText(l.file()));}
        else{l.error("LOG_ARCHIVE",statistics);r.fail("log archive completeness",statistics);}
    },false,true);
}

int runAllTests(const RunOptions& o){
    if(o.live){
        std::cerr<<"ERROR: test_all does not allow --live. Run each live test executable separately so its cleanup can be reviewed before continuing."<<std::endl;
        return 1;
    }
    int rc=0;
    auto run=[&](const std::function<int(const RunOptions&)>& test){rc|=test(o);};
    run(runTest01Connect);
    run(runTest12MarketPosition);
    run(runTest02BasicTrade);
    run(runTest03Reconnect);
    run(runTest04OrderCancelCount);
    run(runTest05Duplicate);
    run(runTest06Threshold);
    run(runTest07InstructionCheck);
    run(runTest08ErrorMessage);
    run(runTest09PauseTrade);
    run(runTest10BatchCancel);
    // The logging archive audit depends on real order/cancel and cabinet-error logs.
    // Run test_11_logging explicitly after those live workflows have been reviewed.
    return rc;
}
}
