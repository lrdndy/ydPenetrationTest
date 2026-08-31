#include "core/YdSession.h"
#include "core/OrderValidator.h"
#include "ydError.h"
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>
namespace ydtest {
	namespace {
		const char* directionName(int value) { return value == YD_D_Buy ? "BUY" : (value == YD_D_Sell ? "SELL" : "UNKNOWN"); }
		const char* offsetName(int value) {
			switch (value) { case YD_OF_Open:return "OPEN"; case YD_OF_Close:return "CLOSE"; case YD_OF_ForceClose:return "FORCE_CLOSE"; case YD_OF_CloseToday:return "CLOSE_TODAY"; case YD_OF_CloseYesterday:return "CLOSE_YESTERDAY"; default:return "UNKNOWN"; }
		}
		const char* orderStatusName(int value) {
			switch (value) { case YD_OS_Accepted:return "ACCEPTED"; case YD_OS_Queuing:return "QUEUING"; case YD_OS_Canceled:return "CANCELED"; case YD_OS_AllTraded:return "ALL_TRADED"; case YD_OS_Rejected:return "REJECTED"; default:return "UNKNOWN"; }
		}
		const char* ydErrorName(int value) {
			switch (value) {
			case YD_ERROR_NoError:return "NO_ERROR";
			case YD_ERROR_NoPositionToClose:return "NO_POSITION_TO_CLOSE";
			case YD_ERROR_NoMoneyToOpen:return "NO_MONEY_TO_OPEN";
			case YD_ERROR_InstrumentCanNotTrade:return "INSTRUMENT_CANNOT_TRADE";
			case YD_ERROR_NotProperTime:return "NOT_PROPER_TIME";
			case YD_ERROR_InstrumentTradingPaused:return "INSTRUMENT_TRADING_PAUSED";
			case YD_ERROR_CannotTradeInCurrentSegment:return "CANNOT_TRADE_IN_CURRENT_SEGMENT";
			case YD_ERROR_SSEATPGatewayNoTradingTime:return "NO_TRADING_TIME";
			default:return "YD_ERROR";
			}
		}
		std::string numberText(double value) {
			if (!std::isfinite(value) || value == DBL_MAX || value == -DBL_MAX)return "N/A";
			std::ostringstream out; out << std::fixed << std::setprecision(10) << value;
			auto text = out.str(); while (text.size() > 1 && text.back() == '0')text.pop_back(); if (!text.empty() && text.back() == '.')text.pop_back(); return text;
		}
		std::string accountName(const YDAccount* account, const std::string& fallback) { return account && account->AccountID[0] ? std::string(account->AccountID) : fallback; }
		bool samePrice(double expected, double actual, double tick) {
			return std::isfinite(actual) && std::fabs(expected - actual) <= std::max(1e-9, std::max(std::fabs(expected) * 1e-12, std::fabs(tick) * 1e-6));
		}
		std::string systemOrderIdText(long long orderSysId, long long longOrderSysId) {
			const long long value = preferredSystemOrderId(orderSysId, longOrderSysId);
			return value > 0 ? std::to_string(value) : std::string("N/A");
		}
		const char* apiEventName(int value) {
			switch (value) {
			case YD_AE_TCPTradeConnected:return "TCP_TRADE_CONNECTED";
			case YD_AE_TCPTradeDisconnected:return "TCP_TRADE_DISCONNECTED";
			case YD_AE_TCPMarketDataConnected:return "TCP_MARKET_DATA_CONNECTED";
			case YD_AE_TCPMarketDataDisconnected:return "TCP_MARKET_DATA_DISCONNECTED";
			case YD_AE_ServerRestarted:return "SERVER_RESTARTED";
			case YD_AE_ServerSwitched:return "SERVER_SWITCHED";
			case YD_AE_XTCPTradeConnected:return "XTCP_TRADE_CONNECTED";
			case YD_AE_XTCPTradeDisconnected:return "XTCP_TRADE_DISCONNECTED";
			default:return "UNKNOWN";
			}
		}
	}
	YdSession::YdSession(std::string c, std::string u, std::string p, Logger& l, bool useExtendedApi, MonitorThresholds monitorThresholds, bool liveOrderSubmissionEnabled) :config_(std::move(c)), username_(std::move(u)), password_(std::move(p)), log_(l), instructionMonitor_(log_, username_, monitorThresholds), useExtendedApi_(useExtendedApi), liveOrderSubmissionEnabled_(liveOrderSubmissionEnabled) {}
	YdSession::~YdSession() { stop(); }
	bool YdSession::start() {
		extendedApi_ = useExtendedApi_ ? makeYDExtendedApi(config_.c_str()) : nullptr;
		api_ = useExtendedApi_ ? static_cast<YDApi*>(extendedApi_) : makeYDApi(config_.c_str());
		if (!api_) { log_.error("SYSTEM", std::string(useExtendedApi_ ? "makeYDExtendedApi failed: " : "makeYDApi failed: ") + config_); return false; }
		if (!api_->start(this)) { log_.error("SYSTEM", "api->start failed"); api_ = nullptr; extendedApi_ = nullptr; return false; }
		log_.info("SYSTEM", "YD API started; waiting for connection/login callbacks"); return true;
	}
	void YdSession::stop() {
		logInstructionMonitoringSummary();
		YDApi* p = nullptr;
		{
			std::unique_lock<std::mutex> apiLifecycleLock(apiLifecycleMu_);
			{ std::lock_guard<std::mutex> lk(mu_); if (!api_ || destroying_) return; destroying_ = true; p = api_; }
			apiLifecycleCv_.wait(apiLifecycleLock, [&] {return activeLoginApiCalls_ == 0; });
		}
		log_.info("SYSTEM", "startDestroy"); p->startDestroy();
		{ std::unique_lock<std::mutex> lk(mu_); cv_.wait(lk, [&] {return destroyed_; }); api_ = nullptr; extendedApi_ = nullptr; }
	}
	void YdSession::logInstructionMonitoringSummary() {
		if (instructionMonitoringSummaryLogged_ || (instructionMonitor_.orderCount() == 0 && instructionMonitor_.cancelCount() == 0))return;
		instructionMonitor_.logRiskStatistics(); instructionMonitor_.logDuplicateStatistics(); instructionMonitoringSummaryLogged_ = true;
	}
	bool YdSession::waitConnected(int s) { return waitState(s, [&] {return connected_; }); }
	bool YdSession::waitDisconnected(int s) { return waitState(s, [&] {return disconnectedSeen_; }); }
	bool YdSession::waitLogin(int s) { return waitState(s, [&] {return loginDone_; }); }
	bool YdSession::waitInit(int s) { return waitState(s, [&] {return initDone_; }); }
	bool YdSession::waitCaughtUp(int s) { return waitState(s, [&] {return caughtUp_; }); }
	SessionEventGenerations YdSession::eventGenerations() const {
		std::lock_guard<std::mutex> lk(mu_); SessionEventGenerations out;
		out.current = eventGeneration_; out.tradeConnected = tradeConnectedGeneration_; out.tradeDisconnected = tradeDisconnectedGeneration_; out.login = loginGeneration_; out.caughtUp = caughtUpGeneration_;
		out.tradeConnectedState = connected_; out.loginCompleted = loginDone_; out.caughtUpState = caughtUp_; out.loginError = loginError_;
		out.tradeConnectedTime = tradeConnectedTime_; out.tradeDisconnectedTime = tradeDisconnectedTime_; out.loginTime = loginTime_; out.caughtUpTime = caughtUpTime_; return out;
	}
	bool YdSession::waitTradeConnectedAfter(std::uint64_t generation, int s) { return waitState(s, [&] {return tradeConnectedGeneration_ > generation; }); }
	bool YdSession::waitTradeDisconnectedAfter(std::uint64_t generation, int s) { return waitState(s, [&] {return tradeDisconnectedGeneration_ > generation; }); }
	bool YdSession::waitLoginAfter(std::uint64_t generation, int s) { return waitState(s, [&] {return loginGeneration_ > generation; }); }
	bool YdSession::waitCaughtUpAfter(std::uint64_t generation, int s) { return waitState(s, [&] {return caughtUpGeneration_ > generation; }); }
	OrderActivitySnapshot YdSession::orderActivity() const { std::lock_guard<std::mutex> lk(mu_); return { orderApiRequests_,orderRequestsSubmitted_,acceptedOrderRefs_.size(),cancelApiRequests_,cancelRequestsSubmitted_,confirmedCancellations_,failedCancelCallbacks_,callbackValidationFailures_ }; }
	YdSession::HistoricalCallbackCounts YdSession::historicalCallbackCounts() const { std::lock_guard<std::mutex> lk(mu_); return { historicalOrderCallbacks_,historicalTradeCallbacks_,historicalCancelCallbacks_,historicalRejectedCallbacks_ }; }
	YdSession::HistoricalCallbackCounts YdSession::historicalCallbackCounts(const std::string& instrumentId) const { std::lock_guard<std::mutex> lk(mu_); const auto it = historicalByInstrument_.find(instrumentId); return it == historicalByInstrument_.end() ? HistoricalCallbackCounts{} : it->second; }
	BatchCancelActivitySnapshot YdSession::batchCancelActivity() const { std::lock_guard<std::mutex> lk(mu_); return { batchCancelApiCalls_,batchCancelApiCallsSubmitted_,batchCancelOrdersRequested_,batchCancelOrdersSubmitted_ }; }
	InstructionValidationSnapshot YdSession::instructionValidation() const { std::lock_guard<std::mutex> lk(mu_); return { invalidInstrumentInstructions_,invalidLimitPriceInstructions_,invalidLimitVolumeInstructions_ }; }
	OrderStreamSnapshot YdSession::orderStreamSnapshotLocked() const {
		OrderStreamSnapshot out;
		out.activity = { orderApiRequests_,orderRequestsSubmitted_,acceptedOrderRefs_.size(),cancelApiRequests_,cancelRequestsSubmitted_,confirmedCancellations_,failedCancelCallbacks_,callbackValidationFailures_ };
		out.activityGeneration = orderActivityGeneration_; out.sessionGeneration = eventGeneration_;
		out.sessionReady = connected_ && loginDone_ && loginError_ == 0 && caughtUp_ && !destroying_ && !destroyed_;
		out.destroying = destroying_; out.destroyed = destroyed_;
		for (const auto& item : ownedOrders_)if (item.second.hasOrder)out.orders.emplace(item.first, item.second.latest);
		for (const auto& trade : ownedTrades_)out.tradeVolumeByOrderRef[trade.OrderRef] += trade.Volume;
		return out;
	}
	OrderStreamSnapshot YdSession::orderStreamSnapshot() const { std::lock_guard<std::mutex> lk(mu_); return orderStreamSnapshotLocked(); }
	bool YdSession::waitOrderActivityQuiet(int quietMilliseconds, int maxSeconds, OrderStreamSnapshot& out) {
		const auto quiet = std::chrono::milliseconds(std::max(1, quietMilliseconds)); const auto overall = std::chrono::seconds(std::max(1, maxSeconds));
		std::unique_lock<std::mutex> lk(mu_);
		const auto ready = [&] {return connected_ && loginDone_ && loginError_ == 0 && caughtUp_ && !destroying_ && !destroyed_; };
		if (!ready())return false;
		std::uint64_t observedActivity = orderActivityGeneration_, observedSession = eventGeneration_;
		const auto deadline = std::chrono::steady_clock::now() + overall; auto quietDeadline = std::chrono::steady_clock::now() + quiet;
		while (std::chrono::steady_clock::now() < deadline) {
			const auto target = std::min(quietDeadline, deadline);
			const bool changed = cv_.wait_until(lk, target, [&] {return destroying_ || destroyed_ || !ready() || orderActivityGeneration_ != observedActivity || eventGeneration_ != observedSession; });
			if (!changed) {
				if (target != quietDeadline || !ready())return false;
				out = orderStreamSnapshotLocked(); return true;
			}
			if (!ready())return false;
			observedActivity = orderActivityGeneration_; observedSession = eventGeneration_; quietDeadline = std::chrono::steady_clock::now() + quiet;
		}
		return false;
	}
	bool YdSession::stopIfOrderStreamUnchanged(std::uint64_t activityGeneration, std::uint64_t sessionGeneration, OrderStreamSnapshot& out) {
		YDApi* p = nullptr;
		{ std::lock_guard<std::mutex> lk(mu_);
			const bool ready = connected_ && loginDone_ && loginError_ == 0 && caughtUp_ && !destroying_ && !destroyed_;
			if (!api_ || !ready || orderActivityGeneration_ != activityGeneration || eventGeneration_ != sessionGeneration) { out = orderStreamSnapshotLocked(); return false; }
			p = api_;
		}
		{
			std::unique_lock<std::mutex> apiLifecycleLock(apiLifecycleMu_);
			{ std::lock_guard<std::mutex> lk(mu_);
				const bool ready = connected_ && loginDone_ && loginError_ == 0 && caughtUp_ && !destroying_ && !destroyed_;
				if (api_ != p || !ready || orderActivityGeneration_ != activityGeneration || eventGeneration_ != sessionGeneration) { out = orderStreamSnapshotLocked(); return false; }
				destroying_ = true;
			}
			apiLifecycleCv_.wait(apiLifecycleLock, [&] {return activeLoginApiCalls_ == 0; });
		}
		log_.info("SYSTEM", "startDestroy after stable owned-order snapshot");
		p->startDestroy();
		{ std::unique_lock<std::mutex> lk(mu_); cv_.wait(lk, [&] {return destroyed_; }); api_ = nullptr; extendedApi_ = nullptr; out = orderStreamSnapshotLocked(); }
		return out.activityGeneration == activityGeneration;
	}
	int YdSession::loginError() const { std::lock_guard<std::mutex> lk(mu_); return loginError_; }
	int YdSession::maxOrderRef() const { std::lock_guard<std::mutex> lk(mu_); return maxOrderRef_; }
	const YDInstrument* YdSession::instrument(const std::string& id) const { return api_ ? api_->getInstrumentByID(id.c_str()) : nullptr; }
	bool YdSession::subscribe(const YDInstrument* i) { return api_ && i && api_->subscribe(i); }
	void YdSession::pauseTrading(const std::string& source) {
		bool changed = false;
		{ std::lock_guard<std::mutex> lock(tradingControlMu_); changed = tradingGate_.pause(); tradingPauseSource_ = source.empty() ? "MANUAL" : source; }
		log_.warn("RISK", "account=" + username_ + " event=TRADING_PAUSED source=" + (source.empty() ? std::string("MANUAL") : source) + " newOrdersAllowed=false stateChanged=" + (changed ? std::string("true") : std::string("false")));
	}
	void YdSession::resumeTrading(const std::string& source) {
		bool changed = false;
		{ std::lock_guard<std::mutex> lock(tradingControlMu_); changed = tradingGate_.resume(); tradingPauseSource_ = "NONE"; }
		log_.info("RISK", "account=" + username_ + " event=TRADING_RESUMED source=" + (source.empty() ? std::string("MANUAL") : source) + " newOrdersAllowed=true stateChanged=" + (changed ? std::string("true") : std::string("false")));
	}
	TradingControlSnapshot YdSession::tradingControl() const { std::lock_guard<std::mutex> lock(tradingControlMu_); return { tradingGate_.paused(),blockedOrderInstructions_ }; }
	bool YdSession::waitMarketData(int ref, int s, YDMarketData& out) { std::unique_lock<std::mutex> lk(mu_); if (!cv_.wait_for(lk, std::chrono::seconds(s), [&] {return market_.count(ref) > 0; }))return false; out = market_[ref]; return true; }
	bool YdSession::waitNextMarketData(int ref, std::uint64_t& version, int s, YDMarketData& out) { std::unique_lock<std::mutex> lk(mu_); if (!cv_.wait_for(lk, std::chrono::seconds(s), [&] {auto found = marketVersions_.find(ref); return found != marketVersions_.end() && found->second > version; }))return false; out = market_[ref]; version = marketVersions_[ref]; return true; }
	void YdSession::logInstructionRejected(const std::string& validation, const std::string& instrumentId, int dir, int off, double price, int vol, const std::string& reason) {
		{ std::lock_guard<std::mutex> lk(mu_); if (validation == "INSTRUMENT")++invalidInstrumentInstructions_; else if (validation == "LIMIT_PRICE")++invalidLimitPriceInstructions_; else if (validation == "LIMIT_VOLUME")++invalidLimitVolumeInstructions_; }
		log_.error("VALIDATION", "account=" + username_ + " event=INSTRUCTION_REJECTED validation=" + validation
			+ " instrument=" + instrumentId + " direction=" + directionName(dir) + " offset=" + offsetName(off)
			+ " price=" + numberText(price) + " volume=" + std::to_string(vol) + " reason=" + reason + " apiCalled=false");
	}
	bool YdSession::rejectOrderWhilePaused(const std::string& instrumentId, int dir, int off, double price, int vol) {
		if (tradingGate_.canTrade())return false;
		const std::uint64_t blockedCount = ++blockedOrderInstructions_;
		log_.error("RISK", "account=" + username_ + " event=ORDER_BLOCKED source=STRATEGY instrument=" + instrumentId
			+ " direction=" + directionName(dir) + " offset=" + offsetName(off) + " price=" + numberText(price)
			+ " volume=" + std::to_string(vol) + " reason=TRADING_PAUSED pauseSource=" + tradingPauseSource_
			+ " blockedOrderInstructions=" + std::to_string(blockedCount) + " apiCalled=false");
		return true;
	}
	int YdSession::sendLimitOrder(const std::string& instrumentId, int dir, int off, double price, int vol, int hedge) {
		std::lock_guard<std::mutex> controlLock(tradingControlMu_);
		if (rejectOrderWhilePaused(instrumentId, dir, off, price, vol))return -1;
		const YDInstrument* instrument = api_ ? api_->getInstrumentByID(instrumentId.c_str()) : nullptr;
		return sendLimitOrderUnlocked(instrument, instrumentId, dir, off, price, vol, hedge);
	}
	int YdSession::sendLimitOrder(const YDInstrument* i, int dir, int off, double price, int vol, int hedge) {
		const std::string instrumentId = i ? std::string(i->InstrumentID) : std::string("<null>");
		std::lock_guard<std::mutex> controlLock(tradingControlMu_);
		if (rejectOrderWhilePaused(instrumentId, dir, off, price, vol))return -1;
		return sendLimitOrderUnlocked(i, instrumentId, dir, off, price, vol, hedge);
	}
	int YdSession::sendLimitOrderUnlocked(const YDInstrument* i, const std::string& instrumentId, int dir, int off, double price, int vol, int hedge) {
		OrderValidator validator;
		const ValidationResult instrumentResult = i ? validator.instrument(api_, instrumentId) : ValidationResult{ false,"instrument not found" };
		if (!instrumentResult.ok) { logInstructionRejected("INSTRUMENT", instrumentId, dir, off, price, vol, instrumentResult.reason); return -1; }
		const ValidationResult priceResult = validator.limitPrice(i, price);
		if (!priceResult.ok) { logInstructionRejected("LIMIT_PRICE", instrumentId, dir, off, price, vol, priceResult.reason); return -1; }
		const ValidationResult volumeResult = validator.limitVolume(i, vol);
		if (!volumeResult.ok) { logInstructionRejected("LIMIT_VOLUME", instrumentId, dir, off, price, vol, volumeResult.reason); return -1; }
		if (!liveOrderSubmissionEnabled_) { log_.error("RISK", "account=" + username_ + " event=LIVE_ORDER_BLOCKED instrument=" + instrumentId + " reason=YdSession live order submission is disabled apiCalled=false"); return -1; }
		const YDAccount* account = api_->getMyAccount();
		if (!account) { log_.error("TRADE", "account unavailable; insertOrder blocked account=" + username_); return -1; }
		YDInputOrder o; std::memset(&o, 0, sizeof(o)); std::string blockedReason;
		o.Direction = static_cast<char>(dir); o.OffsetFlag = static_cast<char>(off); o.HedgeFlag = static_cast<char>(hedge); o.ConnectionSelectionType = YD_CS_Any; o.Price = price; o.OrderVolume = vol; o.OrderType = YD_ODT_Limit; o.YDOrderFlag = YD_YOF_Normal;
		{ std::lock_guard<std::mutex> lk(mu_);
			if (!connected_ || loginError_ != 0 || !caughtUp_ || destroying_)blockedReason = "trade session is not connected/logged-in/caught-up";
			else {
				while (nextOrderRef_ > 0 && nextOrderRef_ < INT_MAX && ownedOrders_.count(nextOrderRef_) != 0)++nextOrderRef_;
				if (nextOrderRef_ <= 0 || nextOrderRef_ >= INT_MAX)blockedReason = "OrderRef space exhausted";
				else {
					o.OrderRef = nextOrderRef_++;
					OwnedOrderState state; state.accountRef = account->AccountRef; state.instrumentRef = i->InstrumentRef;
					state.direction = o.Direction; state.offset = o.OffsetFlag; state.hedge = o.HedgeFlag; state.orderType = o.OrderType; state.orderFlag = o.YDOrderFlag;
					state.price = o.Price; state.tick = i->Tick; state.volume = o.OrderVolume; ownedOrders_.emplace(o.OrderRef, state);
				}
			}
		}
		if (!blockedReason.empty()) { log_.error("TRADE", blockedReason + "; insertOrder blocked account=" + username_); return -1; }
		instructionMonitor_.recordOrder({ i->InstrumentID,dir,off,price,vol,false });
		std::ostringstream s; s << "insertOrder account=" << username_ << " ref=" << o.OrderRef << " instrument=" << i->InstrumentID << " direction=" << directionName(dir) << " offset=" << offsetName(off) << " orderPrice=" << numberText(price) << " volume=" << vol; log_.info("TRADE", s.str());
		const std::string requestTime = timestampText();
		std::uint64_t requestCount = 0; { std::lock_guard<std::mutex> lk(mu_); auto found = ownedOrders_.find(o.OrderRef); if (found == ownedOrders_.end())return -1; found->second.apiCallStarted = true; requestCount = ++orderApiRequests_; }
		const bool submitted = api_->insertOrder(&o, i);
		bool newlyAccepted = false; std::uint64_t submittedCount = 0, acceptedCount = 0;
		{ std::lock_guard<std::mutex> lk(mu_); auto found = ownedOrders_.find(o.OrderRef);
			if (!submitted) { if (found != ownedOrders_.end())ownedOrders_.erase(found); acceptedOrderRefs_.erase(o.OrderRef); }
			else if (found != ownedOrders_.end()) { found->second.apiSubmitted = true; submittedCount = ++orderRequestsSubmitted_; if (found->second.acceptedObserved)newlyAccepted = acceptedOrderRefs_.insert(o.OrderRef).second; acceptedCount = acceptedOrderRefs_.size(); }
			cv_.notify_all();
		}
		log_.info("MONITOR", "account=" + username_ + " activity=ORDER_API_REQUEST activityTime=" + requestTime + " instrument=" + i->InstrumentID + " orderRef=" + std::to_string(o.OrderRef) + " apiReturned=" + (submitted ? std::string("true") : std::string("false")) + " orderApiRequests=" + std::to_string(requestCount) + " orderRequestsSubmitted=" + std::to_string(submittedCount));
		if (!submitted) { log_.error("TRADE", "insertOrder returned false ref=" + std::to_string(o.OrderRef)); return -1; }
		if (newlyAccepted)log_.info("MONITOR", "account=" + username_ + " activity=ORDER_ACCEPTED activityTime=" + timestampText() + " instrument=" + i->InstrumentID + " orderRef=" + std::to_string(o.OrderRef) + " uniqueAcceptedOrders=" + std::to_string(acceptedCount));
		return o.OrderRef;
	}
	bool YdSession::cancelOrder(const YDInstrument* i, const YDOrder& o) {
		if (!liveOrderSubmissionEnabled_) { log_.error("RISK", "account=" + username_ + " event=LIVE_CANCEL_BLOCKED reason=YdSession live order submission is disabled apiCalled=false"); return false; }
		if (!api_ || !i)return false;
		YDCancelOrder c; std::memset(&c, 0, sizeof(c)); std::string blockedReason; std::uint64_t requestCount = 0, attemptId = 0; const std::string requestTime = timestampText();
		{ std::lock_guard<std::mutex>lk(mu_);
			if (!connected_ || loginError_ != 0 || !caughtUp_ || destroying_)blockedReason = "trade session is not ready";
			else {
				auto found = ownedOrders_.find(o.OrderRef);
				if (found == ownedOrders_.end() || !found->second.apiSubmitted || !found->second.hasOrder)blockedReason = "order is not owned by this session";
				else {
					const OwnedOrderState& state = found->second;
					const long long suppliedSys = static_cast<long long>(o.OrderSysID), suppliedLongSys = static_cast<long long>(o.LongOrderSysID);
					if (state.instrumentRef != i->InstrumentRef || state.latest.OrderStatus != YD_OS_Queuing || state.latest.ErrorNo != 0)blockedReason = "owned order is no longer cancelable";
					else if (!state.systemIdentity.matches(suppliedSys, suppliedLongSys))blockedReason = "order identity/system ID mismatch";
					else {
						if (isAssignedYdId(state.systemIdentity.longOrderSysId()))c.LongOrderSysID = static_cast<YDLongOrderSysID>(state.systemIdentity.longOrderSysId());
						else c.OrderSysID = static_cast<YDSysOrderID>(state.systemIdentity.orderSysId());
						CancelAttemptState attempt; attempt.attemptId = attemptId = nextCancelAttemptId_++; attempt.afterOrderCallbackGeneration = orderCallbackGeneration_;
						cancelAttempts_[o.OrderRef] = attempt; requestCount = ++cancelApiRequests_;
					}
				}
			}
		}
		if (!blockedReason.empty()) { log_.error("TRADE", blockedReason + "; cancelOrder blocked account=" + username_ + " ref=" + std::to_string(o.OrderRef)); return false; }
		instructionMonitor_.recordCancel({ i->InstrumentID,o.Direction,o.OffsetFlag,o.Price,o.OrderVolume,true });
		const bool submitted = api_->cancelOrder(&c, i->m_pExchange, nullptr);
		bool newlyConfirmed = false; std::uint64_t submittedCount = 0, confirmedCount = 0;
		{ std::lock_guard<std::mutex>lk(mu_); auto attempt = cancelAttempts_.find(o.OrderRef);
			if (attempt != cancelAttempts_.end() && attempt->second.attemptId == attemptId) {
				attempt->second.apiReturned = true; attempt->second.submitted = submitted;
				if (submitted) {
					submittedCount = ++cancelRequestsSubmitted_;
					if (!attempt->second.failed && attempt->second.canceledCallbackGeneration > attempt->second.afterOrderCallbackGeneration) { attempt->second.confirmed = true; newlyConfirmed = true; confirmedCount = ++confirmedCancellations_; }
				}
			}
			cv_.notify_all();
		}
		std::ostringstream s; s << "cancelOrder account=" << username_ << " ref=" << o.OrderRef << " instrument=" << i->InstrumentID << " direction=" << directionName(o.Direction) << " offset=" << offsetName(o.OffsetFlag) << " orderPrice=" << numberText(o.Price) << " volume=" << o.OrderVolume << " sys=" << o.OrderSysID << " longSys=" << o.LongOrderSysID << " apiReturned=" << (submitted ? "true" : "false"); log_.info("TRADE", s.str());
		log_.info("MONITOR", "account=" + username_ + " activity=CANCEL_API_REQUEST activityTime=" + requestTime + " instrument=" + i->InstrumentID + " orderRef=" + std::to_string(o.OrderRef) + " apiReturned=" + (submitted ? std::string("true") : std::string("false")) + " cancelApiRequests=" + std::to_string(requestCount) + " cancelRequestsSubmitted=" + std::to_string(submittedCount));
		if (!submitted)log_.error("TRADE", "cancelOrder returned false ref=" + std::to_string(o.OrderRef));
		if (newlyConfirmed)log_.info("MONITOR", "account=" + username_ + " activity=CANCELLATION_CONFIRMED activityTime=" + timestampText() + " instrument=" + i->InstrumentID + " orderRef=" + std::to_string(o.OrderRef) + " confirmedCancellations=" + std::to_string(confirmedCount));
		return submitted;
	}
	bool YdSession::cancelMulti(const std::vector<std::pair<const YDInstrument*, YDOrder>>& os) {
		if (!liveOrderSubmissionEnabled_) { log_.error("RISK", "account=" + username_ + " event=LIVE_BATCH_CANCEL_BLOCKED reason=YdSession live order submission is disabled apiCalled=false"); return false; }
		if (!api_ || os.empty() || os.size() > 16) { log_.error("TRADE", "account=" + username_ + " event=BATCH_CANCEL_BLOCKED reason=invalid target count apiCalled=false"); return false; }

		std::vector<YDCancelOrder> cancels(os.size());
		std::vector<const YDExchange*> exchanges(os.size());
		std::vector<OrderIntent> intents;
		std::vector<std::uint64_t> attemptIds(os.size());
		intents.reserve(os.size());
		std::string blockedReason;
		std::ostringstream targetRefs;
		std::uint64_t cancelRequestCount = 0, batchApiCallCount = 0, batchTargetCount = 0;
		const std::string requestTime = timestampText();
		{
			std::lock_guard<std::mutex> lock(mu_);
			if (!connected_ || loginError_ != 0 || !caughtUp_ || destroying_)blockedReason = "trade session is not ready";
			std::set<int> uniqueRefs;
			for (std::size_t n = 0; blockedReason.empty() && n < os.size(); ++n) {
				const YDInstrument* instrument = os[n].first;
				const YDOrder& supplied = os[n].second;
				if (!instrument || !instrument->m_pExchange) { blockedReason = "target instrument/exchange is unavailable"; break; }
				if (!uniqueRefs.insert(supplied.OrderRef).second) { blockedReason = "duplicate order reference in batch"; break; }
				auto found = ownedOrders_.find(supplied.OrderRef);
				if (found == ownedOrders_.end() || !found->second.apiSubmitted || !found->second.hasOrder) { blockedReason = "target order is not owned by this session"; break; }
				const OwnedOrderState& state = found->second;
				const long long suppliedSys = static_cast<long long>(supplied.OrderSysID), suppliedLongSys = static_cast<long long>(supplied.LongOrderSysID);
				if (state.instrumentRef != instrument->InstrumentRef || state.latest.OrderStatus != YD_OS_Queuing || state.latest.ErrorNo != 0) { blockedReason = "owned order is no longer cancelable"; break; }
				if (!state.systemIdentity.matches(suppliedSys, suppliedLongSys)) { blockedReason = "order identity/system ID mismatch"; break; }
				std::memset(&cancels[n], 0, sizeof(YDCancelOrder));
				if (isAssignedYdId(state.systemIdentity.longOrderSysId()))cancels[n].LongOrderSysID = static_cast<YDLongOrderSysID>(state.systemIdentity.longOrderSysId());
				else cancels[n].OrderSysID = static_cast<YDSysOrderID>(state.systemIdentity.orderSysId());
				exchanges[n] = instrument->m_pExchange;
				intents.push_back({ instrument->InstrumentID,state.direction,state.offset,state.price,state.volume,true });
				if (n)targetRefs << ',';
				targetRefs << supplied.OrderRef;
			}
			if (blockedReason.empty()) {
				for (std::size_t n = 0; n < os.size(); ++n) {
					CancelAttemptState attempt;
					attempt.attemptId = attemptIds[n] = nextCancelAttemptId_++;
					attempt.afterOrderCallbackGeneration = orderCallbackGeneration_;
					cancelAttempts_[os[n].second.OrderRef] = attempt;
				}
				cancelApiRequests_ += os.size();
				++batchCancelApiCalls_;
				batchCancelOrdersRequested_ += os.size();
				cancelRequestCount = cancelApiRequests_;
				batchApiCallCount = batchCancelApiCalls_;
				batchTargetCount = batchCancelOrdersRequested_;
			}
		}
		if (!blockedReason.empty()) {
			log_.error("TRADE", "account=" + username_ + " event=BATCH_CANCEL_BLOCKED targetCount=" + std::to_string(os.size()) + " reason=" + blockedReason + " apiCalled=false");
			return false;
		}

		for (const OrderIntent& intent : intents)instructionMonitor_.recordCancel(intent);
		const bool submitted = api_->cancelMultiOrders(static_cast<unsigned>(os.size()), cancels.data(), exchanges.data(), nullptr);
		std::vector<std::pair<int,std::uint64_t>> newlyConfirmedRefs;
		std::uint64_t cancelSubmittedCount = 0, batchSubmittedCount = 0, batchTargetsSubmitted = 0;
		{
			std::lock_guard<std::mutex> lock(mu_);
			for (std::size_t n = 0; n < os.size(); ++n) {
				auto attempt = cancelAttempts_.find(os[n].second.OrderRef);
				if (attempt == cancelAttempts_.end() || attempt->second.attemptId != attemptIds[n])continue;
				attempt->second.apiReturned = true;
				attempt->second.submitted = submitted;
				if (submitted) {
					++cancelRequestsSubmitted_;
					if (!attempt->second.failed && attempt->second.canceledCallbackGeneration > attempt->second.afterOrderCallbackGeneration && !attempt->second.confirmed) {
						attempt->second.confirmed = true;
						++confirmedCancellations_;
						newlyConfirmedRefs.push_back({ os[n].second.OrderRef,confirmedCancellations_ });
					}
				}
			}
			if (submitted) {
				++batchCancelApiCallsSubmitted_;
				batchCancelOrdersSubmitted_ += os.size();
			}
			cancelSubmittedCount = cancelRequestsSubmitted_;
			batchSubmittedCount = batchCancelApiCallsSubmitted_;
			batchTargetsSubmitted = batchCancelOrdersSubmitted_;
			cv_.notify_all();
		}

		log_.info("TRADE", "account=" + username_ + " event=BATCH_CANCEL_REQUEST targetCount=" + std::to_string(os.size()) + " orderRefs=" + targetRefs.str() + " apiReturned=" + (submitted ? std::string("true") : std::string("false")));
		log_.info("MONITOR", "account=" + username_ + " activity=BATCH_CANCEL_API_REQUEST activityTime=" + requestTime
			+ " apiReturned=" + (submitted ? std::string("true") : std::string("false"))
			+ " batchApiCalls=" + std::to_string(batchApiCallCount) + " batchApiCallsSubmitted=" + std::to_string(batchSubmittedCount)
			+ " batchTargetOrders=" + std::to_string(batchTargetCount) + " batchTargetOrdersSubmitted=" + std::to_string(batchTargetsSubmitted)
			+ " cancelApiRequests=" + std::to_string(cancelRequestCount) + " cancelRequestsSubmitted=" + std::to_string(cancelSubmittedCount));
		for (const auto& confirmation : newlyConfirmedRefs)log_.info("MONITOR", "account=" + username_ + " activity=CANCELLATION_CONFIRMED activityTime=" + timestampText() + " orderRef=" + std::to_string(confirmation.first) + " confirmedCancellations=" + std::to_string(confirmation.second));
		if (!submitted)log_.error("TRADE", "account=" + username_ + " event=BATCH_CANCEL_REQUEST_REJECTED targetCount=" + std::to_string(os.size()));
		return submitted;
	}
	bool YdSession::waitOrder(int ref, int s, const std::function<bool(const YDOrder&)>& pred, YDOrder& out) { std::unique_lock<std::mutex>lk(mu_); if (!cv_.wait_for(lk, std::chrono::seconds(s), [&] {auto it = ownedOrders_.find(ref); return it != ownedOrders_.end() && it->second.hasOrder && pred(it->second.latest); }))return false; out = ownedOrders_[ref].latest; return true; }
	int YdSession::waitCancelTerminal(int ref, int s, YDOrder& out) {
		std::unique_lock<std::mutex>lk(mu_); auto initial = cancelAttempts_.find(ref); if (initial == cancelAttempts_.end() || !initial->second.apiReturned || !initial->second.submitted)return -2;
		const std::uint64_t attemptId = initial->second.attemptId;
		const auto ready = [&] {
			auto attempt = cancelAttempts_.find(ref); if (attempt == cancelAttempts_.end() || attempt->second.attemptId != attemptId)return true;
			if (attempt->second.failed || attempt->second.confirmed)return true;
			auto order = ownedOrders_.find(ref); if (order == ownedOrders_.end() || !order->second.hasOrder || order->second.lastOrderCallbackGeneration <= attempt->second.afterOrderCallbackGeneration)return false;
			return order->second.latest.OrderStatus == YD_OS_AllTraded || order->second.latest.OrderStatus == YD_OS_Rejected;
		};
		if (!cv_.wait_for(lk, std::chrono::seconds(s), ready))return -1;
		auto attempt = cancelAttempts_.find(ref); if (attempt == cancelAttempts_.end() || attempt->second.attemptId != attemptId)return -2;
		if (attempt->second.failed)return std::max(1, attempt->second.errorNo);
		auto order = ownedOrders_.find(ref); if (order == ownedOrders_.end() || !order->second.hasOrder)return -2;
		if (attempt->second.confirmed || (order->second.lastOrderCallbackGeneration > attempt->second.afterOrderCallbackGeneration && (order->second.latest.OrderStatus == YD_OS_AllTraded || order->second.latest.OrderStatus == YD_OS_Rejected))) { out = order->second.latest; return 0; }
		return -2;
	}
	bool YdSession::waitTrade(int ref, int s, YDTrade& out) { std::unique_lock<std::mutex>lk(mu_); if (!cv_.wait_for(lk, std::chrono::seconds(s), [&] {for (const auto& t : ownedTrades_)if (t.OrderRef == ref)return true; return false; }))return false; for (auto it = ownedTrades_.rbegin(); it != ownedTrades_.rend(); ++it)if (it->OrderRef == ref) { out = *it; return true; }return false; }
	bool YdSession::disconnectNow(std::uint64_t& beforeGeneration) {
		YDApi* api = nullptr; const std::string requestTime = timestampText();
		{ std::lock_guard<std::mutex>lk(mu_); if (!api_ || destroying_ || !connected_ || loginError_ != 0 || !caughtUp_)return false; api = api_; }
		log_.info("CONNECTION", "account=" + username_ + " channel=TRADE state=DISCONNECT_REQUESTED requestTime=" + requestTime);
		bool changed = false; { std::lock_guard<std::mutex>lk(mu_); changed = api_ != api || destroying_ || !connected_ || loginError_ != 0 || !caughtUp_; if (!changed)beforeGeneration = eventGeneration_; }
		if (changed) { log_.error("CONNECTION", "account=" + username_ + " channel=TRADE state=DISCONNECT_ABORTED reason=session_changed_before_api_call"); return false; }
		api->disconnect();
		log_.info("CONNECTION", "account=" + username_ + " channel=TRADE state=DISCONNECT_API_CALLED requestTime=" + requestTime + " afterGeneration=" + std::to_string(beforeGeneration));
		return true;
	}
	void YdSession::notifyAfterApiDestroy() { std::lock_guard<std::mutex>lk(mu_); destroyed_ = true; cv_.notify_all(); log_.info("SYSTEM", "YD API destroyed"); }
	void YdSession::notifyEvent(int e) {
		const std::string eventTime = timestampText(); std::uint64_t generation = 0; std::string channel; std::string state;
		{ std::lock_guard<std::mutex>lk(mu_); generation = ++eventGeneration_;
			if (e == YD_AE_TCPTradeConnected) {
				connected_ = true; tradeConnectedGeneration_ = generation; tradeConnectedTime_ = eventTime; channel = "TRADE"; state = tradeDisconnectedGeneration_ == 0 ? "NORMAL" : "RECONNECTED";
			} else if (e == YD_AE_TCPTradeDisconnected) {
				connected_ = false; disconnectedSeen_ = true; loginDone_ = false; loginError_ = -999; caughtUp_ = false; tradeDisconnectedGeneration_ = generation; tradeDisconnectedTime_ = eventTime; historicalOrderCallbacks_ = 0; historicalTradeCallbacks_ = 0; historicalCancelCallbacks_ = 0; historicalRejectedCallbacks_ = 0; historicalByInstrument_.clear(); channel = "TRADE"; state = "ABNORMAL";
			} else if (e == YD_AE_TCPMarketDataConnected) { channel = "MARKET_DATA"; state = "CONNECTED";
			} else if (e == YD_AE_TCPMarketDataDisconnected) { channel = "MARKET_DATA"; state = "DISCONNECTED"; }
			cv_.notify_all();
		}
		log_.info("API_EVENT", "account=" + username_ + " event=" + std::to_string(e) + " eventName=" + apiEventName(e) + " eventTime=" + eventTime + " generation=" + std::to_string(generation));
		if (!channel.empty())log_.info("CONNECTION", "account=" + username_ + " channel=" + channel + " state=" + state + " eventName=" + apiEventName(e) + " eventTime=" + eventTime + " generation=" + std::to_string(generation));
	}
	void YdSession::notifyReadyForLogin(bool failed) {
		YDApi* api = nullptr; std::uint64_t loginGenerationBefore = 0;
		{
			std::lock_guard<std::mutex> apiLifecycleLock(apiLifecycleMu_);
			{ std::lock_guard<std::mutex>lk(mu_); if (!destroying_)api = api_; loginGenerationBefore = loginGeneration_; }
			if (api)++activeLoginApiCalls_;
		}
		if (!api)return;
		bool apiCallActive = true;
		auto finishApiCall = [&] {
			if (!apiCallActive)return;
			std::lock_guard<std::mutex> apiLifecycleLock(apiLifecycleMu_);
			--activeLoginApiCalls_; apiCallActive = false; apiLifecycleCv_.notify_all();
		};
		std::string configuredAppId, configuredAuthCode;
		if (const char* appId = api->getConfig("AppID"))configuredAppId = trim(appId);
		if (const char* authCode = api->getConfig("AuthCode"))configuredAuthCode = trim(authCode);
		{ std::lock_guard<std::mutex>lk(mu_); appId_ = configuredAppId; }
		const std::string appIdForLog = configuredAppId.empty() ? std::string("UNCONFIGURED") : configuredAppId;
		log_.info("LOGIN", "account=" + username_ + " appId=" + appIdForLog + " notifyReadyForLogin hasLoginFailed=" + (failed ? std::string("true") : std::string("false")) + " eventTime=" + timestampText());
		if (configuredAppId.empty() || configuredAuthCode.empty()) {
			const std::string loginTime = timestampText(); std::uint64_t generation = 0; bool stateChanged = false;
			{ std::lock_guard<std::mutex>lk(mu_); if (loginGeneration_ == loginGenerationBefore) { loginError_ = YD_ERROR_InvalidLoginInfo; loginDone_ = true; loginTime_ = loginTime; generation = loginGeneration_ = ++eventGeneration_; stateChanged = true; cv_.notify_all(); } }
			if (stateChanged)log_.error("LOGIN", "account=" + username_ + " appId=" + appIdForLog + " event=LOGIN_CONFIGURATION_REJECTED appIdConfigured=" + (!configuredAppId.empty() ? std::string("true") : std::string("false")) + " authCodeConfigured=" + (!configuredAuthCode.empty() ? std::string("true") : std::string("false")) + " errorNo=" + std::to_string(YD_ERROR_InvalidLoginInfo) + " generation=" + std::to_string(generation) + " apiCalled=false");
			finishApiCall();
			return;
		}
		log_.info("LOGIN", "account=" + username_ + " appId=" + configuredAppId + " event=LOGIN_REQUESTED authCodeConfigured=true eventTime=" + timestampText());
		if (!api->login(username_.c_str(), password_.c_str(), configuredAppId.c_str(), configuredAuthCode.c_str())) {
			const std::string loginTime = timestampText(); std::uint64_t generation = 0; bool stateChanged = false;
			{ std::lock_guard<std::mutex>lk(mu_); if (loginGeneration_ == loginGenerationBefore) { loginError_ = YD_ERROR_CannotSend; loginDone_ = true; loginTime_ = loginTime; generation = loginGeneration_ = ++eventGeneration_; stateChanged = true; cv_.notify_all(); } }
			if (stateChanged)log_.error("LOGIN", "account=" + username_ + " appId=" + configuredAppId + " event=LOGIN_REQUEST_FAILED errorNo=" + std::to_string(YD_ERROR_CannotSend) + " generation=" + std::to_string(generation) + " apiCalled=true");
		}
		finishApiCall();
	}
	void YdSession::notifyLogin(int err, int maxRef, bool) {
		const std::string loginTime = timestampText(); std::uint64_t generation = 0; std::string appId;
		{ std::lock_guard<std::mutex>lk(mu_); loginError_ = err; loginDone_ = true; loginTime_ = loginTime; maxOrderRef_ = maxRef; appId = appId_; generation = loginGeneration_ = ++eventGeneration_; if (err == 0) { const int serverNext = maxRef >= INT_MAX - 1 ? INT_MAX : std::max(1, maxRef + 1); nextOrderRef_ = std::max(nextOrderRef_, serverNext); } cv_.notify_all(); }
		const std::string appIdForLog = appId.empty() ? std::string("UNCONFIGURED") : appId;
		if (err == 0)log_.info("LOGIN", "login OK account=" + username_ + " appId=" + appIdForLog + " loginTime=" + loginTime + " maxOrderRef=" + std::to_string(maxRef) + " generation=" + std::to_string(generation)); else log_.error("LOGIN", "login failed account=" + username_ + " appId=" + appIdForLog + " loginTime=" + loginTime + " errorNo=" + std::to_string(err) + " generation=" + std::to_string(generation));
	}
	void YdSession::notifyFinishInit() { std::lock_guard<std::mutex>lk(mu_); initDone_ = true; cv_.notify_all(); log_.info("SYSTEM", "notifyFinishInit: static data ready"); }
	void YdSession::notifyCaughtUp() { const std::string caughtUpTime = timestampText(); std::size_t orderCount = 0, tradeCount = 0; std::uint64_t generation = 0; { std::lock_guard<std::mutex>lk(mu_); caughtUp_ = true; caughtUpTime_ = caughtUpTime; generation = caughtUpGeneration_ = ++eventGeneration_; orderCount = historicalOrderCallbacks_; tradeCount = historicalTradeCallbacks_; cv_.notify_all(); } log_.info("SYSTEM", "notifyCaughtUp: history caught up account=" + username_ + " caughtUpTime=" + caughtUpTime + " generation=" + std::to_string(generation) + " historicalOrderCallbacks=" + std::to_string(orderCount) + " historicalTradeCallbacks=" + std::to_string(tradeCount)); }
	void YdSession::notifyOrder(const YDOrder* o, const YDInstrument* i, const YDAccount* a) {
		if (!o)return; bool shouldLog = false, validationFailed = false, newlyAccepted = false, newlyConfirmed = false; std::string validationReason; std::uint64_t acceptedCount = 0, confirmedCount = 0;
		{ std::lock_guard<std::mutex>lk(mu_); const std::uint64_t callbackGeneration = ++orderCallbackGeneration_; if (!caughtUp_) { ++historicalOrderCallbacks_; if (o->ErrorNo != 0)++historicalRejectedCallbacks_; else if (o->OrderStatus == YD_OS_Canceled)++historicalCancelCallbacks_; if (i && i->InstrumentID[0]) { HistoricalCallbackCounts& per = historicalByInstrument_[i->InstrumentID]; ++per.order; if (o->ErrorNo != 0)++per.rejected; else if (o->OrderStatus == YD_OS_Canceled)++per.cancel; } }
			auto found = ownedOrders_.find(o->OrderRef);
			if (found != ownedOrders_.end()) {
				OwnedOrderState& state = found->second; shouldLog = true; ++orderActivityGeneration_;
				const long long incomingSys = static_cast<long long>(o->OrderSysID), incomingLongSys = static_cast<long long>(o->LongOrderSysID);
				const bool acceptedStatus = o->OrderStatus == YD_OS_Accepted || o->OrderStatus == YD_OS_Queuing || o->OrderStatus == YD_OS_AllTraded || o->OrderStatus == YD_OS_Canceled;
				const SystemOrderIdEvidence systemIdEvidence = o->ErrorNo != 0 ? SystemOrderIdEvidence::Ignore
					: (acceptedStatus ? SystemOrderIdEvidence::BindOrMatch : SystemOrderIdEvidence::ValidateOnly);
				if (!state.apiCallStarted)validationReason = "callback preceded the API request";
				else if (!a || a->AccountRef != state.accountRef)validationReason = "account mismatch";
				else if (!i || i->InstrumentRef != state.instrumentRef)validationReason = "instrument mismatch";
				else if (o->Direction != state.direction || o->OffsetFlag != state.offset || o->HedgeFlag != state.hedge)validationReason = "direction/offset/hedge mismatch";
				else if (o->OrderType != state.orderType || o->YDOrderFlag != state.orderFlag || o->OrderGroupID != 0)validationReason = "order type/flag/group mismatch";
				else if (o->OrderVolume != state.volume || !samePrice(state.price, o->Price, state.tick))validationReason = "price/volume mismatch";
				else if (static_cast<int>(o->RealConnectionID) < 0)validationReason = "callback came from another system connection";
				else if (!observeSystemOrderId(state.systemIdentity, systemIdEvidence, incomingSys, incomingLongSys))validationReason = "system order ID mismatch";
				if (!validationReason.empty()) { validationFailed = true; shouldLog = false; ++callbackValidationFailures_; }
				else {
					state.hasOrder = true; state.latest = *o; state.lastOrderCallbackGeneration = callbackGeneration;
					if (o->ErrorNo == 0 && acceptedStatus && state.systemIdentity.bound()) { state.acceptedObserved = true; if (state.apiSubmitted)newlyAccepted = acceptedOrderRefs_.insert(o->OrderRef).second; }
					if (o->ErrorNo == 0 && o->OrderStatus == YD_OS_Canceled) {
						auto attempt = cancelAttempts_.find(o->OrderRef);
						if (attempt != cancelAttempts_.end() && callbackGeneration > attempt->second.afterOrderCallbackGeneration) {
							attempt->second.canceledCallbackGeneration = callbackGeneration;
							if (attempt->second.apiReturned && attempt->second.submitted && !attempt->second.failed && !attempt->second.confirmed) { attempt->second.confirmed = true; newlyConfirmed = true; ++confirmedCancellations_; }
						}
					}
				}
			}
			acceptedCount = acceptedOrderRefs_.size(); confirmedCount = confirmedCancellations_; cv_.notify_all();
		}
		const std::string account = accountName(a, username_); const std::string instrument = i ? std::string(i->InstrumentID) : std::string();
		if (validationFailed)log_.warn("VALIDATION", "ignored order callback account=" + account + " ref=" + std::to_string(o->OrderRef) + " instrument=" + instrument + " reason=" + validationReason + " sys=" + std::to_string(o->OrderSysID) + " longSys=" + std::to_string(o->LongOrderSysID));
		if (shouldLog) {
			std::ostringstream s;
			if (o->ErrorNo)s << "account=" << account << " event=ORDER_REJECTED source=notifyOrder";
			else s << "notifyOrder account=" << account;
			s << " ref=" << o->OrderRef << " instrument=" << instrument << " direction=" << directionName(o->Direction) << " offset=" << offsetName(o->OffsetFlag) << " orderPrice=" << numberText(o->Price) << " volume=" << o->OrderVolume << " status=" << orderStatusName(o->OrderStatus) << '(' << o->OrderStatus << ") errorNo=" << o->ErrorNo;
			if (o->ErrorNo)s << " errorName=" << ydErrorName(o->ErrorNo);
			s << " sys=" << o->OrderSysID << " longSys=" << o->LongOrderSysID << " traded=" << o->TradeVolume << '/' << o->OrderVolume;
			if (o->ErrorNo)log_.error("ERROR", s.str()); else log_.info("ORDER", s.str());
		}
		if (newlyAccepted)log_.info("MONITOR", "account=" + account + " activity=ORDER_ACCEPTED activityTime=" + timestampText() + " instrument=" + instrument + " orderRef=" + std::to_string(o->OrderRef) + " uniqueAcceptedOrders=" + std::to_string(acceptedCount));
		if (newlyConfirmed)log_.info("MONITOR", "account=" + account + " activity=CANCELLATION_CONFIRMED activityTime=" + timestampText() + " instrument=" + instrument + " orderRef=" + std::to_string(o->OrderRef) + " confirmedCancellations=" + std::to_string(confirmedCount));
	}
	void YdSession::notifyTrade(const YDTrade* t, const YDInstrument* i, const YDAccount* a) {
		if (!t)return; bool shouldLog = false, validationFailed = false; std::string validationReason;
		{ std::lock_guard<std::mutex>lk(mu_); trades_.push_back(*t); if (!caughtUp_) { ++historicalTradeCallbacks_; if (i && i->InstrumentID[0]) { ++historicalByInstrument_[i->InstrumentID].trade; } }
			auto found = ownedOrders_.find(t->OrderRef);
			if (found != ownedOrders_.end()) {
				const OwnedOrderState& state = found->second; ++orderActivityGeneration_; const long long incomingSys = static_cast<long long>(t->OrderSysID), incomingLongSys = static_cast<long long>(t->LongOrderSysID);
				if (!state.apiSubmitted || !state.systemIdentity.bound())validationReason = "owned order is not submitted/bound";
				else if (t->AccountRef != state.accountRef || (a && a->AccountRef != state.accountRef))validationReason = "account mismatch";
				else if (t->InstrumentRef != state.instrumentRef || !i || i->InstrumentRef != state.instrumentRef)validationReason = "instrument mismatch";
				else if (t->Direction != state.direction || t->OffsetFlag != state.offset || t->HedgeFlag != state.hedge)validationReason = "direction/offset/hedge mismatch";
				else if (t->OrderGroupID != 0 || t->YDTradeFlag != YD_YTF_Normal || static_cast<int>(t->RealConnectionID) < 0)validationReason = "trade flag/group/connection mismatch";
				else if (!isAssignedYdId(t->TradeID) && !isAssignedYdId(t->LongTradeID))validationReason = "trade ID unavailable";
				else if (!state.systemIdentity.matches(incomingSys, incomingLongSys))validationReason = "system order ID mismatch";
				if (validationReason.empty()) { ownedTrades_.push_back(*t); shouldLog = true; }
				else { validationFailed = true; ++callbackValidationFailures_; }
			}
			cv_.notify_all();
		}
		const std::string account = accountName(a, username_); const std::string instrument = i ? std::string(i->InstrumentID) : std::string();
		if (validationFailed)log_.warn("VALIDATION", "ignored trade callback account=" + account + " ref=" + std::to_string(t->OrderRef) + " instrument=" + instrument + " reason=" + validationReason + " orderSysId=" + systemOrderIdText(t->OrderSysID, t->LongOrderSysID));
		if (!shouldLog)return; std::ostringstream s; s << "notifyTrade account=" << account << " ref=" << t->OrderRef << " instrument=" << instrument << " direction=" << directionName(t->Direction) << " offset=" << offsetName(t->OffsetFlag) << " tradePrice=" << numberText(t->Price) << " volume=" << t->Volume << " tradeId=" << (isAssignedYdId(t->LongTradeID) ? t->LongTradeID : static_cast<long long>(t->TradeID)) << " orderSysId=" << systemOrderIdText(t->OrderSysID, t->LongOrderSysID); log_.info("TRADE", s.str());
	}
	void YdSession::notifyFailedCancelOrder(const YDFailedCancelOrder* f, const YDExchange*, const YDAccount* a) {
		if (!f)return; bool ownedAttempt = false, validationFailed = false; std::string validationReason;
		{ std::lock_guard<std::mutex>lk(mu_); auto found = ownedOrders_.find(f->OrderRef); auto attempt = cancelAttempts_.find(f->OrderRef); if (found != ownedOrders_.end())++orderActivityGeneration_;
			if (found != ownedOrders_.end() && attempt != cancelAttempts_.end()) {
				OwnedOrderState& state = found->second; const long long incomingSys = static_cast<long long>(f->OrderSysID), incomingLongSys = static_cast<long long>(f->LongOrderSysID);
				if (f->AccountRef != state.accountRef || (a && a->AccountRef != state.accountRef))validationReason = "account mismatch";
				else if (f->YDOrderFlag != state.orderFlag || f->OrderGroupID != 0)validationReason = "order flag/group mismatch";
				else if (!state.systemIdentity.matches(incomingSys, incomingLongSys))validationReason = "system order ID mismatch";
				if (validationReason.empty()) {
					ownedAttempt = true;
					if (!attempt->second.failed) { attempt->second.failed = true; attempt->second.errorNo = f->ErrorNo; ++failedCancelCallbacks_; if (attempt->second.confirmed) { attempt->second.confirmed = false; if (confirmedCancellations_ > 0)--confirmedCancellations_; } }
				} else { validationFailed = true; ++callbackValidationFailures_; }
			}
			cv_.notify_all();
		}
		const std::string account = accountName(a, username_);
		if (validationFailed)log_.warn("VALIDATION", "ignored failed-cancel callback account=" + account + " ref=" + std::to_string(f->OrderRef) + " reason=" + validationReason + " orderSysId=" + systemOrderIdText(f->OrderSysID, f->LongOrderSysID));
		if (ownedAttempt) { std::ostringstream s; s << "notifyFailedCancelOrder account=" << account << " ref=" << f->OrderRef << " orderSysId=" << systemOrderIdText(f->OrderSysID, f->LongOrderSysID) << " errorNo=" << f->ErrorNo; log_.error("ERROR", s.str()); }
	}
	void YdSession::notifyMarketData(const YDMarketData* m) { if (!m)return; { std::lock_guard<std::mutex>lk(mu_); market_[m->InstrumentRef] = *m; marketVersions_[m->InstrumentRef] = ++marketVersion_; cv_.notify_all(); } }
}
