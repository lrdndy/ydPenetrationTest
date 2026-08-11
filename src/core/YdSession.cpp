#include "core/YdSession.h"
#include <cstring>
namespace ydtest {
YdSession::YdSession(std::string c,std::string u,std::string p,Logger& l):config_(std::move(c)),username_(std::move(u)),password_(std::move(p)),log_(l){}
YdSession::~YdSession(){ stop(); }
bool YdSession::start(){api_=makeYDApi(config_.c_str());if(!api_){log_.error("SYSTEM","makeYDApi failed: "+config_);return false;}if(!api_->start(this)){log_.error("SYSTEM","api->start failed");api_=nullptr;return false;}log_.info("SYSTEM","YD API started; waiting for connection/login callbacks");return true;}
void YdSession::stop(){YDApi* p=nullptr;{std::lock_guard<std::mutex>lk(mu_);if(!api_||destroying_)return;destroying_=true;p=api_;}log_.info("SYSTEM","startDestroy");p->startDestroy();waitState(10,[&]{return destroyed_;});{std::lock_guard<std::mutex>lk(mu_);api_=nullptr;}}
bool YdSession::waitConnected(int s){return waitState(s,[&]{return connected_;});}
bool YdSession::waitDisconnected(int s){return waitState(s,[&]{return disconnectedSeen_;});}
bool YdSession::waitLogin(int s){return waitState(s,[&]{return loginDone_;});}
bool YdSession::waitInit(int s){return waitState(s,[&]{return initDone_;});}
bool YdSession::waitCaughtUp(int s){return waitState(s,[&]{return caughtUp_;});}
int YdSession::loginError() const{std::lock_guard<std::mutex>lk(mu_);return loginError_;}
int YdSession::maxOrderRef() const{std::lock_guard<std::mutex>lk(mu_);return maxOrderRef_;}
const YDInstrument* YdSession::instrument(const std::string&id)const{return api_?api_->getInstrumentByID(id.c_str()):nullptr;}
bool YdSession::subscribe(const YDInstrument*i){return api_&&i&&api_->subscribe(i);}
int YdSession::sendLimitOrder(const YDInstrument*i,int dir,int off,double price,int vol,int hedge){if(!api_||!i)return -1;YDInputOrder o;std::memset(&o,0,sizeof(o));{std::lock_guard<std::mutex>lk(mu_);o.OrderRef=nextOrderRef_++;}o.Direction=static_cast<char>(dir);o.OffsetFlag=static_cast<char>(off);o.HedgeFlag=static_cast<char>(hedge);o.ConnectionSelectionType=YD_CS_Any;o.Price=price;o.OrderVolume=vol;o.OrderType=YD_ODT_Limit;o.YDOrderFlag=YD_YOF_Normal;if(!api_->insertOrder(&o,i))return -1;return o.OrderRef;}
bool YdSession::cancelOrder(const YDInstrument*i,const YDOrder&o){if(!api_||!i)return false;YDCancelOrder c;std::memset(&c,0,sizeof(c));c.LongOrderSysID=o.LongOrderSysID;if(c.LongOrderSysID==0)c.OrderSysID=o.OrderSysID;return api_->cancelOrder(&c,i->m_pExchange,nullptr);}
bool YdSession::cancelMulti(const std::vector<std::pair<const YDInstrument*,YDOrder>>&){return false;}
bool YdSession::waitOrder(int ref,int s,const std::function<bool(const YDOrder&)>&pred,YDOrder&out){std::unique_lock<std::mutex>lk(mu_);if(!cv_.wait_for(lk,std::chrono::seconds(s),[&]{auto it=orders_.find(ref);return it!=orders_.end()&&pred(it->second);} ))return false;out=orders_[ref];return true;}
bool YdSession::waitTrade(int ref,int s,YDTrade&out){std::unique_lock<std::mutex>lk(mu_);if(!cv_.wait_for(lk,std::chrono::seconds(s),[&]{for(auto&t:trades_)if(t.OrderRef==ref)return true;return false;}))return false;for(auto&t:trades_)if(t.OrderRef==ref){out=t;return true;}return false;}
void YdSession::disconnectNow(){if(api_)api_->disconnect();}
void YdSession::notifyAfterApiDestroy(){std::lock_guard<std::mutex>lk(mu_);destroyed_=true;cv_.notify_all();log_.info("SYSTEM","YD API destroyed");}
void YdSession::notifyEvent(int e){std::lock_guard<std::mutex>lk(mu_);if(e==YD_AE_TCPTradeConnected)connected_=true;else if(e==YD_AE_TCPTradeDisconnected){connected_=false;disconnectedSeen_=true;caughtUp_=false;}cv_.notify_all();log_.info("API_EVENT","event="+std::to_string(e));}
void YdSession::notifyReadyForLogin(bool failed){log_.info("LOGIN",std::string("notifyReadyForLogin hasLoginFailed=")+(failed?"true":"false"));if(api_)api_->login(username_.c_str(),password_.c_str(),nullptr,nullptr);}
void YdSession::notifyLogin(int err,int maxRef,bool){std::lock_guard<std::mutex>lk(mu_);loginError_=err;loginDone_=true;maxOrderRef_=maxRef;if(err==0)nextOrderRef_=std::max(1,maxRef+1);cv_.notify_all();}
void YdSession::notifyFinishInit(){std::lock_guard<std::mutex>lk(mu_);initDone_=true;cv_.notify_all();}
void YdSession::notifyCaughtUp(){std::lock_guard<std::mutex>lk(mu_);caughtUp_=true;cv_.notify_all();}
void YdSession::notifyOrder(const YDOrder*o,const YDInstrument*,const YDAccount*){if(!o)return;{std::lock_guard<std::mutex>lk(mu_);orders_[o->OrderRef]=*o;cv_.notify_all();}}
void YdSession::notifyTrade(const YDTrade*t,const YDInstrument*,const YDAccount*){if(!t)return;{std::lock_guard<std::mutex>lk(mu_);trades_.push_back(*t);cv_.notify_all();}}
void YdSession::notifyFailedCancelOrder(const YDFailedCancelOrder*,const YDExchange*,const YDAccount*){}
void YdSession::notifyMarketData(const YDMarketData*m){if(!m)return;{std::lock_guard<std::mutex>lk(mu_);market_[m->InstrumentRef]=*m;cv_.notify_all();}}
}
