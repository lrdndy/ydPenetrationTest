#pragma once
#include "core/Common.h"
#include "core/Logger.h"
#include "core/Monitor.h"
#include "core/SystemOrderIdentity.h"
#include "ydApi.h"

namespace ydtest {
struct SessionEventGenerations {
    std::uint64_t current=0;
    std::uint64_t tradeConnected=0;
    std::uint64_t tradeDisconnected=0;
    std::uint64_t login=0;
    std::uint64_t caughtUp=0;
    bool tradeConnectedState=false;
    bool loginCompleted=false;
    bool caughtUpState=false;
    int loginError=-999;
    std::string tradeConnectedTime;
    std::string tradeDisconnectedTime;
    std::string loginTime;
    std::string caughtUpTime;
};

struct OrderActivitySnapshot {
    std::uint64_t orderApiRequests=0;
    std::uint64_t orderRequestsSubmitted=0;
    std::uint64_t uniqueAcceptedOrders=0;
    std::uint64_t cancelApiRequests=0;
    std::uint64_t cancelRequestsSubmitted=0;
    std::uint64_t confirmedCancellations=0;
    std::uint64_t failedCancelCallbacks=0;
    std::uint64_t callbackValidationFailures=0;
};

struct BatchCancelActivitySnapshot {
    std::uint64_t apiCalls=0;
    std::uint64_t apiCallsSubmitted=0;
    std::uint64_t targetOrdersRequested=0;
    std::uint64_t targetOrdersSubmitted=0;
};

struct OrderStreamSnapshot {
    OrderActivitySnapshot activity;
    std::uint64_t activityGeneration=0;
    std::uint64_t sessionGeneration=0;
    bool sessionReady=false;
    bool destroying=false;
    bool destroyed=false;
    std::unordered_map<int,YDOrder> orders;
    std::unordered_map<int,std::int64_t> tradeVolumeByOrderRef;
};

struct InstructionValidationSnapshot {
    std::uint64_t invalidInstrument=0;
    std::uint64_t invalidLimitPrice=0;
    std::uint64_t invalidLimitVolume=0;
};

struct TradingControlSnapshot {
    bool paused=false;
    std::uint64_t blockedOrderInstructions=0;
};

class YdSession : public YDListener {
public:
    YdSession(std::string config, std::string username, std::string password, Logger& log, bool useExtendedApi = false, MonitorThresholds monitorThresholds = {}, bool liveOrderSubmissionEnabled = false);
    ~YdSession() override;
    bool start();
    void stop();
    bool waitConnected(int sec);
    bool waitDisconnected(int sec);
    bool waitLogin(int sec);
    bool waitInit(int sec);
    bool waitCaughtUp(int sec);
    SessionEventGenerations eventGenerations() const;
    bool waitTradeConnectedAfter(std::uint64_t generation,int sec);
    bool waitTradeDisconnectedAfter(std::uint64_t generation,int sec);
    bool waitLoginAfter(std::uint64_t generation,int sec);
    bool waitCaughtUpAfter(std::uint64_t generation,int sec);
    OrderActivitySnapshot orderActivity() const;
    BatchCancelActivitySnapshot batchCancelActivity() const;
    InstructionValidationSnapshot instructionValidation() const;
    OrderStreamSnapshot orderStreamSnapshot() const;
    struct HistoricalCallbackCounts {
        std::uint64_t order = 0;
        std::uint64_t trade = 0;
        std::uint64_t cancel = 0;
        std::uint64_t rejected = 0;
    };
    HistoricalCallbackCounts historicalCallbackCounts() const;
    HistoricalCallbackCounts historicalCallbackCounts(const std::string& instrumentId) const;
    bool waitOrderActivityQuiet(int quietMilliseconds,int maxSeconds,OrderStreamSnapshot& out);
    bool stopIfOrderStreamUnchanged(std::uint64_t activityGeneration,std::uint64_t sessionGeneration,OrderStreamSnapshot& out);
    bool waitMarketData(int instrumentRef, int sec, YDMarketData& out);
    bool waitNextMarketData(int instrumentRef, std::uint64_t& version, int sec, YDMarketData& out);
    const YDInstrument* instrument(const std::string& id) const;
    bool subscribe(const YDInstrument* inst);
    void pauseTrading(const std::string& source="MANUAL");
    void resumeTrading(const std::string& source="MANUAL");
    TradingControlSnapshot tradingControl() const;
    int sendLimitOrder(const std::string& instrumentId,int direction,int offset,double price,int volume,int hedge=YD_HF_Speculation);
    int sendLimitOrder(const YDInstrument* inst,int direction,int offset,double price,int volume,int hedge=YD_HF_Speculation);
    bool cancelOrder(const YDInstrument* inst,const YDOrder& order);
    bool cancelMulti(const std::vector<std::pair<const YDInstrument*,YDOrder>>& orders);
    bool waitOrder(int orderRef,int sec,const std::function<bool(const YDOrder&)>& pred,YDOrder& out);
    int waitCancelTerminal(int orderRef,int sec,YDOrder& out);
    bool waitTrade(int orderRef,int sec,YDTrade& out);
    bool disconnectNow(std::uint64_t& beforeGeneration);
    int loginError() const;
    int maxOrderRef() const;
    YDApi* api() const { return api_; }
    YDExtendedApi* extendedApi() const { return extendedApi_; }

    void notifyAfterApiDestroy() override;
    void notifyEvent(int apiEvent) override;
    void notifyReadyForLogin(bool hasLoginFailed) override;
    void notifyLogin(int errorNo,int maxOrderRef,bool isMonitor) override;
    void notifyFinishInit() override;
    void notifyCaughtUp() override;
    void notifyOrder(const YDOrder*,const YDInstrument*,const YDAccount*) override;
    void notifyTrade(const YDTrade*,const YDInstrument*,const YDAccount*) override;
    void notifyFailedCancelOrder(const YDFailedCancelOrder*,const YDExchange*,const YDAccount*) override;
    void notifyMarketData(const YDMarketData*) override;
private:
    struct OwnedOrderState {
        int accountRef=0;
        int instrumentRef=0;
        char direction=0;
        char offset=0;
        char hedge=0;
        char orderType=0;
        char orderFlag=0;
        double price=0;
        double tick=0;
        int volume=0;
        bool apiCallStarted=false;
        bool apiSubmitted=false;
        bool acceptedObserved=false;
        SystemOrderIdentity systemIdentity;
        bool hasOrder=false;
        YDOrder latest{};
        std::uint64_t lastOrderCallbackGeneration=0;
    };
    struct CancelAttemptState {
        std::uint64_t attemptId=0;
        std::uint64_t afterOrderCallbackGeneration=0;
        std::uint64_t canceledCallbackGeneration=0;
        bool apiReturned=false;
        bool submitted=false;
        bool confirmed=false;
        bool failed=false;
        int errorNo=0;
    };
    template<class Pred> bool waitState(int sec, Pred p){ std::unique_lock<std::mutex> lk(mu_); return cv_.wait_for(lk,std::chrono::seconds(sec),p); }
    int sendLimitOrderUnlocked(const YDInstrument* inst,const std::string& instrumentId,int direction,int offset,double price,int volume,int hedge);
    bool rejectOrderWhilePaused(const std::string& instrumentId,int direction,int offset,double price,int volume);
    void logInstructionRejected(const std::string& validation,const std::string& instrumentId,int direction,int offset,double price,int volume,const std::string& reason);
    void logInstructionMonitoringSummary();
    std::string config_,username_,password_,appId_;
    Logger& log_;
    Monitor instructionMonitor_;
    TradingGate tradingGate_;
    mutable std::mutex tradingControlMu_;
    std::string tradingPauseSource_="NONE";
    std::uint64_t blockedOrderInstructions_=0;
    bool instructionMonitoringSummaryLogged_=false;
    bool useExtendedApi_=false;
    bool liveOrderSubmissionEnabled_=false;
    YDApi* api_=nullptr;
    YDExtendedApi* extendedApi_=nullptr;
    std::mutex apiLifecycleMu_;
    std::condition_variable apiLifecycleCv_;
    std::size_t activeLoginApiCalls_=0;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool connected_=false,disconnectedSeen_=false,loginDone_=false,initDone_=false,caughtUp_=false,destroyed_=false,destroying_=false;
    int loginError_=-999,maxOrderRef_=0,nextOrderRef_=1;
    std::unordered_map<int,OwnedOrderState> ownedOrders_;
    std::unordered_map<int,CancelAttemptState> cancelAttempts_;
    std::vector<YDTrade> trades_;
    std::vector<YDTrade> ownedTrades_;
    std::set<int> acceptedOrderRefs_;
    std::uint64_t orderApiRequests_=0,orderRequestsSubmitted_=0;
    std::uint64_t cancelApiRequests_=0,cancelRequestsSubmitted_=0;
    std::uint64_t batchCancelApiCalls_=0,batchCancelApiCallsSubmitted_=0;
    std::uint64_t batchCancelOrdersRequested_=0,batchCancelOrdersSubmitted_=0;
    std::uint64_t confirmedCancellations_=0,failedCancelCallbacks_=0;
    std::uint64_t callbackValidationFailures_=0,orderActivityGeneration_=0;
    std::uint64_t invalidInstrumentInstructions_=0,invalidLimitPriceInstructions_=0,invalidLimitVolumeInstructions_=0;
    std::uint64_t orderCallbackGeneration_=0,nextCancelAttemptId_=1;
    std::uint64_t eventGeneration_=0,tradeConnectedGeneration_=0,tradeDisconnectedGeneration_=0,loginGeneration_=0,caughtUpGeneration_=0;
    std::string tradeConnectedTime_,tradeDisconnectedTime_,loginTime_,caughtUpTime_;
    std::size_t historicalOrderCallbacks_=0,historicalTradeCallbacks_=0;
    std::size_t historicalCancelCallbacks_=0,historicalRejectedCallbacks_=0;
    std::unordered_map<std::string,HistoricalCallbackCounts> historicalByInstrument_;
    std::unordered_map<int,YDMarketData> market_;
    std::unordered_map<int,std::uint64_t> marketVersions_;
    std::uint64_t marketVersion_=0;
    OrderStreamSnapshot orderStreamSnapshotLocked() const;
};
}
