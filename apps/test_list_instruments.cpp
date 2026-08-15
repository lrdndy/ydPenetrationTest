#include "core/Config.h"
#include "core/Logger.h"
#include "core/YdSession.h"
#include <iostream>

int main(int argc,char** argv){
    try{
        const auto o=ydtest::parseArgs(argc,argv);
        const auto accounts=ydtest::loadAccountsCsv(o.accounts);
        if(accounts.empty()){
            std::cerr<<"ERROR: no accounts in "<<o.accounts<<std::endl;
            return 2;
        }

        ydtest::Config cfg;
        int timeoutSeconds=12;
        if(cfg.load(o.testConfig)) timeoutSeconds=cfg.getInt("Test.TimeoutSeconds",12);

        const auto& account=accounts.front();
        ydtest::Logger log(std::filesystem::path(o.outputRoot)/"list_instruments.log");
        ydtest::YdSession session(o.ydConfig,account.username,account.password,log);

        if(!session.start()) return 1;
        if(!session.waitConnected(timeoutSeconds)){
            std::cerr<<"ERROR: trade connection timeout"<<std::endl;
            return 1;
        }
        if(!session.waitLogin(timeoutSeconds)||session.loginError()!=0){
            std::cerr<<"ERROR: login failed errorNo="<<session.loginError()<<std::endl;
            return 1;
        }
        if(!session.waitInit(timeoutSeconds)){
            std::cerr<<"ERROR: notifyFinishInit timeout"<<std::endl;
            return 1;
        }
        if(!session.waitCaughtUp(timeoutSeconds)){
            std::cerr<<"ERROR: notifyCaughtUp timeout"<<std::endl;
            return 1;
        }

        YDApi* api=session.api();
        if(!api){
            std::cerr<<"ERROR: API unavailable"<<std::endl;
            return 1;
        }

        const std::string filter=o.instrument;
        const int total=api->getInstrumentCount();
        int matched=0;

        std::cout<<"Instrument count: "<<total<<std::endl;
        if(!filter.empty()) std::cout<<"Filter: "<<filter<<std::endl;
        std::cout<<"----------------------------------------"<<std::endl;

        for(int pos=0;pos<total;++pos){
            const YDInstrument* inst=api->getInstrument(pos);
            if(!inst) continue;
            const std::string id=inst->InstrumentID;
            if(!filter.empty()&&id.find(filter)==std::string::npos) continue;
            std::cout<<id<<std::endl;
            ++matched;
        }

        std::cout<<"----------------------------------------"<<std::endl;
        std::cout<<"Matched instruments: "<<matched<<std::endl;
        std::cout<<"Read-only tool: no orders or cancels were sent."<<std::endl;
        return matched>0?0:1;
    }catch(const std::exception& e){
        std::cerr<<"ERROR: "<<e.what()<<std::endl;
        return 2;
    }
}
