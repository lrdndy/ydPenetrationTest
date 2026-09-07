#include "core/OrderValidator.h"
#include <cmath>
namespace ydtest {
ValidationResult OrderValidator::instrument(YDApi* api,const std::string& id) const {
    if(!api) return {false,"API not ready"};
    if(!api->getInstrumentByID(id.c_str())) return {false,"instrument not found"};
    return {true,""};
}
ValidationResult OrderValidator::limitPrice(const YDInstrument* i,double p) const {
    if(!i || i->Tick<=0) return {false,"invalid instrument/tick"};
    if(!std::isfinite(p)||p<=0){ std::ostringstream s; s<<"price="<<p<<" must be positive and finite"; return {false,s.str()}; }
    const double units=p/i->Tick, nearest=std::round(units);
    if(std::fabs(units-nearest)>1e-7){ std::ostringstream s; s<<"price="<<p<<" is not multiple of Tick="<<i->Tick; return {false,s.str()}; }
    return {true,""};
}
ValidationResult OrderValidator::limitPriceRange(const YDInstrument* i,double p) const {
    if(!i || i->Tick<=0) return {false,"invalid instrument/tick"};
    const YDMarketData* md=i->m_pMarketData;
    if(!md) return {true,""};
    const double lower=md->LowerLimitPrice, upper=md->UpperLimitPrice;
    const double tolerance=i->Tick*1e-6;
    if(std::isfinite(lower)&&lower>0&&p<lower-tolerance){
        std::ostringstream s; s<<"price="<<p<<" is below lower limit="<<lower; return {false,s.str()};
    }
    if(std::isfinite(upper)&&upper>0&&p>upper+tolerance){
        std::ostringstream s; s<<"price="<<p<<" is above upper limit="<<upper; return {false,s.str()};
    }
    return {true,""};
}
ValidationResult OrderValidator::limitVolume(const YDInstrument* i,int v) const {
    if(!i) return {false,"null instrument"};
    if(v<i->MinLimitOrderVolume || v>i->MaxLimitOrderVolume || (i->MinLimitOrderVolume>0 && v%i->MinLimitOrderVolume!=0)){
        std::ostringstream s; s<<"volume="<<v<<" allowed=["<<i->MinLimitOrderVolume<<','<<i->MaxLimitOrderVolume<<"] step="<<i->MinLimitOrderVolume; return {false,s.str()};
    }
    return {true,""};
}
}
