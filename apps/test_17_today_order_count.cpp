#include "core/Config.h"
#include "core/Logger.h"
#include "core/Monitor.h"
#include "core/YdSession.h"
#include <iostream>
#include <map>
#include <set>

int main(int argc,char** argv){
    try{
        const auto o=ydtest::parseArgs(argc,argv);
        const auto accounts=ydtest::loadAccountsCsv(o.accounts);
        if(accounts.empty()){std::cerr<<"ERROR: no accounts in "<<o.accounts<<std::endl;return 2;}

        ydtest::Config cfg;
        int timeoutSeconds=12;
        if(cfg.load(o.testConfig)) timeoutSeconds=cfg.getInt("Test.TimeoutSeconds",12);

        const auto& account=accounts.front();
        ydtest::Logger log(std::filesystem::path(o.outputRoot)/"today_order_count.log");
        ydtest::YdSession session(o.ydConfig,account.username,account.password,log);

        if(!session.start()){std::cerr<<"ERROR: start failed"<<std::endl;return 1;}
        if(!session.waitConnected(timeoutSeconds)){std::cerr<<"ERROR: trade connection timeout"<<std::endl;return 1;}
        if(!session.waitLogin(timeoutSeconds)||session.loginError()!=0){
            std::cerr<<"ERROR: login failed errorNo="<<session.loginError()<<std::endl;return 1;
        }
        if(!session.waitInit(timeoutSeconds)){std::cerr<<"ERROR: notifyFinishInit timeout"<<std::endl;return 1;}
        if(!session.waitCaughtUp(timeoutSeconds)){std::cerr<<"ERROR: notifyCaughtUp timeout"<<std::endl;return 1;}

        YDApi* api=session.api();
        if(!api){std::cerr<<"ERROR: API unavailable"<<std::endl;return 1;}

        const int tradingDay=api->getTradingDay();
        const std::string accountId=(api->getMyAccount()&&api->getMyAccount()->AccountID[0])
            ?std::string(api->getMyAccount()->AccountID):account.username;

        ydtest::MonitorThresholds thresholds;
        thresholds.orderCount=std::max(0,cfg.getInt("Threshold.OrderCount",0));
        thresholds.cancelCount=std::max(0,cfg.getInt("Threshold.CancelCount",0));
        thresholds.duplicateCount=std::max(0,cfg.getInt("Threshold.DuplicateCount",0));
        thresholds.popupEnabled=cfg.getBool("Threshold.PopupEnabled",false);
        ydtest::Monitor monitor(log,accountId,thresholds);

        std::uint64_t orderTotal=0,cancelTotal=0,rejectedTotal=0,fillTotal=0;

        std::cout<<"----------------------------------------"<<std::endl;
        std::cout<<"account      : "<<accountId<<std::endl;
        std::cout<<"tradingDay   : "<<tradingDay<<std::endl;

        if(!o.instrument.empty()){
            // Single instrument query.
            const auto counts=session.historicalCallbackCounts(o.instrument);
            orderTotal=counts.order;cancelTotal=counts.cancel;rejectedTotal=counts.rejected;fillTotal=counts.trade;
            std::cout<<"instrument   : "<<o.instrument<<std::endl;
            std::cout<<"orderTotal   : "<<orderTotal<<std::endl;
            std::cout<<"cancelTotal  : "<<cancelTotal<<std::endl;
            std::cout<<"rejectedTotal: "<<rejectedTotal<<std::endl;
            std::cout<<"fillTotal    : "<<fillTotal<<std::endl;
            log.info("ORDER_STATISTICS","account="+accountId+" tradingDay="+std::to_string(tradingDay)
                +" instrument="+o.instrument
                +" orderTotal="+std::to_string(orderTotal)
                +" cancelTotal="+std::to_string(cancelTotal)
                +" rejectedTotal="+std::to_string(rejectedTotal)
                +" fillTotal="+std::to_string(fillTotal));
        }else{
            // All instruments: aggregate the per-instrument counters.
            std::set<std::string> seen;
            const int instrumentCount=api->getInstrumentCount();
            for(int pos=0;pos<instrumentCount;++pos){
                const YDInstrument* inst=api->getInstrument(pos);
                if(!inst||!inst->InstrumentID[0])continue;
                const std::string id(inst->InstrumentID);
                if(!seen.insert(id).second)continue;
                const auto counts=session.historicalCallbackCounts(id);
                if(counts.order==0&&counts.trade==0&&counts.cancel==0&&counts.rejected==0)continue;
                orderTotal+=counts.order;cancelTotal+=counts.cancel;rejectedTotal+=counts.rejected;fillTotal+=counts.trade;
            }
            std::cout<<"instrument   : ALL"<<std::endl;
            std::cout<<"orderTotal   : "<<orderTotal<<std::endl;
            std::cout<<"cancelTotal  : "<<cancelTotal<<std::endl;
            std::cout<<"rejectedTotal: "<<rejectedTotal<<std::endl;
            std::cout<<"fillTotal    : "<<fillTotal<<std::endl;
            log.info("ORDER_STATISTICS","account="+accountId+" tradingDay="+std::to_string(tradingDay)
                +" instrument=ALL"
                +" orderTotal="+std::to_string(orderTotal)
                +" cancelTotal="+std::to_string(cancelTotal)
                +" rejectedTotal="+std::to_string(rejectedTotal)
                +" fillTotal="+std::to_string(fillTotal));
        }

        // Threshold alerts based on today's counted totals.
        monitor.alertOnTodayCounts(orderTotal,cancelTotal);

        std::cout<<"----------------------------------------"<<std::endl;
        std::cout<<"Read-only tool: no orders or cancels were sent."<<std::endl;
        return 0;
    }catch(const std::exception& e){
        std::cerr<<"ERROR: "<<e.what()<<std::endl;
        return 2;
    }
}
