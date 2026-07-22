/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include <rpl/producer.h>
#include <rpl/variable.h>

#include <set>

namespace Main {
class Session;
} // namespace Main

namespace Enterprise {

enum class BroadcastState {
	Idle,
	Broadcasting,
};

struct BroadcastStatus {
	BroadcastState state = BroadcastState::Idle;
	int current = 0;
	int total = 0;
};

class BroadcastService final {
public:
	explicit BroadcastService(not_null<Main::Session*> session);
	~BroadcastService();

	void setEnabled(bool enabled);
	[[nodiscard]] bool enabled() const;

	void broadcastMessage(FullMsgId msgId);

	[[nodiscard]] rpl::producer<BroadcastStatus> statusValue() const;

private:
	struct Target {
		not_null<PeerData*> peer;
	};

	[[nodiscard]] std::vector<Target> collectTargets() const;
	[[nodiscard]] bool isDuplicate(MsgId msgId, PeerId peerId) const;
	void markProcessed(MsgId msgId, PeerId peerId);

	void scheduleNextCheck();
	void performCheck();
	void startBroadcast(FullMsgId msgId, std::vector<Target> targets);
	void sendToNext(
		FullMsgId msgId,
		std::vector<Target> targets,
		int index);
	void setStatus(BroadcastStatus status);

	const not_null<Main::Session*> _session;
	base::Timer _timer;
	bool _enabled = false;
	MsgId _lastCheckedMsgId = 0;
	std::set<uint64_t> _processedPairs;
	rpl::variable<BroadcastStatus> _status;
	rpl::lifetime _lifetime;

};

} // namespace Enterprise
