#include "prx/libSceAgcDriver/Execution/include/PreparationPool.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using AgcDriver::DriverDetail::PreparationPool;
using AgcDriver::DriverDetail::PreparationStopped;
using namespace std::chrono_literals;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

class Gate {
public:
    void Arrive() {
        {
            std::lock_guard lock(mutex);
            ++arrivals;
        }
        changed.notify_all();
    }

    bool Wait(unsigned target) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return arrivals >= target; });
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    unsigned arrivals = 0;
};

void OverlapAndOrdering() {
    PreparationPool pool;
    Gate entered;
    const auto action = [&](int value) {
        entered.Arrive();
        Require(entered.Wait(2), "different templates did not overlap");
        return std::make_unique<int>(value);
    };
    const auto first = pool.RunPair([&] { return action(3); }, [&] { return action(7); }, true);
    Require(*first[0] == 3 && *first[1] == 7, "template publication order changed");
    const auto second = pool.RunPair([] { return 11; }, [] { return 13; }, true);
    Require(second[0] == 11 && second[1] == 13, "second pair failed");
    const auto counts = pool.Totals();
    Require(counts.workerStarts == 1 && counts.parallelPairs == 2 && counts.active == 0 && counts.peakActive == 2,
        "persistent worker was not reused or admission exceeded two lanes");
}

void SerialAndPrimaryFailure() {
    PreparationPool pool;
    unsigned order = 0;
    const auto result = pool.RunPair([&] { Require(order++ == 0, "full template was not first"); return 5; },
        [&] { Require(order++ == 1, "partial template was not second"); return 9; }, false);
    Require(result[0] == 5 && result[1] == 9 && pool.Totals().workerStarts == 0, "warm pair started a worker");
    bool secondary = false;
    try {
        static_cast<void>(pool.RunPair([]() -> int { throw std::runtime_error("primary"); },
            [&] { secondary = true; return 1; }, false));
        throw std::runtime_error("primary error was suppressed");
    } catch (const std::runtime_error& error) {
        Require(std::string_view(error.what()) == "primary" && !secondary, "serial failure ordering changed");
    }
}

void JoinedErrorsAndLifetime() {
    PreparationPool pool;
    Gate entered;
    std::atomic<bool> secondaryFinished = false;
    auto input = std::make_shared<int>(17);
    std::weak_ptr<int> lifetime = input;
    try {
        static_cast<void>(pool.RunPair([&]() -> int {
            entered.Arrive();
            Require(entered.Wait(2), "secondary did not start before primary error");
            throw std::runtime_error("primary");
        }, [held = input, &entered, &secondaryFinished]() -> int {
            entered.Arrive();
            Require(entered.Wait(2) && *held == 17, "snapshot did not survive the worker");
            secondaryFinished = true;
            throw std::runtime_error("secondary");
        }, true));
        throw std::runtime_error("parallel errors were suppressed");
    } catch (const std::runtime_error& error) {
        Require(std::string_view(error.what()) == "primary", "secondary failure replaced the primary error");
        Require(secondaryFinished.load(), "caller returned before failing secondary joined");
    }
    input.reset();
    Require(lifetime.expired(), "worker retained request ownership after joining");
    try {
        static_cast<void>(pool.RunPair([] { return 1; }, []() -> int { throw std::runtime_error("secondary"); }, true));
        throw std::runtime_error("secondary error was suppressed");
    } catch (const std::runtime_error& error) {
        Require(std::string_view(error.what()) == "secondary", "secondary failure was not propagated");
    }
    Require(pool.Totals().active == 0, "failed preparation leaked an admission slot");
}

void BoundedConcurrentCallers() {
    PreparationPool pool;
    std::atomic<unsigned> running = 0;
    std::atomic<unsigned> peak = 0;
    Gate active;
    std::latch release(1);
    auto held = [&] {
        const auto count = running.fetch_add(1) + 1;
        auto observed = peak.load();
        while (observed < count && !peak.compare_exchange_weak(observed, count)) {}
        active.Arrive();
        release.wait();
        running.fetch_sub(1);
        return 1;
    };
    auto initial = std::async(std::launch::async, [&] { return pool.RunPair(held, held, true); });
    const auto started = active.Wait(2);
    if (!started) release.count_down();
    Require(started, "initial two lanes did not start");
    std::vector<std::future<std::array<int, 2>>> callers;
    for (unsigned i = 0; i < 8; ++i) callers.push_back(std::async(std::launch::async, [&] {
        return pool.RunPair([] { return 2; }, [] { return 3; }, true);
    }));
    const auto bounded = pool.Totals().active == 2 && peak.load() == 2;
    release.count_down();
    Require(bounded, "heavy admissions exceeded two before release");
    Require(initial.get() == std::array<int, 2>{1, 1}, "initial pair failed");
    for (auto& caller : callers) Require(caller.get() == std::array<int, 2>{2, 3}, "concurrent caller failed");
    Require(pool.Totals().active == 0 && pool.Totals().peakActive == 2, "concurrent callers leaked or exceeded admission");
}

void NoSecondPermitDeadlock() {
    PreparationPool pool;
    Gate entered;
    const auto action = [&] {
        entered.Arrive();
        Require(entered.Wait(2), "one-per-caller fallback deadlocked");
        return 1;
    };
    auto first = std::async(std::launch::async, [&] { return pool.RunPair(action, [] { return 2; }, false); });
    Require(entered.Wait(1), "first serial admission did not start");
    auto second = std::async(std::launch::async, [&] { return pool.RunPair(action, [] { return 3; }, true); });
    Require(first.get() == std::array<int, 2>{1, 2} && second.get() == std::array<int, 2>{1, 3}, "fallback changed results");
    Require(pool.Totals().workerStarts == 0, "fallback started an inadmissible extra worker");
}

void StopAndCancellation() {
    PreparationPool pool;
    Gate entered;
    std::latch release(1);
    std::atomic<unsigned> finished = 0;
    auto held = [&] {
        entered.Arrive();
        release.wait();
        ++finished;
        return 1;
    };
    auto current = std::async(std::launch::async, [&] {
        try {
            static_cast<void>(pool.RunPair(held, held, true));
            return false;
        } catch (const PreparationStopped&) {
            return true;
        }
    });
    const auto started = entered.Wait(2);
    if (!started) release.count_down();
    Require(started, "shutdown fixture did not start two lanes");
    std::stop_source cancellation;
    Gate waiting;
    auto blocked = std::async(std::launch::async, [&] {
        waiting.Arrive();
        try {
            static_cast<void>(pool.RunPair([] { return 1; }, [] { return 2; }, true, cancellation.get_token()));
            return false;
        } catch (const PreparationStopped&) {
            return true;
        }
    });
    Require(waiting.Wait(1), "cancelled caller did not enter");
    cancellation.request_stop();
    const auto cancelled = blocked.wait_for(5s) == std::future_status::ready;
    auto shutdown = std::async(std::launch::async, [&] { pool.Stop(); });
    const auto stillWaiting = shutdown.wait_for(50ms) == std::future_status::timeout;
    release.count_down();
    Require(cancelled && blocked.get(), "stop token did not wake admission");
    Require(stillWaiting && current.get(), "shutdown failed to join both in-flight templates");
    shutdown.get();
    Require(finished == 2 && pool.Totals().active == 0, "shutdown lost an active template");
    bool rejected = false;
    try {
        static_cast<void>(pool.RunPair([] { return 1; }, [] { return 2; }, true));
    } catch (const PreparationStopped&) {
        rejected = true;
    }
    Require(rejected, "shutdown admitted a new preparation");
    pool.Stop();
}

}

int main() {
    try {
        OverlapAndOrdering();
        SerialAndPrimaryFailure();
        JoinedErrorsAndLifetime();
        BoundedConcurrentCallers();
        NoSecondPermitDeadlock();
        StopAndCancellation();
        std::puts("Preparation pool tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
