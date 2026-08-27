#pragma once

namespace ydtest {

inline bool isAssignedYdId(long long value) noexcept {
    return value > 0;
}

inline bool hasAssignedSystemOrderId(long long orderSysId, long long longOrderSysId) noexcept {
    return isAssignedYdId(orderSysId) || isAssignedYdId(longOrderSysId);
}

inline long long preferredSystemOrderId(long long orderSysId, long long longOrderSysId) noexcept {
    if (isAssignedYdId(longOrderSysId))return longOrderSysId;
    return isAssignedYdId(orderSysId) ? orderSysId : 0;
}

inline bool compatibleAssignedSystemOrderIds(long long boundOrderSysId, long long boundLongOrderSysId,
    long long orderSysId, long long longOrderSysId) noexcept {
    if (!hasAssignedSystemOrderId(boundOrderSysId, boundLongOrderSysId)
        || !hasAssignedSystemOrderId(orderSysId, longOrderSysId))return false;
    bool compared = false;
    if (isAssignedYdId(boundLongOrderSysId) && isAssignedYdId(longOrderSysId)) {
        compared = true;
        if (boundLongOrderSysId != longOrderSysId)return false;
    }
    if (isAssignedYdId(boundOrderSysId) && isAssignedYdId(orderSysId)) {
        compared = true;
        if (boundOrderSysId != orderSysId)return false;
    }
    return compared;
}

class SystemOrderIdentity {
public:
    bool bound() const noexcept {
        return hasAssignedSystemOrderId(orderSysId_, longOrderSysId_);
    }

    long long orderSysId() const noexcept { return orderSysId_; }
    long long longOrderSysId() const noexcept { return longOrderSysId_; }

    bool matches(long long orderSysId, long long longOrderSysId) const noexcept {
        return compatibleAssignedSystemOrderIds(orderSysId_, longOrderSysId_, orderSysId, longOrderSysId);
    }

    // An early callback may carry 0/-1 before the cabinet assigns an ID. Accept
    // that callback without binding; the first positive ID establishes identity.
    bool acceptAndUpdate(long long orderSysId, long long longOrderSysId) noexcept {
        if (!bound()) {
            if (isAssignedYdId(orderSysId))orderSysId_ = orderSysId;
            if (isAssignedYdId(longOrderSysId))longOrderSysId_ = longOrderSysId;
            return true;
        }
        if (!matches(orderSysId, longOrderSysId))return false;
        if (!isAssignedYdId(orderSysId_) && isAssignedYdId(orderSysId))orderSysId_ = orderSysId;
        if (!isAssignedYdId(longOrderSysId_) && isAssignedYdId(longOrderSysId))longOrderSysId_ = longOrderSysId;
        return true;
    }

private:
    long long orderSysId_ = 0;
    long long longOrderSysId_ = 0;
};

enum class SystemOrderIdEvidence {
    Ignore,
    ValidateOnly,
    BindOrMatch
};

// Error callbacks may store MaxOrderRef in the OrderSysID union, so they must
// not bind or validate system identity. A non-error rejected callback may
// validate a positive ID, but it must never establish or enrich the binding.
inline bool observeSystemOrderId(SystemOrderIdentity& identity, SystemOrderIdEvidence evidence,
    long long orderSysId, long long longOrderSysId) noexcept {
    if (evidence == SystemOrderIdEvidence::Ignore)return true;
    if (evidence == SystemOrderIdEvidence::BindOrMatch)return identity.acceptAndUpdate(orderSysId, longOrderSysId);
    if (!identity.bound() || !hasAssignedSystemOrderId(orderSysId, longOrderSysId))return true;
    return identity.matches(orderSysId, longOrderSysId);
}

}
