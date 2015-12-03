/*
 * Copyright (c) 2015 Ember
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "RealmService.h"
#include "RealmList.h"
#include <boost/uuid/uuid.hpp>

namespace em = ember::messaging;

namespace ember {

RealmService::RealmService(RealmList& realms, spark::Service& spark, spark::ServiceDiscovery& s_disc, log::Logger* logger)
                           : realms_(realms), spark_(spark), s_disc_(s_disc), logger_(logger) {
	spark_.dispatcher()->register_handler(this, em::Service::RealmStatus, spark::EventDispatcher::Mode::CLIENT);
	listener_ = std::move(s_disc_.listener(em::Service::RealmStatus,
	                      std::bind(&RealmService::service_located, this, std::placeholders::_1)));
	listener_->search();
}

RealmService::~RealmService() {
	spark_.dispatcher()->remove_handler(this);
}

void RealmService::handle_message(const spark::Link& link, const em::MessageRoot* root) {
	switch(root->data_type()) {
		case em::Data::RealmStatus:
			handle_realm_status(link, root);
			break;
	}
}

void RealmService::handle_realm_status(const spark::Link& link, const em::MessageRoot* root) {
	LOG_TRACE(logger_) << __func__ << LOG_ASYNC;

	auto msg = static_cast<const em::realm::RealmStatus*>(root->data());

	if(!msg->name() || !msg->id() || !msg->ip()) {
		LOG_DEBUG(logger_) << "Failed" << LOG_ASYNC; // todo
	}

	Realm realm;
	realm.id = msg->id();
	realm.ip = msg->ip()->str();
	realm.name = msg->name()->str();
	realm.population = msg->population();
	realm.type = static_cast<Realm::Type>(msg->type());
	realm.flags = static_cast<Realm::Flag>(msg->flags());
	realm.timezone = msg->timezone();
	realms_.add_realm(realm);

	known_realms_[link.uuid] = msg->id();
}

void RealmService::handle_link_event(const spark::Link& link, spark::LinkState event) {
	LOG_TRACE(logger_) << __func__ << LOG_ASYNC;

	switch(event) {
		case spark::LinkState::LINK_UP:
			LOG_INFO(logger_) << "Link to realm gateway established" << LOG_ASYNC;
			request_realm_status(link);
			break;
		case spark::LinkState::LINK_DOWN:
			LOG_INFO(logger_) << "Link to realm gateway closed" << LOG_ASYNC;
			mark_realm_offline(link);
			break;
	}
}

void RealmService::service_located(const messaging::multicast::LocateAnswer* message) {
	LOG_DEBUG(logger_) << "Located realm gateway at " << message->ip()->str() << LOG_ASYNC; // todo
	spark_.connect(message->ip()->str(), message->port());
}

void RealmService::mark_realm_offline(const spark::Link& link) {
	auto it = known_realms_.find(link.uuid);

	if(it == known_realms_.end()) {
		return;
	}

	Realm realm = realms_.get_realm(it->second);
	realm.flags = static_cast<Realm::Flag>(realm.flags | Realm::Flag::OFFLINE);
	realms_.add_realm(realm);
}

void RealmService::request_realm_status(const spark::Link& link) {
	LOG_TRACE(logger_) << __func__ << LOG_ASYNC;

	auto fbb = std::make_shared<flatbuffers::FlatBufferBuilder>();
	auto msg = messaging::CreateMessageRoot(*fbb, messaging::Service::RealmStatus, 0, 0,
		em::Data::RequestRealmStatus, em::realm::CreateRequestRealmStatus(*fbb).Union());
	fbb->Finish(msg);

	if(spark_.send(link, fbb) != spark::Service::Result::OK) {
		LOG_DEBUG(logger_) << "Failed" << LOG_ASYNC; // todo
	}
}

} // ember