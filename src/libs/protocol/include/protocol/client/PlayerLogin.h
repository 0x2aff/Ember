/*
 * Copyright (c) 2016 - 2018 Ember
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <protocol/ResultCodes.h>
#include <boost/endian/arithmetic.hpp>
#include <cstdint>
#include <cstddef>

namespace ember::protocol::client {

namespace be = boost::endian;

class PlayerLogin final {
	State state_ = State::INITIAL;

public:
	be::little_uint64_at character_id;

	State read_from_stream(spark::BinaryStream& stream) try {
		BOOST_ASSERT_MSG(state_ != State::DONE, "Packet already complete - check your logic!");

		stream >> character_id;

		return (state_ = State::DONE);
	} catch(const spark::exception&) {
		return State::ERRORED;
	}

	void write_to_stream(spark::BinaryStream& stream) const {
		stream << character_id;
	}
};

} // cmsg, protocol, ember
