/*
* Copyright 2012-2013 Valentin Milea
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*   http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "ExUtil.h"
#include "Looper/Thread.h"
#include <CppAwait/Awaitable.h>
#include <random>
#include <cmath>
#include <boost/asio.hpp>

//
// ABOUT: how to implement an Awaitable on top of threads,
//        how to handle interruption
//

// try to use std::thread, fallback to boost::thread
//
using namespace loo::lthread;
using namespace loo::lchrono;


// run loop
static boost::asio::io_service sIo;


static ut::Awaitable asyncCountdown()
{
    return ut::startAsync("asyncCountdown", []() {
        timed_mutex mutex;
        condition_variable_any cond;
        bool isInterrupted = false;

        ut::Awaitable awtLiftoff("evt-liftoff");

        thread countdownThread([&](ut::Completer completer) {
            unique_lock<timed_mutex> lock(mutex);

            for (int i = 3; i > 0 && !isInterrupted; i--) {
                printf ("%d seconds until liftoff...\n", i);

                // up to 1 second of interruptible sleep
                auto timepoint = chrono::steady_clock::now() + chrono::milliseconds(1000);
                cond.wait_until(lock, timepoint, [&] { return isInterrupted; });
            }

            if (isInterrupted) {
                printf ("liftoff aborted!\n");
            } else {
                printf ("liftoff!\n");

                // Safe coroutine resumal: schedule completion on main thread.
                //
                // It's possible the abort comes too late to prevent liftoff. Completer checks
                // the awaitable is still valid, so nothing happens if it runs after thread.join().
                //
                sIo.post(completer);
            }
        }, awtLiftoff.takeCompleter());

        try {
            // suspend until liftoff or abort
            awtLiftoff.await();
        } catch (const ut::ForcedUnwind&) {
            printf ("aborting liftoff...\n");

            // launch aborted, interrupt countdown thread
            lock_guard<timed_mutex> _(mutex);
            isInterrupted = true;
            cond.notify_one();
        }

        countdownThread.join();
        printf ("\njoined countdown thread\n");
    });
}

static ut::Awaitable asyncKey()
{
    return ut::startAsync("asyncKey", []() {
        ut::Coro *coro = ut::currentCoro();

        thread keyThread([&]() {
            // Wait for user to hit [Return]. Uninterruptible blocking calls
            // are generally a bad idea. Here we pretend it's safe to kill
            // thread at any time.
            readLine();

            sIo.post([&]() {
                // vulnerable to coro being destroyed in the meantime
                ut::yieldTo(coro);
            });
        });


        try {
            // ok -- yield explicitly to master context
            ut::yield();

            keyThread.join();
            printf ("\njoined key thread\n");
        } catch (const ut::ForcedUnwind&) {
            keyThread.detach();
            printf ("\nkilled key thread\n");
        }
    });
}

static ut::Awaitable asyncThread()
{
    return ut::startAsync("asyncThread", []() {
        printf ("hit [Return] to abort launch\n\n");

        {
            ut::Awaitable awtCountdown = asyncCountdown();
            ut::Awaitable awtKey = asyncKey();

            // wait until liftoff or abort
            ut::awaitAny(awtCountdown, awtKey);

            // scope end, the other awaitable will interrupt itself

            sIo.stop();
        }
    });
}

void ex_awaitThread()
{
    ut::Awaitable awt = asyncThread();

    // io_service::run() quits immediately if there's nothing scheduled

    // setup a dummy timer
    boost::asio::deadline_timer timer(sIo, boost::posix_time::hours(1));
    timer.async_wait([](const boost::system::error_code& ec) { });

    sIo.run();
}
