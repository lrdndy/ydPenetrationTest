#include "core/SystemOrderIdentity.h"
#include <iostream>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition)return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}
}

int main() {
    using ydtest::SystemOrderIdentity;
    using ydtest::SystemOrderIdEvidence;
    using ydtest::hasAssignedSystemOrderId;
    using ydtest::isAssignedYdId;
    using ydtest::observeSystemOrderId;
    using ydtest::preferredSystemOrderId;

    expect(!isAssignedYdId(-1), "-1 is an unassigned YD ID sentinel");
    expect(!isAssignedYdId(0), "0 is an unassigned YD ID sentinel");
    expect(isAssignedYdId(14706), "positive YD ID is assigned");
    expect(!hasAssignedSystemOrderId(-1, -1), "negative system IDs are not assigned");
    expect(!hasAssignedSystemOrderId(-1, 0), "mixed negative/zero system IDs are not assigned");
    expect(!hasAssignedSystemOrderId(0, -1), "mixed zero/negative system IDs are not assigned");
    expect(!hasAssignedSystemOrderId(0, 0), "zero system IDs are not assigned");
    expect(preferredSystemOrderId(-1, -1) == 0, "unassigned system ID has no preferred value");
    expect(preferredSystemOrderId(14706, -1) == 14706, "positive short system ID is preferred over an invalid long ID");
    expect(preferredSystemOrderId(-1, 14706) == 14706, "positive long system ID is preferred over an invalid short ID");

    SystemOrderIdentity observed;
    expect(observed.acceptAndUpdate(-1, -1), "early callback without system ID is accepted");
    expect(!observed.bound(), "early -1 callback does not bind identity");
    expect(observed.acceptAndUpdate(0, 0), "early zero callback is accepted");
    expect(!observed.bound(), "early zero callback does not bind identity");
    expect(observed.acceptAndUpdate(14706, 14706), "first positive system ID binds identity");
    expect(observed.bound(), "identity is bound after positive IDs arrive");
    expect(observed.orderSysId() == 14706 && observed.longOrderSysId() == 14706, "both assigned IDs are retained");
    expect(observed.matches(14706, 14706), "matching full identity is accepted");
    expect(observed.matches(-1, 14706), "matching positive long ID is sufficient");
    expect(observed.matches(14706, -1), "matching positive short ID is sufficient");
    expect(!observed.matches(-1, -1), "missing identity is rejected after binding");
    expect(!observed.acceptAndUpdate(14707, 14707), "different positive identity is rejected");
    expect(!observed.acceptAndUpdate(14707, 14706), "matching long ID cannot hide a short-ID conflict");
    expect(!observed.acceptAndUpdate(14706, 14707), "matching short ID cannot hide a long-ID conflict");
    expect(observed.orderSysId() == 14706 && observed.longOrderSysId() == 14706, "mismatch cannot replace bound identity");

    SystemOrderIdentity partial;
    expect(partial.acceptAndUpdate(14706, -1), "single positive short ID can bind");
    expect(partial.bound() && partial.longOrderSysId() == 0, "negative long ID is normalized to unassigned");
    expect(partial.acceptAndUpdate(14706, 14706), "matching callback can enrich missing long ID");
    expect(partial.longOrderSysId() == 14706, "enriched long ID is retained");

    SystemOrderIdentity partialLong;
    expect(partialLong.acceptAndUpdate(-1, 90001), "single positive long ID can bind");
    expect(partialLong.acceptAndUpdate(14706, 90001), "shared long ID permits short-ID enrichment");
    expect(partialLong.orderSysId() == 14706 && partialLong.longOrderSysId() == 90001, "both enriched fields are retained");
    expect(!partialLong.acceptAndUpdate(14707, 90001), "short-ID conflict is rejected even when long ID matches");
    expect(!partialLong.acceptAndUpdate(14706, 90002), "long-ID conflict is rejected even when short ID matches");

    SystemOrderIdentity noOverlap;
    expect(noOverlap.acceptAndUpdate(14706, -1), "short-only identity binds");
    expect(!noOverlap.acceptAndUpdate(-1, 14706), "callback without a shared positive field cannot replace identity");

    SystemOrderIdentity rejected;
    expect(observeSystemOrderId(rejected, SystemOrderIdEvidence::ValidateOnly, 14706, 14706), "unbound rejected callback is accepted");
    expect(!rejected.bound(), "rejected callback cannot establish identity");
    expect(rejected.acceptAndUpdate(14706, 14706), "accepted callback establishes identity after rejection");
    expect(observeSystemOrderId(rejected, SystemOrderIdEvidence::ValidateOnly, -1, -1), "bound identity accepts rejected callback without IDs");
    expect(rejected.orderSysId() == 14706 && rejected.longOrderSysId() == 14706, "rejected callback without IDs leaves identity unchanged");
    expect(observeSystemOrderId(rejected, SystemOrderIdEvidence::ValidateOnly, 14706, -1), "rejected callback may validate a matching positive ID");
    expect(!observeSystemOrderId(rejected, SystemOrderIdEvidence::ValidateOnly, 14707, -1), "rejected callback with a conflicting positive ID is rejected");

    SystemOrderIdentity errorCallback;
    expect(observeSystemOrderId(errorCallback, SystemOrderIdEvidence::Ignore, 99999, -1), "error callback union value is ignored");
    expect(!errorCallback.bound(), "error callback union value cannot establish identity");
    expect(errorCallback.acceptAndUpdate(14706, 14706), "later accepted callback establishes the real identity");
    expect(observeSystemOrderId(errorCallback, SystemOrderIdEvidence::Ignore, 99999, 99999), "bound identity ignores error callback union values");
    expect(errorCallback.orderSysId() == 14706 && errorCallback.longOrderSysId() == 14706, "error callback cannot replace bound identity");

    if (failures == 0)std::cout << "SystemOrderIdentity regression tests passed\n";
    return failures == 0 ? 0 : 1;
}
