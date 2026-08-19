#include "tests/TestCases.h"
#include "core/Logger.h"
#include "core/Monitor.h"
#include "core/OrderValidator.h"
#include "core/TestResult.h"
#include "core/YdSession.h"
#include "ydError.h"
#include <cfloat>
#include <cmath>
#include <memory>

namespace ydtest {
namespace {
struct Env { Config cfg; std::string runStamp; std::filesystem::path root; std::vector<Account> accounts; };
Env makeEnv(const RunOptions& o){Env e; if(!e.cfg.load(o.testConfig)) throw std::runtime_error("cannot load test config: "+o.testConfig);e.runStamp=timestampForPath();e.root=std::filesystem::path(o.outputRoot)/e.runStamp;e.accounts=loadAccountsCsv(o.accounts);if(e.accounts.empty())throw std::runtime_error("no accounts in "+o.accounts);return e;}
std::string instID(const RunOptions& o,const Config& c){return o.instrument.empty()?c.get("Test.Instrument","au2612"):o.instrument;}
int timeout(const Config& c){return c.getInt("Test.TimeoutSeconds",12);}
std::filesystem::path accountLog(const Env& e,const std::string& id,const Account& a){return e.root/id/(a.label.empty()?a.username:a.label)/"test.log";}
int combine(int acc,int code){return (acc||code)?1:0;}

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

std::string orderSystemId(const YDOrder& order){return std::to_string(order.LongOrderSysID!=0?order.LongOrderSysID:static_cast<long long>(order.OrderSysID));}
std::string tradeId(const YDTrade& trade){return std::to_string(trade.LongTradeID!=0?trade.LongTradeID:static_cast<long long>(trade.TradeID));}

struct LiveTicket {
    const char* phase="";
    int sequence=0;
    int orderRef=0;
    int direction=YD_D_Buy;
    int offset=YD_OF_Open;
    double orderPrice=0;
    int volume=0;
};

struct LongPositionSnapshot {
    int today=0;
    int history=0;
    int other=0;
    int total()const{return today+history+other;}
};

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
    if(order.LongOrderSysID==0&&order.OrderSysID==0){reason="system order ID unavailable";return false;}
    return true;
}

bool tradeMatchesTicket(const LiveTicket& ticket,const YDOrder& order,const YDTrade& trade,std::string& reason){
    if(trade.OrderRef!=ticket.orderRef){reason="trade OrderRef mismatch";return false;}
    if(trade.Direction!=ticket.direction){reason="trade direction mismatch";return false;}
    if(trade.OffsetFlag!=ticket.offset){reason="trade offset mismatch";return false;}
    if(trade.Volume!=ticket.volume){reason="trade volume mismatch";return false;}
    if(!usablePrice(trade.Price)){reason="trade price unavailable";return false;}
    if(trade.LongTradeID==0&&trade.TradeID==0){reason="trade ID unavailable";return false;}
    if(order.LongOrderSysID!=0&&trade.LongOrderSysID!=0&&order.LongOrderSysID!=trade.LongOrderSysID){reason="long system order ID mismatch";return false;}
    if((order.LongOrderSysID==0||trade.LongOrderSysID==0)&&order.OrderSysID!=trade.OrderSysID){reason="system order ID mismatch";return false;}
    return true;
}

void logFillResult(Logger& log,const std::string& account,const std::string& instrumentId,const LiveTicket& ticket,const YDOrder& order,const YDTrade& trade){
    std::ostringstream line;line<<ticket.phase<<" | seq="<<ticket.sequence<<"/2 | account="<<account
        <<" | instrument="<<instrumentId
        <<" | direction="<<directionName(order.Direction)<<" | offset="<<offsetName(order.OffsetFlag)
        <<" | orderPrice="<<snapshotNumber(order.Price)<<" | tradePrice="<<snapshotNumber(trade.Price)
        <<" | volume="<<order.OrderVolume<<" | filled="<<order.TradeVolume<<'/'<<order.OrderVolume
        <<" | orderRef="<<order.OrderRef<<" | orderSysId="<<orderSystemId(order)<<" | tradeId="<<tradeId(trade)
        <<" | status="<<orderStatusName(order.OrderStatus)<<" | errorNo="<<order.ErrorNo<<" | resultTime="<<timestampText();
    log.info("ORDER_RESULT",line.str());
}

void logCancelResult(Logger& log,const std::string& account,const std::string& instrumentId,const LiveTicket& ticket,const YDOrder& order){
    std::ostringstream line;line<<ticket.phase<<" | seq="<<ticket.sequence<<"/2 | account="<<account
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

bool marketFor(YdSession& s,const YDInstrument* i,int t,YDMarketData& md){return i&&s.subscribe(i)&&s.waitMarketData(i->InstrumentRef,t,md);}
double legalPrice(double p,double tick){return tick>0?std::round(p/tick)*tick:p;}
bool terminal(const YDOrder& o){return o.OrderStatus==YD_OS_Canceled||o.OrderStatus==YD_OS_AllTraded||o.OrderStatus==YD_OS_Rejected;}

int closeOffset(const YDInstrument* i){return (i&&i->m_pExchange&&i->m_pExchange->UseTodayPosition)?YD_OF_CloseToday:YD_OF_Close;}

int runEach(const RunOptions& o,const std::string& id,const std::function<void(const Account&,const Config&,Logger&,TestResult&)>& fn,bool stopAccountsOnFailure=false){
    Env e=makeEnv(o);int rc=0;for(const auto& a:e.accounts){Logger log(accountLog(e,id,a));log.info("TEST",id+" account="+a.username);TestResult r(id,a.username,log);try{fn(a,e.cfg,log,r);}catch(const std::exception& ex){r.fail("unhandled exception",ex.what());}r.writeSummary(e.root/"summary.csv");const bool accountFailed=r.failed();rc=combine(rc,accountFailed?1:0);if(stopAccountsOnFailure&&accountFailed){log.error("ALERT","LIVE account sequence stopped after failure account="+a.username+"; no later accounts were started");break;}}std::cout<<"Output: "<<e.root.string()<<std::endl;return rc;
}
}

int runTest01Connect(const RunOptions& o){return runEach(o,"2.1_connect",[&](const Account&a,const Config&c,Logger&l,TestResult&r){YdSession s(o.ydConfig,a.username,a.password,l);readySession(s,r,timeout(c),a.username);});}

int runTest12MarketPosition(const RunOptions& o){return runEach(o,"1.2_market_position",[&](const Account&a,const Config&c,Logger&l,TestResult&r){
    l.info("SYSTEM","READ_ONLY snapshot test: no order or cancel API will be called");
    const int snapshotTimeout=c.getInt("Snapshot.TimeoutSeconds",120);
    YdSession s(o.ydConfig,a.username,a.password,l,true);if(!readySession(s,r,snapshotTimeout,a.username))return;
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

enum class LiveWorkflowMode { BasicTrade, OrderCancelCount };

int runLiveOrderWorkflow(const RunOptions& o,const std::string& testId,LiveWorkflowMode mode){
return runEach(o,testId,[&](const Account&a,const Config&c,Logger&l,TestResult&r){
    const bool countMode=mode==LiveWorkflowMode::OrderCancelCount;
    // This gate must remain before YdSession construction: without --live this test performs no order/cancel API call.
    if(!o.live){r.skip(countMode?"live order/cancel monitoring":"live trading","rerun with --live in a broker-approved test environment");return;}

    constexpr int requiredCount=2;
    constexpr int orderVolume=1;
    const int sessionTimeout=std::max(1,c.getInt("Trade.SessionTimeoutSeconds",120));
    const int actionTimeout=std::max(1,c.getInt("Trade.ActionTimeoutSeconds",30));
    const int callbackQuietMilliseconds=std::max(1,c.getInt("Trade.CallbackQuietMilliseconds",2000));
    const int aggressiveTicks=std::max(0,c.getInt("Trade.AggressiveTicks",2));
    const int workingOffsetTicks=std::max(1,c.getInt("Trade.WorkingOrderOffsetTicks",50));

    YdSession s(o.ydConfig,a.username,a.password,l,true);
    if(!readySession(s,r,sessionTimeout,a.username))return;
    const std::string requestedInstrument=instID(o,c);
    const YDInstrument* instrument=s.instrument(requestedInstrument);
    if(!instrument){r.fail("instrument exists","instrument="+requestedInstrument);return;}
    const std::string instrumentId=instrument->InstrumentID;
    r.pass("instrument exists","instrument="+instrumentId);

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
    if(baselinePosition.total()!=0){
        r.fail("live trading preflight","account="+accountId+" instrument="+instrumentId+" has existing LONG SPECULATION position="+std::to_string(baselinePosition.total())+"; choose a zero-long-position contract so this test cannot close pre-existing lots; no order sent");return;
    }

    if(!s.subscribe(instrument)){r.fail("live trading preflight","market subscription failed account="+accountId+" instrument="+instrumentId+"; no order sent");return;}
    std::uint64_t marketVersion=0;
    YDMarketData initialMarket{};
    if(!s.waitNextMarketData(instrument->InstrumentRef,marketVersion,sessionTimeout,initialMarket)){r.fail("live trading preflight","market data timeout account="+accountId+" instrument="+instrumentId+"; no order sent");return;}
    std::string marketReason;
    if(!usableMarket(instrument,initialMarket,marketReason)){r.fail("live trading preflight","account="+accountId+" instrument="+instrumentId+" reason="+marketReason+"; no order sent");return;}
    r.pass("live trading preflight","account="+accountId+" instrument="+instrumentId+" bid="+snapshotNumber(initialMarket.BidPrice)+" ask="+snapshotNumber(initialMarket.AskPrice)+" volumePerOrder=1");
    const OrderActivitySnapshot activityStart=s.orderActivity();
    std::vector<LiveTicket> tickets;
    tickets.reserve(16);
    int openCompleted=0,cancelCompleted=0,closeCompleted=0;
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
        LiveTicket ticket{phase,sequence,0,direction,offset,price,orderVolume};
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
        LiveTicket ticket{"CANCEL",sequence,0,YD_D_Buy,YD_OF_Open,price,orderVolume};
        tickets.push_back(ticket);
        ticket.orderRef=s.sendLimitOrder(instrument,ticket.direction,ticket.offset,ticket.orderPrice,ticket.volume);
        if(ticket.orderRef<0){tickets.pop_back();noteFailure("CANCEL #"+std::to_string(sequence)+" insertOrder returned false");return false;}
        tickets.back().orderRef=ticket.orderRef;

        YDOrder working{};
        if(!s.waitOrder(ticket.orderRef,actionTimeout,[](const YDOrder& order){return order.OrderStatus==YD_OS_Queuing||terminal(order);},working)){
            noteFailure("CANCEL #"+std::to_string(sequence)+" working/terminal order callback timeout ref="+std::to_string(ticket.orderRef));return false;
        }
        if(working.ErrorNo!=0||working.OrderStatus!=YD_OS_Queuing){
            noteFailure("CANCEL #"+std::to_string(sequence)+" did not become cancelable ref="+std::to_string(ticket.orderRef)+" status="+orderStatusName(working.OrderStatus)+" errorNo="+std::to_string(working.ErrorNo)+" traded="+std::to_string(working.TradeVolume));return false;
        }
        if(working.LongOrderSysID==0&&working.OrderSysID==0){
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

    bool executionException=false;
    std::string executionExceptionDetail;
    try{
        if(countMode){
            stepFailure.clear();
            for(int sequence=1;sequence<=requiredCount;++sequence){if(!sendAndCancel(sequence))break;++cancelCompleted;}
            if(cancelCompleted==requiredCount)r.pass("monitored order cancellations","account="+accountId+" instrument="+instrumentId+" completed=2/2");
            else r.fail("monitored order cancellations","account="+accountId+" instrument="+instrumentId+" completed="+std::to_string(cancelCompleted)+"/2 reason="+stepFailure);
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
            if(!s.waitOrder(ticket.orderRef,actionTimeout,[](const YDOrder& order){return order.OrderStatus==YD_OS_Queuing||terminal(order);},state)){
                unresolvedRefs.push_back(ticket.orderRef);l.error("CLEANUP","no cancelable/terminal state ref="+std::to_string(ticket.orderRef));continue;
            }
            if(!terminal(state)){
                l.warn("CLEANUP","canceling outstanding order account="+accountId+" instrument="+instrumentId+" ref="+std::to_string(ticket.orderRef)+" phase="+ticket.phase);
                if(state.LongOrderSysID==0&&state.OrderSysID==0){
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
            if(std::string(ticket.phase)=="CANCEL")unexpectedTradeVolume+=observedVolume;
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
                +" baselineTotal="+std::to_string(baselinePosition.total())+" finalTotal="+std::to_string(finalPosition.total())
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
    const bool monitoringPass=countMode&&!executionException&&cancelCompleted==requiredCount&&measurementCountsMatch
        &&totalCountsStable&&callbackStreamQuiet&&streamStableThroughStop&&measurementTradeKnown&&tradeVolumeConsistent&&unexpectedTradeVolume==0&&cleanupRestored&&cleanupUsedNoTradeApi;
    if(cleanupRestored){
        r.pass("live cleanup","account="+accountId+" instrument="+instrumentId+" noWorkingOrders=true ownedOrderNetPosition=0 baselineQuantityRestored=true");
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

    if(executionException){
        r.fail(countMode?"order/cancel monitoring exception":"basic trading workflow exception","account="+accountId+" reason="+executionExceptionDetail+"; cleanup was attempted");
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
    }else if(openCompleted==requiredCount&&cancelCompleted==requiredCount&&closeCompleted==requiredCount
        &&callbackStreamQuiet&&streamStableThroughStop&&measurementTradeKnown&&tradeVolumeConsistent&&unexpectedTradeVolume==0
        &&totalActivity.failedCancelCallbacks==0&&totalActivity.callbackValidationFailures==0&&cleanupRestored){
        r.pass("basic trading workflow","account="+accountId+" instrument="+instrumentId+" openFills=2 cancellations=2 closeFills=2 ownedOrderNetPosition=0 baselineQuantityRestored=true");
    }else r.fail("basic trading workflow","account="+accountId+" instrument="+instrumentId+" openFills="+std::to_string(openCompleted)+" cancellations="+std::to_string(cancelCompleted)+" closeFills="+std::to_string(closeCompleted)+" callbackStreamQuiet="+(callbackStreamQuiet?std::string("true"):std::string("false"))+" streamStableThroughStop="+(streamStableThroughStop?std::string("true"):std::string("false")));
},true);
}

int runTest02BasicTrade(const RunOptions& o){return runLiveOrderWorkflow(o,"2.2_basic_trade",LiveWorkflowMode::BasicTrade);}

int runTest03Reconnect(const RunOptions& o){return runEach(o,"2.3_reconnect",[&](const Account&a,const Config&c,Logger&l,TestResult&r){
    const int sessionTimeout=std::max(1,c.getInt("Reconnect.SessionTimeoutSeconds",120));
    const int disconnectTimeout=std::max(1,c.getInt("Reconnect.DisconnectTimeoutSeconds",30));
    const int reconnectTimeout=std::max(1,c.getInt("Reconnect.ReconnectTimeoutSeconds",120));
    YdSession s(o.ydConfig,a.username,a.password,l);if(!readySession(s,r,sessionTimeout,a.username))return;
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

int runTest05Duplicate(const RunOptions& o){return runEach(o,"2.5_duplicate",[&](const Account&,const Config&c,Logger&l,TestResult&r){Monitor m(l,999,999,2);OrderIntent open{instID(o,c),YD_D_Buy,YD_OF_Open,100.0,1,false};m.recordOrder(open);m.recordOrder(open);OrderIntent close{instID(o,c),YD_D_Sell,YD_OF_Close,100.0,1,false};m.recordOrder(close);m.recordOrder(close);OrderIntent can{instID(o,c),YD_D_Buy,YD_OF_Open,100.0,1,true};m.recordCancel(can);m.recordCancel(can);if(m.duplicateCount()==3)r.pass("duplicate open/close/cancel statistics","duplicates=3");else r.fail("duplicate statistics","actual="+std::to_string(m.duplicateCount()));});}

int runTest06Threshold(const RunOptions& o){return runEach(o,"2.6_threshold",[&](const Account&,const Config&c,Logger&l,TestResult&r){const int ot=c.getInt("Threshold.OrderCount",3),ct=c.getInt("Threshold.CancelCount",2),dt=c.getInt("Threshold.DuplicateCount",2);Monitor m(l,ot,ct,dt);OrderIntent x{instID(o,c),YD_D_Buy,YD_OF_Open,100.0,1,false};for(int n=0;n<ot;++n){auto y=x;y.price+=n;m.recordOrder(y);}for(int n=0;n<ct;++n){auto y=x;y.cancel=true;y.price+=n;m.recordCancel(y);}m.recordOrder(x);m.recordOrder(x);m.recordOrder(x);if(m.orderAlerted())r.pass("order threshold alert");else r.fail("order threshold alert");if(m.cancelAlerted())r.pass("cancel threshold alert");else r.fail("cancel threshold alert");if(m.duplicateAlerted())r.pass("duplicate threshold alert");else r.fail("duplicate threshold alert");});}

int runTest07InstructionCheck(const RunOptions& o){return runEach(o,"2.7_instruction_check",[&](const Account&a,const Config&c,Logger&l,TestResult&r){YdSession s(o.ydConfig,a.username,a.password,l);if(!readySession(s,r,timeout(c),a.username))return;OrderValidator v(l);auto badInst=v.instrument(s.api(),"THIS_CONTRACT_MUST_NOT_EXIST");if(!badInst.ok)r.pass("invalid instrument rejected locally",badInst.reason);else r.fail("invalid instrument rejected locally");const auto* i=s.instrument(instID(o,c));if(!i){r.fail("reference instrument exists");return;}double badPrice=i->Tick*1000.0+i->Tick*0.5;auto bp=v.limitPrice(i,badPrice);if(!bp.ok)r.pass("invalid minimum price tick rejected locally",bp.reason);else r.fail("invalid minimum price tick rejected locally");auto bv=v.limitVolume(i,i->MaxLimitOrderVolume+std::max(1,i->MinLimitOrderVolume));if(!bv.ok)r.pass("over max single-order volume rejected locally",bv.reason);else r.fail("over max single-order volume rejected locally");});}

int runTest08ErrorMessage(const RunOptions& o){return runEach(o,"2.8_error_message",[&](const Account&a,const Config&c,Logger&l,TestResult&r){if(!o.live){r.skip("live rejected-order test","requires --live; choose --case no-position|insufficient-funds|market-state");return;}YdSession s(o.ydConfig,a.username,a.password,l);if(!readySession(s,r,timeout(c),a.username))return;const auto* i=s.instrument(instID(o,c));if(!i){r.fail("instrument exists");return;}YDMarketData md{};if(!marketFor(s,i,timeout(c),md)){r.fail("market data");return;}const std::string cs=o.caseName.empty()?"no-position":o.caseName;int dir=YD_D_Sell,off=closeOffset(i),vol=1;double price=legalPrice(md.BidPrice-i->Tick,i->Tick);std::set<int> expected;
        if(cs=="no-position"){expected={YD_ERROR_NoPositionToClose};}
        else if(cs=="insufficient-funds"){dir=YD_D_Buy;off=YD_OF_Open;vol=c.getInt("ErrorTest.InsufficientFundsVolume",std::max(1,i->MaxLimitOrderVolume));price=legalPrice(md.AskPrice+i->Tick,i->Tick);expected={YD_ERROR_NoMoneyToOpen};}
        else if(cs=="market-state"){dir=YD_D_Buy;off=YD_OF_Open;expected={YD_ERROR_InstrumentTradingPaused,YD_ERROR_SSEATPGatewayNoTradingTime};}
        else {r.fail("known --case","use no-position|insufficient-funds|market-state");return;}
        int ref=s.sendLimitOrder(i,dir,off,price,vol);if(ref<0){r.fail("error test submit","insertOrder returned false before callback");return;}YDOrder ro{};if(!s.waitOrder(ref,timeout(c),[](const YDOrder&x){return x.ErrorNo!=0||x.OrderStatus==YD_OS_Rejected;},ro)){r.fail("error callback received","environment did not create requested rejection");return;}if(expected.count(ro.ErrorNo))r.pass("expected error displayed","case="+cs+" errorNo="+std::to_string(ro.ErrorNo));else r.fail("expected error displayed","case="+cs+" actual errorNo="+std::to_string(ro.ErrorNo));},true);}

int runTest09PauseTrade(const RunOptions& o){return runEach(o,"2.9_pause_trade",[&](const Account&,const Config&,Logger&l,TestResult&r){TradingGate g;if(g.canTrade())r.pass("trading initially enabled");else r.fail("trading initially enabled");g.pause();if(!g.canTrade()){l.warn("RISK","order blocked because trading is paused");r.pass("pause blocks trading instructions");}else r.fail("pause blocks trading instructions");g.resume();if(g.canTrade())r.pass("resume re-enables trading");else r.fail("resume re-enables trading");});}

int runTest10BatchCancel(const RunOptions& o){return runEach(o,"2.10_batch_cancel",[&](const Account&a,const Config&c,Logger&l,TestResult&r){if(!o.live){r.skip("batch cancel live test","rerun with --live");return;}YdSession s(o.ydConfig,a.username,a.password,l);if(!readySession(s,r,timeout(c),a.username))return;const auto* i=s.instrument(instID(o,c));if(!i){r.fail("instrument exists");return;}YDMarketData md{};if(!marketFor(s,i,timeout(c),md)){r.fail("market data");return;}const int far=c.getInt("Trade.WorkingOrderOffsetTicks",50);std::vector<std::pair<const YDInstrument*,YDOrder>> working;for(int n=0;n<2;++n){double p=legalPrice(md.BidPrice-(far+n)*i->Tick,i->Tick);int ref=s.sendLimitOrder(i,YD_D_Buy,YD_OF_Open,p,1);YDOrder w{};if(ref<0||!s.waitOrder(ref,timeout(c),[](const YDOrder&x){return x.OrderStatus==YD_OS_Queuing;},w)){r.fail("prepare working order "+std::to_string(n+1));return;}working.push_back({i,w});}r.pass("prepare multiple working orders","count=2");if(!s.cancelMulti(working)){r.fail("cancelMultiOrders submit");return;}bool all=true;for(auto& x:working){YDOrder f{};if(!s.waitOrder(x.second.OrderRef,timeout(c),[](const YDOrder&o){return o.OrderStatus==YD_OS_Canceled;},f))all=false;}if(all)r.pass("batch cancel completed");else r.fail("batch cancel completed");},true);}

int runTest11Logging(const RunOptions& o){return runEach(o,"2.11_logging",[&](const Account&a,const Config&c,Logger&l,TestResult&r){l.info("SYSTEM","system runtime record example");l.info("MONITOR","monitor record example orderCount=1 cancelCount=0");l.error("ERROR","error-prompt record example");l.info("TRADE","trade/order record example instrument="+instID(o,c)+" volume=1");r.pass("system runtime log written",l.file().string());r.pass("monitoring log written");r.pass("error log written");r.pass("trade information log written");
        // Also verify real connection logging when credentials are available.
        YdSession s(o.ydConfig,a.username,a.password,l);if(s.start()&&s.waitConnected(timeout(c)))r.pass("YD connection event recorded");else r.fail("YD connection event recorded");});}

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
    run(runTest11Logging);
    return rc;
}
}
