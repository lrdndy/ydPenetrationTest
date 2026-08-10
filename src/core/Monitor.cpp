#include "core/Monitor.h"
namespace ydtest {
Monitor::Monitor(Logger& l,int o,int c,int d):log_(l),orderThreshold_(o),cancelThreshold_(c),duplicateThreshold_(d){}
std::string Monitor::key(const OrderIntent& x) const { std::ostringstream s; s<<x.cancel<<'|'<<x.instrument<<'|'<<x.direction<<'|'<<x.offset<<'|'<<std::fixed<<std::setprecision(8)<<x.price<<'|'<<x.volume; return s.str(); }
void Monitor::recordOrder(const OrderIntent& x){ ++orderCount_; const int n=++seen_[key(x)]; if(n>1) ++duplicateCount_; log_.info("MONITOR","orderCount="+std::to_string(orderCount_)+" duplicateCount="+std::to_string(duplicateCount_)); checkThresholds(); }
void Monitor::recordCancel(const OrderIntent& x){ ++cancelCount_; const int n=++seen_[key(x)]; if(n>1) ++duplicateCount_; log_.info("MONITOR","cancelCount="+std::to_string(cancelCount_)+" duplicateCount="+std::to_string(duplicateCount_)); checkThresholds(); }
void Monitor::checkThresholds(){
    if(orderThreshold_>0 && orderCount_>=orderThreshold_ && !orderAlerted_){orderAlerted_=true;log_.warn("ALERT","ORDER_COUNT_THRESHOLD value="+std::to_string(orderCount_)+" threshold="+std::to_string(orderThreshold_));}
    if(cancelThreshold_>0 && cancelCount_>=cancelThreshold_ && !cancelAlerted_){cancelAlerted_=true;log_.warn("ALERT","CANCEL_COUNT_THRESHOLD value="+std::to_string(cancelCount_)+" threshold="+std::to_string(cancelThreshold_));}
    if(duplicateThreshold_>0 && duplicateCount_>=duplicateThreshold_ && !duplicateAlerted_){duplicateAlerted_=true;log_.warn("ALERT","DUPLICATE_ORDER_THRESHOLD value="+std::to_string(duplicateCount_)+" threshold="+std::to_string(duplicateThreshold_));}
}
}
