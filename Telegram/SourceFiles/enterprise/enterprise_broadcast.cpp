/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "enterprise/enterprise_broadcast.h"

#include "apiwrap.h"
#include "api/api_common.h"
#include "base/call_delayed.h"
#include "base/unixtime.h"
#include "data/data_folder.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/business/data_shortcut_messages.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"

#include <QRandomGenerator>
#include <map>
#include <memory>

namespace Enterprise {
namespace {

constexpr auto kTimerMinMs        = crl::time(20 * 60 * 1000);
constexpr auto kTimerMaxMs        = crl::time(30 * 60 * 1000);
constexpr auto kDelayMinMs        = crl::time(100);
constexpr auto kDelayMaxMs        = crl::time(1000);
constexpr auto kActivityThreshold = TimeId(10 * 3600);

[[nodiscard]] uint64 MakePairKey(MsgId msgId, PeerId peerId) {
	return (uint64(uint32(msgId.bare)) << 32) ^ peerId.value;
}

} // namespace

BroadcastService::BroadcastService(not_null<Main::Session*> session)
: _session(session)
, _timer([=] { performCheck(); }) {
}

BroadcastService::~BroadcastService() = default;

void BroadcastService::setEnabled(bool enabled) {
	if (_enabled == enabled) {
		return;
	}
	_enabled = enabled;
	if (_enabled) {
		scheduleNextCheck();
	} else {
		_timer.cancel();
	}
}

bool BroadcastService::enabled() const {
	return _enabled;
}

rpl::producer<BroadcastStatus> BroadcastService::statusValue() const {
	return _status.value();
}

std::vector<BroadcastService::Target> BroadcastService::collectTargets() const {
	auto result = std::vector<Target>();
	const auto now = base::unixtime::now();
	const auto list = _session->data().chatsList();
	for (const auto &row : list->indexed()->all()) {
		const auto history = row->history();
		if (!history) {
			continue;
		}
		const auto peer = history->peer;

		if (!peer->isChat() && !peer->isMegagroup()) {
			continue;
		}

		if (history->folder() != nullptr) {
			continue;
		}

		const auto lastMsg = history->lastMessage();
		if (!lastMsg) {
			continue;
		}
		if ((now - lastMsg->date()) > kActivityThreshold) {
			continue;
		}

		if (lastMsg->out()) {
			continue;
		}

		result.push_back({ peer });
	}
	return result;
}

bool BroadcastService::isDuplicate(MsgId msgId, PeerId peerId) const {
	return _processedPairs.contains(MakePairKey(msgId, peerId));
}

void BroadcastService::markProcessed(MsgId msgId, PeerId peerId) {
	_processedPairs.emplace(MakePairKey(msgId, peerId));
}

void BroadcastService::scheduleNextCheck() {
	const auto range = int(kTimerMaxMs - kTimerMinMs);
	_timer.callOnce(kTimerMinMs + QRandomGenerator::global()->bounded(range));
}

void BroadcastService::performCheck() {
	const auto savedHistory = _session->data().history(_session->user());
	const auto lastMsg = savedHistory->lastMessage();
	if (!lastMsg || lastMsg->id == _lastCheckedMsgId) {
		scheduleNextCheck();
		return;
	}
	_lastCheckedMsgId = lastMsg->id;

	auto targets = collectTargets();
	if (!targets.empty()) {
		startBroadcast(lastMsg->fullId(), std::move(targets));
	}
	scheduleNextCheck();
}

void BroadcastService::broadcastMessage(FullMsgId msgId) {
	auto targets = collectTargets();
	if (!targets.empty()) {
		startBroadcast(msgId, std::move(targets));
	}
}

void BroadcastService::setStatus(BroadcastStatus status) {
	_status = std::move(status);
}

void BroadcastService::startBroadcast(
		FullMsgId msgId,
		std::vector<Target> targets) {
	setStatus({ BroadcastState::Broadcasting, 0, int(targets.size()) });
	sendToNext(msgId, std::move(targets), 0);
}

void BroadcastService::sendToNext(
		FullMsgId msgId,
		std::vector<Target> targets,
		int index) {
	if (index >= int(targets.size())) {
		setStatus({ BroadcastState::Idle });
		return;
	}

	setStatus({
		BroadcastState::Broadcasting,
		index + 1,
		int(targets.size()),
	});

	const auto peer = targets[index].peer;

	if (isDuplicate(msgId.msg, peer->id)) {
		const auto range = int(kDelayMaxMs - kDelayMinMs);
		const auto delay = kDelayMinMs + QRandomGenerator::global()->bounded(range);
		base::call_delayed(
			delay,
			_session,
			[=, targets = std::move(targets)]() mutable {
				sendToNext(msgId, std::move(targets), index + 1);
			});
		return;
	}

	using Flag = MTPmessages_ForwardMessages::Flag;
	const auto flags = Flag::f_drop_author;
	const auto randomId = QRandomGenerator::global()->generate64();
	const auto rawMsgId = int32(msgId.msg.bare);

	_session->api().request(MTPmessages_ForwardMessages(
		MTP_flags(flags),
		MTP_inputPeerSelf(),
		MTP_vector<MTPint>(1, MTP_int(rawMsgId)),
		MTP_vector<MTPlong>(1, MTP_long(randomId)),
		peer->input(),
		MTPint(),
		MTPInputReplyTo(),
		MTPint(),
		MTPint(),
		MTP_inputPeerEmpty(),
		Data::ShortcutIdToMTP(_session, 0),
		MTPlong(),
		MTPint(),
		MTPlong(),
		Api::SuggestToMTP({})
	)).done([=, targets = std::move(targets)](const MTPUpdates &) mutable {
		markProcessed(msgId.msg, peer->id);

		const auto range = int(kDelayMaxMs - kDelayMinMs);
		const auto delay = kDelayMinMs + QRandomGenerator::global()->bounded(range);
		base::call_delayed(
			delay,
			_session,
			[=, targets = std::move(targets)]() mutable {
				sendToNext(msgId, std::move(targets), index + 1);
			});
	}).fail([=, targets = std::move(targets)](
			const MTP::Error &error) mutable {
		auto pause = kDelayMaxMs;
		if (!MTP::IgnoreError(error)) {
			static const auto kFloodWait = u"FLOOD_WAIT_"_q;
			if (error.type().startsWith(kFloodWait)) {
				const auto secs = error.type().mid(kFloodWait.size()).toInt();
				if (secs > 0) {
					pause = crl::time(secs + 5) * 1000;
				}
			}
		}
		base::call_delayed(
			pause,
			_session,
			[=, targets = std::move(targets)]() mutable {
				sendToNext(msgId, std::move(targets), index + 1);
			});
	}).send();
}

} // namespace Enterprise

namespace Enterprise {
namespace {

std::map<Main::Session*, std::unique_ptr<BroadcastService>> gServices;
std::map<Main::Session*, bool> gEnabledStates;

} // namespace

BroadcastService &GetService(not_null<Main::Session*> session) {
	auto &ptr = gServices[session];
	if (!ptr) {
		ptr = std::make_unique<BroadcastService>(session);
		const auto it = gEnabledStates.find(session);
		if (it != gEnabledStates.end() && it->second) {
			ptr->setEnabled(true);
		}
	}
	return *ptr;
}

bool IsEnabled(not_null<Main::Session*> session) {
	const auto it = gEnabledStates.find(session);
	return (it != gEnabledStates.end()) && it->second;
}

void SetEnabled(not_null<Main::Session*> session, bool value) {
	gEnabledStates[session] = value;
	GetService(session).setEnabled(value);
}

} // namespace Enterprise
