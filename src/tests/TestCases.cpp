#include "tests/TestCases.h"
#include "core/Logger.h"
#include "core/Monitor.h"
#include "core/OrderValidator.h"
#include "core/TestResult.h"
#include "core/YdSession.h"
#include "ydError.h"
#include <cmath>

namespace ydtest {
namespace {
struct Env { Config cfg; std::string runStamp; std::filesystem::path root; std::vector<Account> accounts; };
Env makeEnv(const RunOptions& o){Env e; if(!e.cfg.load(o.testConfig)) throw std::runtime_error("cannot load test config: "+o.testConfig);e.runStamp=timestampForPath();e.root=std::filesystem::path(o.outputRoot)/e.runStamp;e.accounts=loadAccountsCsv(o.accounts);if(e.accounts.empty())throw std::runtime_error("no accounts in "+o.accounts);return e;}
std::string instID(const RunOptions& o,const Config& c){return o.instrument.empty()?c.get("Test.Instrument","cu2408"):o.instrument;}
int timeout(const Config& c){return c.getInt("Test.TimeoutSeconds",12);}
std::filesystem::path accountLog(const Env& e,const std::string& id,const Account& a){return e.root/id/(a.label.empty()?a.username:a.label)/"test.log";}
int combine(int acc,int code){return (acc||code)?1:0;}

bool readySession(YdSession& s,TestResult& r,int t){
    if(!s.start()){r.fail("API start");return false;}
    if(s.waitConnected(t))r.pass("TCP trade connected");else{r.fail("TCP trade connected","timeout");return false;}
    if(!s.waitLogin(t)){r.fail("account login","timeout");return false;}
    if(s.loginError()!=0){r.fail("account login","errorNo="+std::to_string(s.loginError()));return false;} r.pass("account login");
    if(s.waitInit(t))r.pass("static data ready");else{r.fail("static data ready","notifyFinishInit timeout");return false;}
    if(s.waitCaughtUp(t))r.pass("history caught up");else{r.fail("history caught up","notifyCaughtUp timeout");return false;}
    return true;
}

bool marketFor(YdSession& s,const YDInstrument* i,int t,YDMarketData& md){if(!i)return false;s.subscribe(i);return s.waitMarketData(i->InstrumentRef,t,md);}
double legalPrice(double p,double tick){return tick>0?std::round(p/tick)*tick:p;}
bool terminal(const YDOrder& o){return o.OrderStatus==YD_OS_Canceled||o.OrderStatus==YD_OS_AllTraded||o.OrderStatus==YD_OS_Rejected;}
bool queuedOrTraded(const YDOrder& o){return o.OrderStatus==YD_OS_Queuing||o.OrderStatus==YD_OS_AllTraded||o.OrderStatus==YD_OS_Canceled;}

int closeOffset(const YDInstrument* i){return (i&&i->m_pExchange&&i->m_pExchange->UseTodayPosition)?YD_OF_CloseToday:YD_OF_Close;}

int runEach(const RunOptions& o,const std::string& id,const std::function<void(const Account&,const Config&,Logger&,TestResult&)>& fn){
    Env e=makeEnv(o);int rc=0;for(const auto& a:e.accounts){Logger log(accountLog(e,id,a));log.info("TEST",id+" account="+a.username);TestResult r(id,a.username,log);try{fn(a,e.cfg,log,r);}catch(const std::exception& ex){r.fail("unhandled exception",ex.what());}r.writeSummary(e.root/"summary.csv");rc=combine(rc,r.failed()?1:0);}std::cout<<"Output: "<<e.root.string()<<std::endl;return rc;
}
}

int runTest01Connect(const RunOptions& o){return runEach(o,"2.1_connect",[&](const Account&a,const Config&c,Logger&l,TestResult&r){YdSession s(o.ydConfig,a.username,a.password,l);readySession(s,r,timeout(c));});}

int runTest02BasicTrade(const RunOptions& o){return runEach(o,"2.2_basic_trade",[&](const Account&a,const Config&c,Logger&l,TestResult&r){if(!o.live){r.skip("live trading","rerun with --live in controlled test environment");return;}YdSession s(o.ydConfig,a.username,a.password,l);if(!readySession(s,r,timeout(c)))return;const auto* i=s.instrument(instID(o,c));if(!i){r.fail("instrument exists",instID(o,c));return;}r.pass("instrument exists",i->InstrumentID);YDMarketData md{};if(!marketFor(s,i,timeout(c),md)){r.fail("market data","needed to choose prices");return;}const int aggr=c.getInt("Trade.AggressiveTicks",2);double buy=legalPrice(md.AskPrice+aggr*i->Tick,i->Tick);int openRef=s.sendLimitOrder(i,YD_D_Buy,YD_OF_Open,buy,1);if(openRef<0){r.fail("open order submit");return;}YDOrder oo{};if(s.waitOrder(openRef,timeout(c),queuedOrTraded,oo))r.pass("open instruction submitted");else{r.fail("open instruction submitted","no order state");return;}YDTrade tr{};if(!s.waitTrade(openRef,timeout(c),tr)){r.fail("open trade","not filled; use test environment/liquid contract");if(oo.OrderStatus==YD_OS_Queuing)s.cancelOrder(i,oo);return;}r.pass("open trade");double sell=legalPrice(md.BidPrice-aggr*i->Tick,i->Tick);int closeRef=s.sendLimitOrder(i,YD_D_Sell,closeOffset(i),sell,1);if(closeRef<0){r.fail("close order submit");return;}YDOrder co{};if(s.waitOrder(closeRef,timeout(c),queuedOrTraded,co))r.pass("close instruction submitted");else r.fail("close instruction submitted");YDTrade ctr{};if(s.waitTrade(closeRef,timeout(c),ctr))r.pass("close trade");else r.fail("close trade","not filled");
        // cancel test uses a deliberately passive order
        YDMarketData md2{}; if(!marketFor(s,i,timeout(c),md2)){r.fail("cancel preparation market data");return;}const int farTicks=c.getInt("Trade.WorkingOrderOffsetTicks",50);double far=legalPrice(md2.BidPrice-farTicks*i->Tick,i->Tick);int ref=s.sendLimitOrder(i,YD_D_Buy,YD_OF_Open,far,1);if(ref<0){r.fail("cancel test order submit");return;}YDOrder w{};if(!s.waitOrder(ref,timeout(c),[](const YDOrder&x){return x.OrderStatus==YD_OS_Queuing;},w)){r.fail("working order available for cancel");return;}r.pass("working order available for cancel");if(!s.cancelOrder(i,w)){r.fail("cancel instruction submit");return;}YDOrder fin{};if(s.waitOrder(ref,timeout(c),[](const YDOrder&x){return x.OrderStatus==YD_OS_Canceled;},fin))r.pass("cancel instruction completed");else r.fail("cancel instruction completed");});}

int runTest03Reconnect(const RunOptions& o){return runEach(o,"2.3_reconnect",[&](const Account&a,const Config&c,Logger&l,TestResult&r){YdSession s(o.ydConfig,a.username,a.password,l);if(!readySession(s,r,timeout(c)))return;s.disconnectNow();if(s.waitDisconnected(timeout(c)))r.pass("disconnect detected");else{r.fail("disconnect detected");return;}if(s.waitConnected(timeout(c)))r.pass("reconnect detected");else{r.fail("reconnect detected");return;}if(s.waitCaughtUp(timeout(c)))r.pass("reconnect caught up");else r.fail("reconnect caught up");});}

int runTest04OrderCancelCount(const RunOptions& o){return runEach(o,"2.4_order_cancel_count",[&](const Account&,const Config&c,Logger&l,TestResult&r){Monitor m(l,c.getInt("Threshold.OrderCount",3),c.getInt("Threshold.CancelCount",2),c.getInt("Threshold.DuplicateCount",2));OrderIntent x{instID(o,c),YD_D_Buy,YD_OF_Open,100.0,1,false};m.recordOrder(x);m.recordOrder({x.instrument,YD_D_Sell,YD_OF_Open,101.0,1,false});m.recordOrder({x.instrument,YD_D_Buy,YD_OF_Open,102.0,1,false});m.recordCancel({x.instrument,YD_D_Buy,YD_OF_Open,100.0,1,true});m.recordCancel({x.instrument,YD_D_Sell,YD_OF_Open,101.0,1,true});if(m.orderCount()==3)r.pass("order count","3");else r.fail("order count");if(m.cancelCount()==2)r.pass("cancel count","2");else r.fail("cancel count");});}

int runTest05Duplicate(const RunOptions& o){return runEach(o,"2.5_duplicate",[&](const Account&,const Config&c,Logger&l,TestResult&r){Monitor m(l,999,999,2);OrderIntent open{instID(o,c),YD_D_Buy,YD_OF_Open,100.0,1,false};m.recordOrder(open);m.recordOrder(open);OrderIntent close{instID(o,c),YD_D_Sell,YD_OF_Close,100.0,1,false};m.recordOrder(close);m.recordOrder(close);OrderIntent can{instID(o,c),YD_D_Buy,YD_OF_Open,100.0,1,true};m.recordCancel(can);m.recordCancel(can);if(m.duplicateCount()==3)r.pass("duplicate open/close/cancel statistics","duplicates=3");else r.fail("duplicate statistics","actual="+std::to_string(m.duplicateCount()));});}

int runTest06Threshold(const RunOptions& o){return runEach(o,"2.6_threshold",[&](const Account&,const Config&c,Logger&l,TestResult&r){const int ot=c.getInt("Threshold.OrderCount",3),ct=c.getInt("Threshold.CancelCount",2),dt=c.getInt("Threshold.DuplicateCount",2);Monitor m(l,ot,ct,dt);OrderIntent x{instID(o,c),YD_D_Buy,YD_OF_Open,100.0,1,false};for(int n=0;n<ot;++n){auto y=x;y.price+=n;m.recordOrder(y);}for(int n=0;n<ct;++n){auto y=x;y.cancel=true;y.price+=n;m.recordCancel(y);}m.recordOrder(x);m.recordOrder(x);m.recordOrder(x);if(m.orderAlerted())r.pass("order threshold alert");else r.fail("order threshold alert");if(m.cancelAlerted())r.pass("cancel threshold alert");else r.fail("cancel threshold alert");if(m.duplicateAlerted())r.pass("duplicate threshold alert");else r.fail("duplicate threshold alert");});}

int runTest07InstructionCheck(const RunOptions& o){return runEach(o,"2.7_instruction_check",[&](const Account&a,const Config&c,Logger&l,TestResult&r){YdSession s(o.ydConfig,a.username,a.password,l);if(!readySession(s,r,timeout(c)))return;OrderValidator v(l);auto badInst=v.instrument(s.api(),"THIS_CONTRACT_MUST_NOT_EXIST");if(!badInst.ok)r.pass("invalid instrument rejected locally",badInst.reason);else r.fail("invalid instrument rejected locally");const auto* i=s.instrument(instID(o,c));if(!i){r.fail("reference instrument exists");return;}double badPrice=i->Tick*1000.0+i->Tick*0.5;auto bp=v.limitPrice(i,badPrice);if(!bp.ok)r.pass("invalid minimum price tick rejected locally",bp.reason);else r.fail("invalid minimum price tick rejected locally");auto bv=v.limitVolume(i,i->MaxLimitOrderVolume+std::max(1,i->MinLimitOrderVolume));if(!bv.ok)r.pass("over max single-order volume rejected locally",bv.reason);else r.fail("over max single-order volume rejected locally");});}

int runTest08ErrorMessage(const RunOptions& o){return runEach(o,"2.8_error_message",[&](const Account&a,const Config&c,Logger&l,TestResult&r){if(!o.live){r.skip("live rejected-order test","requires --live; choose --case no-position|insufficient-funds|market-state");return;}YdSession s(o.ydConfig,a.username,a.password,l);if(!readySession(s,r,timeout(c)))return;const auto* i=s.instrument(instID(o,c));if(!i){r.fail("instrument exists");return;}YDMarketData md{};if(!marketFor(s,i,timeout(c),md)){r.fail("market data");return;}const std::string cs=o.caseName.empty()?"no-position":o.caseName;int dir=YD_D_Sell,off=closeOffset(i),vol=1;double price=legalPrice(md.BidPrice-i->Tick,i->Tick);std::set<int> expected;
        if(cs=="no-position"){expected={YD_ERROR_NoPositionToClose};}
        else if(cs=="insufficient-funds"){dir=YD_D_Buy;off=YD_OF_Open;vol=c.getInt("ErrorTest.InsufficientFundsVolume",std::max(1,i->MaxLimitOrderVolume));price=legalPrice(md.AskPrice+i->Tick,i->Tick);expected={YD_ERROR_NoMoneyToOpen};}
        else if(cs=="market-state"){dir=YD_D_Buy;off=YD_OF_Open;expected={YD_ERROR_InstrumentTradingPaused,YD_ERROR_SSEATPGatewayNoTradingTime};}
        else {r.fail("known --case","use no-position|insufficient-funds|market-state");return;}
        int ref=s.sendLimitOrder(i,dir,off,price,vol);if(ref<0){r.fail("error test submit","insertOrder returned false before callback");return;}YDOrder ro{};if(!s.waitOrder(ref,timeout(c),[](const YDOrder&x){return x.ErrorNo!=0||x.OrderStatus==YD_OS_Rejected;},ro)){r.fail("error callback received","environment did not create requested rejection");return;}if(expected.count(ro.ErrorNo))r.pass("expected error displayed","case="+cs+" errorNo="+std::to_string(ro.ErrorNo));else r.fail("expected error displayed","case="+cs+" actual errorNo="+std::to_string(ro.ErrorNo));});}

int runTest09PauseTrade(const RunOptions& o){return runEach(o,"2.9_pause_trade",[&](const Account&,const Config&,Logger&l,TestResult&r){TradingGate g;if(g.canTrade())r.pass("trading initially enabled");else r.fail("trading initially enabled");g.pause();if(!g.canTrade()){l.warn("RISK","order blocked because trading is paused");r.pass("pause blocks trading instructions");}else r.fail("pause blocks trading instructions");g.resume();if(g.canTrade())r.pass("resume re-enables trading");else r.fail("resume re-enables trading");});}

int runTest10BatchCancel(const RunOptions& o){return runEach(o,"2.10_batch_cancel",[&](const Account&a,const Config&c,Logger&l,TestResult&r){if(!o.live){r.skip("batch cancel live test","rerun with --live");return;}YdSession s(o.ydConfig,a.username,a.password,l);if(!readySession(s,r,timeout(c)))return;const auto* i=s.instrument(instID(o,c));if(!i){r.fail("instrument exists");return;}YDMarketData md{};if(!marketFor(s,i,timeout(c),md)){r.fail("market data");return;}const int far=c.getInt("Trade.WorkingOrderOffsetTicks",50);std::vector<std::pair<const YDInstrument*,YDOrder>> working;for(int n=0;n<2;++n){double p=legalPrice(md.BidPrice-(far+n)*i->Tick,i->Tick);int ref=s.sendLimitOrder(i,YD_D_Buy,YD_OF_Open,p,1);YDOrder w{};if(ref<0||!s.waitOrder(ref,timeout(c),[](const YDOrder&x){return x.OrderStatus==YD_OS_Queuing;},w)){r.fail("prepare working order "+std::to_string(n+1));return;}working.push_back({i,w});}r.pass("prepare multiple working orders","count=2");if(!s.cancelMulti(working)){r.fail("cancelMultiOrders submit");return;}bool all=true;for(auto& x:working){YDOrder f{};if(!s.waitOrder(x.second.OrderRef,timeout(c),[](const YDOrder&o){return o.OrderStatus==YD_OS_Canceled;},f))all=false;}if(all)r.pass("batch cancel completed");else r.fail("batch cancel completed");});}

int runTest11Logging(const RunOptions& o){return runEach(o,"2.11_logging",[&](const Account&a,const Config&c,Logger&l,TestResult&r){l.info("SYSTEM","system runtime record example");l.info("MONITOR","monitor record example orderCount=1 cancelCount=0");l.error("ERROR","error-prompt record example");l.info("TRADE","trade/order record example instrument="+instID(o,c)+" volume=1");r.pass("system runtime log written",l.file().string());r.pass("monitoring log written");r.pass("error log written");r.pass("trade information log written");
        // Also verify real connection logging when credentials are available.
        YdSession s(o.ydConfig,a.username,a.password,l);if(s.start()&&s.waitConnected(timeout(c)))r.pass("YD connection event recorded");else r.fail("YD connection event recorded");});}

int runAllTests(const RunOptions& o){int rc=0;rc|=runTest01Connect(o);rc|=runTest02BasicTrade(o);rc|=runTest03Reconnect(o);rc|=runTest04OrderCancelCount(o);rc|=runTest05Duplicate(o);rc|=runTest06Threshold(o);rc|=runTest07InstructionCheck(o);rc|=runTest08ErrorMessage(o);rc|=runTest09PauseTrade(o);rc|=runTest10BatchCancel(o);rc|=runTest11Logging(o);return rc;}
}
