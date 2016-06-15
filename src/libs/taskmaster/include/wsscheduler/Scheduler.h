/*
 * Copyright (c) 2016 Ember
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <wsscheduler/Worker.h>
#include <wsscheduler/Common.h>
#include <wsscheduler/Task.h>
#include <shared/threading/Semaphore.h>
#include <logger/Logging.h>
#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <cstddef>

namespace ember { namespace task { namespace ws {

class Worker;

class Scheduler {
	thread_local static std::size_t allocated_tasks_;
	thread_local static int worker_id_;
	
	const std::size_t WORKER_COUNT_;
	const std::size_t MAX_TASKS_;

	std::vector<Dequeue> queues_;
	std::vector<std::vector<Task>> task_pool_;
	std::vector<std::thread> workers_;
	std::atomic_bool stopped_;

	log::Logger* logger_;

	void spawn_worker(int index);
	void start_worker();
	Dequeue* local_queue();

	bool completion_check(Task* task);
	void execute(Task* task);
	void finish(Task* task);
	Task* fetch_task();

public:
	Scheduler(std::size_t workers, std::size_t max_tasks, log::Logger* logger);
	~Scheduler();

	void stop();

	Task* create_task(TaskFunc func, Task* parent = nullptr);
	void add_continuation(Task* ancestor, Task* continuation);
	void run(Task* task);
	void wait(Task* task);

	friend class Worker;
};

}}} // ws, task, ember