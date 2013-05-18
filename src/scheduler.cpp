// Copyright (c) 2013 Amanieu d'Antras
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <cstdlib>

#include <async++.h>

#include "aligned_alloc.h"
#include "auto_reset_event.h"
#include "fifo_queue.h"
#include "work_steal_queue.h"

// thread_local keyword support
#ifdef __clang__
# if __has_feature(cxx_thread_local)
#  define HAVE_THREAD_LOCAL
# endif
#elif __GNUC__ * 100 + __GNUC_MINOR__ >= 408
# define HAVE_THREAD_LOCAL
#endif

// For compilers that don't support thread_local, use __thread/declspec(thread)
// which has the same semantics but doesn't support dynamic initialization/destruction.
#ifndef HAVE_THREAD_LOCAL
# ifdef _MSC_VER
#  define thread_local __declspec(thread)
# else
#  define thread_local __thread
# endif
#endif

// Cacheline alignment to avoid false sharing between different threads
#ifdef __GNUC__
#define CACHELINE_ALIGN __attribute__((aligned(64)))
#elif _MSC_VER
#define CACHELINE_ALIGN __declspec(align(64))
#else
#define CACHELINE_ALIGN alignas(64)
#endif

namespace async {
namespace detail {

// Current thread's index in the pool, -1 if not in the pool
static thread_local int thread_id = -1;

// Number of threads in the pool
static int num_threads;

// Per-thread data, aligned to cachelines to avoid false sharing
struct CACHELINE_ALIGN thread_data_t {
	work_steal_queue queue;
	std::thread handle;
	std::minstd_rand rng;
	auto_reset_event event;
};

// Custom deleter for the per-thread data, since we can't use delete[]
// for aligned data.
struct thread_data_deleter {
	void operator()(thread_data_t* thread_data)
	{
		for (int i = 0; i < num_threads; i++)
			thread_data[i].~thread_data_t();
		aligned_free(thread_data);
	}
};

// Array of per-thread data
static std::unique_ptr<thread_data_t[], thread_data_deleter> thread_data;

// Global queue for tasks from outside the pool
static std::unique_ptr<fifo_queue> public_queue;

// Shutdown request indicator
static bool shutdown = false;

// List of threads waiting for tasks to run
static spinlock waiters_lock;
static std::vector<auto_reset_event*> waiters;

// Register a thread on the waiter list
static void register_waiter(auto_reset_event& thread_event)
{
	std::lock_guard<spinlock> lock(waiters_lock);
	waiters.push_back(&thread_event);
}

// Remove a thread from the waiter list
static void remove_waiter(auto_reset_event& thread_event)
{
	std::lock_guard<spinlock> lock(waiters_lock);
	waiters.erase(std::remove(waiters.begin(), waiters.end(), &thread_event), waiters.end());
}

// Try to steal a task from another thread's queue
static void* steal_task()
{
	// Make a list of victim thread ids and shuffle it
	std::vector<int> victims(num_threads);
	std::iota(victims.begin(), victims.end(), 0);
	std::shuffle(victims.begin(), victims.end(), thread_data[thread_id].rng);

	// Try to steal from another thread
	for (int i: victims) {
		// Don't try to steal from ourself
		if (i == thread_id)
			continue;

		void* t = thread_data[i].queue.steal();
		if (t)
			return t;
	}

	// No tasks found, but we might have missed one if it was just added. In
	// practice this doesn't really matter since it will be handled by another
	// thread.
	return nullptr;
}

// Wait for a task to complete (for worker threads inside thread pool)
static void threadpool_wait_handler(task_wait_handle wait_task)
{
	// Get our thread's event
	auto_reset_event& thread_event = thread_data[thread_id].event;

	// Flag indicating if we have added a continuation to the task
	bool added_continuation = false;

	// Loop while waiting for the task to complete
	while (true) {
		// Check if the task has finished
		if (wait_task.ready())
			return;

		// Try to get a task from the local queue
		if (void* t = thread_data[thread_id].queue.pop()) {
			task_run_handle::from_void_ptr(t).run();
			continue;
		}

		while (true) {
			// Try to fetch from the public queue
			if (void* t = public_queue->pop()) {
				task_run_handle::from_void_ptr(t).run();
				break;
			}

			// Try to steal a task
			if (void* t = steal_task()) {
				task_run_handle::from_void_ptr(t).run();
				break;
			}

			// No tasks found, so sleep until something happens
			// Reset our event
			thread_event.reset();

			// Memory barrier required to ensure reset is done before checking state
			std::atomic_thread_fence(std::memory_order_seq_cst);

			// Check again here to avoid a missed wakeup
			if (wait_task.ready())
				return;

			// If a continuation has not been added yet, add it
			if (!added_continuation) {
				// Create a continuation for the task we are waiting for
				wait_task.on_finish([&thread_event] {
					// Just signal the thread event
					thread_event.signal();
				});
				added_continuation = true;
			}

			// Add our thread to the list of waiting threads
			register_waiter(thread_event);

			// Wait for our event to be signaled when a task is scheduled or
			// the task we are waiting for has completed.
			thread_event.wait();

			// Remove our thread from the list of waiting threads
			remove_waiter(thread_event);

			// Check if the task has finished
			if (wait_task.ready())
				return;
		}
	}
}

// Worker thread main loop
static void worker_thread(int id)
{
	// Save the thread id
	thread_id = id;

	// Set the wait handler so threads from the pool do useful work while
	// waiting for another task to finish.
	set_thread_wait_handler(threadpool_wait_handler);

	// Seed the random number generator with our id. This gives each thread a
	// different steal order.
	thread_data[thread_id].rng.seed(thread_id);

	// Get our thread's event
	auto_reset_event& thread_event = thread_data[thread_id].event;

	// Main loop
	while (true) {
		// Try to get a task from the local queue
		if (void* t = thread_data[thread_id].queue.pop()) {
			task_run_handle::from_void_ptr(t).run();
			continue;
		}

		// Stealing loop
		while (true) {
			// Try to fetch from the public queue
			if (void* t = public_queue->pop()) {
				task_run_handle::from_void_ptr(t).run();
				break;
			}

			// If there are no local or public tasks, we can shut down
			if (shutdown)
				return;

			// Try to steal a task
			if (void* t = steal_task()) {
				task_run_handle::from_void_ptr(t).run();
				break;
			}

			// No tasks found, so wait for a task to be scheduled
			// Reset our event
			thread_event.reset();

			// Add our thread to the list of waiting threads
			register_waiter(thread_event);

			// Check again for shutdown, otherwise we might miss the wakeup
			if (shutdown)
				return;

			// Wait for our event to be signaled when a task is scheduled
			thread_event.wait();
		}
	}
}

// Thread pool scheduler implementation
class threadpool_scheduler_impl: public scheduler {
public:
	// Initialize thread pool on first use
	threadpool_scheduler_impl()
	{
		// Get the requested number of threads from the environment
		// If that fails, use the number of CPUs in the system
		const char *s = std::getenv("LIBASYNC_NUM_THREADS");
		num_threads = std::thread::hardware_concurrency();
		try {
			if (s)
				num_threads = std::stoi(s);
		} catch (...) {}

		// Make sure thread count isn't something ridiculous
		num_threads = std::max(num_threads, 1);

		// Reserve space in the waiters list to avoid resizes while running
		waiters.reserve(num_threads);

		// Allocate public queue
		public_queue.reset(new fifo_queue);

		// Allocate per-thread data
		thread_data.reset(static_cast<thread_data_t*>(aligned_alloc(sizeof(thread_data_t) * num_threads, std::alignment_of<thread_data_t>::value)));
		for (int i = 0; i < num_threads; i++)
			new (&thread_data[i]) thread_data_t;

		// Start worker threads
		for (int i = 0; i < num_threads; i++)
			thread_data[i].handle = std::thread(worker_thread, i);
	}

	// Wait for all currently running tasks to finish
	~threadpool_scheduler_impl()
	{
		// Signal shutdown
		shutdown = true;

		// Wake up any sleeping threads
		{
			std::lock_guard<spinlock> lock(waiters_lock);
			for (auto_reset_event* i: waiters)
				i->signal();
			waiters.clear();
		}

		// Wait for the threads to finish
		for (int i = 0; i < num_threads; i++)
			thread_data[i].handle.join();

		// Flush the public queue
		while (void* t = public_queue->pop())
			task_run_handle::from_void_ptr(t).run();
	}

	// Schedule a task on the thread pool
	virtual void schedule(task_run_handle t) override final
	{
		// If we have already shut down, just run the task inline
		if (shutdown) {
			t.run();
			return;
		}

		// Check if we are in the thread pool
		if (thread_id != -1) {
			// Push task onto our task queue
			thread_data[thread_id].queue.push(t.to_void_ptr());
		} else {
			// Push task onto the public queue
			public_queue->push(t.to_void_ptr());
		}

		// If there are no sleeping threads, return.
		// Technically this isn't thread safe, but we don't care because we
		// check again inside the lock.
		if (waiters.empty())
			return;

		// Get a thread to wake up from the list
		auto_reset_event* wakeup;
		{
			std::lock_guard<spinlock> lock(waiters_lock);

			// Check again if there are waiters
			if (waiters.empty())
				return;

			// Pop a thread from the list and wake it up
			wakeup = waiters.back();
			waiters.pop_back();
		}

		// Signal the thread
		wakeup->signal();
	}
};

// Inline scheduler implementation
class inline_scheduler_impl: public scheduler {
public:
	virtual void schedule(task_run_handle t) override final
	{
		t.run();
	}
};

// Thread scheduler implementation
class thread_scheduler_impl: public scheduler {
public:
	virtual void schedule(task_run_handle t) override final
	{
		std::thread([](task_run_handle t) {
			t.run();
		}, std::move(t));
	}
};

// Wait for a task to complete (for threads outside thread pool)
static void generic_wait_handler(task_wait_handle wait_task)
{
	// Create an event to wait on
	auto_reset_event thread_event;

	// Create a continuation for the task we are waiting for
	wait_task.on_finish([&thread_event] {
		// Just signal the thread event
		thread_event.signal();
	});

	// Wait for the event to be set
	thread_event.wait();
}

// Wait handler function, per-thread, defaults to generic version
static thread_local wait_handler thread_wait_handler = generic_wait_handler;

// Wait for a task to complete
void wait_for_task(task_base* wait_task)
{
	// Dispatch to the current thread's wait handler
	thread_wait_handler(task_wait_handle(wait_task));
}

} // namespace detail

wait_handler set_thread_wait_handler(wait_handler handler)
{
	wait_handler old = detail::thread_wait_handler;
	detail::thread_wait_handler = handler;
	return old;
}

scheduler& threadpool_scheduler()
{
	static detail::threadpool_scheduler_impl sched;
	return sched;
}

scheduler& inline_scheduler()
{
	static detail::inline_scheduler_impl sched;
	return sched;
}

scheduler& thread_scheduler()
{
	static detail::thread_scheduler_impl sched;
	return sched;
}

} // namespace async
