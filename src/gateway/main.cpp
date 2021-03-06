/*
 * Copyright (c) 2015 - 2018 Ember
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "Config.h"
#include "Locator.h"
#include "FilterTypes.h"
#include "RealmQueue.h"
#include "ServicePool.h"
#include "AccountService.h"
#include "EventDispatcher.h"
#include "CharacterService.h"
#include "RealmService.h"
#include "NetworkListener.h"
#include <spark/Spark.h>
#include <conpool/ConnectionPool.h>
#include <conpool/Policies.h>
#include <conpool/drivers/AutoSelect.h>
#include <logger/Logging.h>
#include <shared/Banner.h>
#include <shared/util/EnumHelper.h>
#include <shared/Version.h>
#include <shared/util/Utility.h>
#include <shared/util/LogConfig.h>
#include <dbcreader/DBCReader.h>
#include <shared/database/daos/RealmDAO.h>
#include <shared/database/daos/UserDAO.h>
#include <shared/util/xoroshiro128plus.h>
#include <boost/asio.hpp>
#include <boost/program_options.hpp>
#include <botan/auto_rng.h>
#include <chrono>
#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <stdexcept>

const char* APP_NAME = "Realm Gateway";

namespace ep = ember::connection_pool;
namespace po = boost::program_options;
namespace ba = boost::asio;

using namespace std::chrono_literals;
using namespace std::placeholders;

namespace ember {

void launch(const po::variables_map& args, log::Logger* logger);
unsigned int check_concurrency(log::Logger* logger); // todo, move
po::variables_map parse_arguments(int argc, const char* argv[]);
void pool_log_callback(ep::Severity, std::string_view message, log::Logger* logger);
std::string category_name(const Realm& realm, const dbc::DBCMap<dbc::Cfg_Categories>& dbc);

} // ember

/*
 * We want to do the minimum amount of work required to get 
 * logging facilities and crash handlers up and running in main.
 *
 * Exceptions that aren't derived from std::exception are
 * left to the crash handler since we can't get useful information
 * from them.
 */
int main(int argc, const char* argv[]) try {
	using namespace ember;

	print_banner(APP_NAME);
	util::set_window_title(APP_NAME);

	const po::variables_map args = parse_arguments(argc, argv);

	auto logger = util::init_logging(args);
	log::set_global_logger(logger.get());
	LOG_INFO(logger) << "Logger configured successfully" << LOG_SYNC;

	launch(args, logger.get());
	LOG_INFO(logger) << APP_NAME << " terminated" << LOG_SYNC;
} catch(const std::exception& e) {
	std::cerr << e.what();
	return 1;
}

namespace ember {

void launch(const po::variables_map& args, log::Logger* logger) try {
#ifdef DEBUG_NO_THREADS
	LOG_WARN(logger) << "Compiled with DEBUG_NO_THREADS!" << LOG_SYNC;
#endif

	LOG_INFO(logger) << "Seeding xorshift RNG..." << LOG_SYNC;
	Botan::AutoSeeded_RNG rng;
	rng.randomize((Botan::byte*)ember::rng::xorshift::seed, sizeof(ember::rng::xorshift::seed));

	LOG_INFO(logger) << "Loading DBC data..." << LOG_SYNC;
	dbc::DiskLoader loader(args["dbc.path"].as<std::string>(), [&](auto message) {
		LOG_DEBUG(logger) << message << LOG_SYNC;
	});

	auto dbc_store = loader.load({"AddonData", "Cfg_Categories"});

	LOG_INFO(logger) << "Resolving DBC references..." << LOG_SYNC;
	dbc::link(dbc_store);

	LOG_INFO(logger) << "Initialising database driver..." << LOG_SYNC;
	auto db_config_path = args["database.config_path"].as<std::string>();
	auto driver(ember::drivers::init_db_driver(db_config_path));

	LOG_INFO(logger) << "Initialising database connection pool..." << LOG_SYNC;
	ep::Pool<decltype(driver), ep::CheckinClean, ep::ExponentialGrowth> pool(driver, 1, 1, 30s);
	
	pool.logging_callback([logger](auto severity, auto message) {
		pool_log_callback(severity, message, logger);
	});

	LOG_INFO(logger) << "Initialising DAOs..." << LOG_SYNC;
	auto realm_dao = ember::dal::realm_dao(pool);

	LOG_INFO(logger) << "Retrieving realm information..."<< LOG_SYNC;
	auto realm = realm_dao->get_realm(args["realm.id"].as<unsigned int>());
	
	if(!realm) {
		throw std::invalid_argument("Invalid realm ID supplied in configuration.");
	}
	
	// Validate category & region
	auto cat_name = category_name(*realm, dbc_store.cfg_categories);

	LOG_INFO(logger) << "Serving as gateway for " << realm->name
	                 << " (" << cat_name << ")" << LOG_SYNC;

	util::set_window_title(std::string(APP_NAME) + " - " + realm->name);

	// Set config
	Config config;
	config.max_slots = args["realm.max_slots"].as<unsigned int>();
	config.list_zone_hide = args["quirks.list_zone_hide"].as<bool>();
	config.realm = &realm.value();

	// Determine concurrency level
	unsigned int concurrency = check_concurrency(logger);

	if(args.count("misc.concurrency")) {
		concurrency = args["misc.concurrency"].as<unsigned int>();
	}

	// Start ASIO service pool
	LOG_INFO(logger) << "Starting service pool with " << concurrency << " threads..." << LOG_SYNC;
	ServicePool service_pool(concurrency);

	LOG_INFO(logger) << "Starting event dispatcher..." << LOG_SYNC;
	EventDispatcher dispatcher(service_pool);

	LOG_INFO(logger) << "Starting Spark service..." << LOG_SYNC;
	auto s_address = args["spark.address"].as<std::string>();
	auto s_port = args["spark.port"].as<std::uint16_t>();
	auto mcast_group = args["spark.multicast_group"].as<std::string>();
	auto mcast_iface = args["spark.multicast_interface"].as<std::string>();
	auto mcast_port = args["spark.multicast_port"].as<std::uint16_t>();
	auto spark_filter = log::Filter(FilterType::LF_SPARK);

	auto& service = service_pool.get_service();

	spark::Service spark("gateway-" + realm->name, service, s_address, s_port, logger);
	spark::ServiceDiscovery discovery(service, s_address, s_port, mcast_iface, mcast_group,
	                               mcast_port, logger);

	RealmQueue queue_service(service_pool.get_service());
	RealmService realm_svc(*realm, spark, discovery, logger);
	AccountService acct_svc(spark, discovery, logger);
	CharacterService char_svc(spark, discovery, config, logger);
	
	// set services - not the best design pattern but it'll do for now
	Locator::set(&dispatcher);
	Locator::set(&queue_service);
	Locator::set(&realm_svc);
	Locator::set(&acct_svc);
	Locator::set(&char_svc);
	Locator::set(&config);
	
	// Start network listener
	auto interface = args["network.interface"].as<std::string>();
	auto port = args["network.port"].as<std::uint16_t>();
	auto tcp_no_delay = args["network.tcp_no_delay"].as<bool>();

	LOG_INFO(logger) << "Starting network service on " << interface << ":" << port << LOG_SYNC;

	NetworkListener server(service_pool, interface, port, tcp_no_delay, logger);

	boost::asio::io_service wait_svc;
	boost::asio::signal_set signals(wait_svc, SIGINT, SIGTERM);

	signals.async_wait([&](const boost::system::error_code& error, int signal) {
		LOG_DEBUG(logger) << "Received signal " << signal << LOG_SYNC;
	});

	service.dispatch([&, logger]() {
		realm_svc.set_online();
		LOG_INFO(logger) << APP_NAME << " started successfully" << LOG_SYNC;
	});

	service_pool.run();
	wait_svc.run();

	LOG_INFO(logger) << APP_NAME << " shutting down..." << LOG_SYNC;
} catch(const std::exception& e) {
	LOG_FATAL(logger) << e.what() << LOG_SYNC;
}

std::string category_name(const Realm& realm, const dbc::DBCMap<dbc::Cfg_Categories>& dbc) {
	for(auto&& [k, record] : dbc) {
		if(record.category == realm.category && record.region == realm.region) {
			return record.name.en_gb;
		}
	}

	throw std::invalid_argument("Unknown category/region combination in database");
}

po::variables_map parse_arguments(int argc, const char* argv[]) {
	//Command-line options
	po::options_description cmdline_opts("Generic options");
	cmdline_opts.add_options()
		("help", "Displays a list of available options")
		("config,c", po::value<std::string>()->default_value("gateway.conf"),
			"Path to the configuration file");

	po::positional_options_description pos; 
	pos.add("config", 1);

	//Config file options
	po::options_description config_opts("Realm gateway configuration options");
	config_opts.add_options()
		("quirks.list_zone_hide", po::value<bool>()->required())
		("dbc.path", po::value<std::string>()->required())
		("misc.concurrency", po::value<unsigned int>())
		("realm.id", po::value<unsigned int>()->required())
		("realm.max_slots", po::value<unsigned int>()->required())
		("realm.reserved_slots", po::value<unsigned int>()->required())
		("spark.address", po::value<std::string>()->required())
		("spark.port", po::value<std::uint16_t>()->required())
		("spark.multicast_interface", po::value<std::string>()->required())
		("spark.multicast_group", po::value<std::string>()->required())
		("spark.multicast_port", po::value<std::uint16_t>()->required())
		("network.interface", po::value<std::string>()->required())
		("network.port", po::value<std::uint16_t>()->required())
		("network.tcp_no_delay", po::value<bool>()->required())
		("network.compression", po::value<std::uint8_t>()->required())
		("console_log.verbosity", po::value<std::string>()->required())
		("console_log.filter-mask", po::value<std::uint32_t>()->default_value(0))
		("console_log.colours", po::value<bool>()->required())
		("remote_log.verbosity", po::value<std::string>()->required())
		("remote_log.filter-mask", po::value<std::uint32_t>()->default_value(0))
		("remote_log.service_name", po::value<std::string>()->required())
		("remote_log.host", po::value<std::string>()->required())
		("remote_log.port", po::value<std::uint16_t>()->required())
		("file_log.verbosity", po::value<std::string>()->required())
		("file_log.filter-mask", po::value<std::uint32_t>()->default_value(0))
		("file_log.path", po::value<std::string>()->default_value("gateway.log"))
		("file_log.timestamp_format", po::value<std::string>())
		("file_log.mode", po::value<std::string>()->required())
		("file_log.size_rotate", po::value<std::uint32_t>()->required())
		("file_log.midnight_rotate", po::value<bool>()->required())
		("file_log.log_timestamp", po::value<bool>()->required())
		("file_log.log_severity", po::value<bool>()->required())
		("database.config_path", po::value<std::string>()->required())
		("metrics.enabled", po::value<bool>()->required())
		("metrics.statsd_host", po::value<std::string>()->required())
		("metrics.statsd_port", po::value<std::uint16_t>()->required())
		("monitor.enabled", po::value<bool>()->required())
		("monitor.interface", po::value<std::string>()->required())
		("monitor.port", po::value<std::uint16_t>()->required());

	po::variables_map options;
	po::store(po::command_line_parser(argc, argv).positional(pos).options(cmdline_opts).run(), options);
	po::notify(options);

	if(options.count("help")) {
		std::cout << cmdline_opts << "\n";
		std::exit(0);
	}

	std::string config_path = options["config"].as<std::string>();
	std::ifstream ifs(config_path);

	if(!ifs) {
		std::string message("Unable to open configuration file: " + config_path);
		throw std::invalid_argument(message);
	}

	po::store(po::parse_config_file(ifs, config_opts), options);
	po::notify(options);

	return options;
}

void pool_log_callback(ep::Severity severity, std::string_view message, log::Logger* logger) {
	using ember::LF_DB_CONN_POOL;

	switch(severity) {
		case(ep::Severity::DEBUG) :
			LOG_DEBUG_FILTER(logger, LF_DB_CONN_POOL) << message << LOG_ASYNC;
			break;
		case(ep::Severity::INFO) :
			LOG_INFO_FILTER(logger, LF_DB_CONN_POOL) << message << LOG_ASYNC;
			break;
		case(ep::Severity::WARN) :
			LOG_WARN_FILTER(logger, LF_DB_CONN_POOL) << message << LOG_ASYNC;
			break;
		case(ep::Severity::ERROR) :
			LOG_ERROR_FILTER(logger, LF_DB_CONN_POOL) << message << LOG_ASYNC;
			break;
		case(ep::Severity::FATAL) :
			LOG_FATAL_FILTER(logger, LF_DB_CONN_POOL) << message << LOG_ASYNC;
			break;
		default:
			LOG_ERROR_FILTER(logger, LF_DB_CONN_POOL) << "Unhandled pool log callback severity" << LOG_ASYNC;
			LOG_ERROR_FILTER(logger, LF_DB_CONN_POOL) << message << LOG_ASYNC;
	}
}

/*
 * The concurrency level returned is usually the number of logical cores
 * in the machine but the standard doesn't guarantee that it won't be zero.
 * In that case, we just set the minimum concurrency level to two.
 */
unsigned int check_concurrency(log::Logger* logger) {
	unsigned int concurrency = std::thread::hardware_concurrency();

	if(!concurrency) {
		concurrency = 2;
		LOG_WARN(logger) << "Unable to determine concurrency level" << LOG_SYNC;
	}

#ifdef DEBUG_NO_THREADS
	return 0;
#else
	return concurrency;
#endif
}

} // ember