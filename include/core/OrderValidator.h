#pragma once
#include "core/Common.h"
#include "ydApi.h"

namespace ydtest {
struct ValidationResult { bool ok; std::string reason; };
class OrderValidator {
public:
    ValidationResult instrument(YDApi* api, const std::string& id) const;
    ValidationResult limitPrice(const YDInstrument* inst, double price) const;
    ValidationResult limitVolume(const YDInstrument* inst, int volume) const;
};
}
