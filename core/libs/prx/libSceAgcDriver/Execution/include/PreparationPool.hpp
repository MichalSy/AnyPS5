#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PREPARATIONPOOL_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PREPARATIONPOOL_HPP

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

namespace AgcDriver::DriverDetail {

class PreparationStopped final : public std::exception {
public:
    const char* what() const noexcept override { return "shader preparation stopped"; }
};

class PreparationPool {
public:
    struct Counters {
        std::size_t active = 0;
        std::size_t peakActive = 0;
        std::size_t workerStarts = 0;
        std::size_t parallelPairs = 0;
        std::size_t serialPairs = 0;
    };

    PreparationPool() = default;
    PreparationPool(const PreparationPool&) = delete;
    PreparationPool& operator=(const PreparationPool&) = delete;
    ~PreparationPool() { Stop(); }

    template<typename First, typename Second>
    auto RunPair(First first, Second second, bool parallel, std::stop_token token = {}) {
        using Result = std::invoke_result_t<First&>;
        static_assert(std::is_same_v<Result, std::invoke_result_t<Second&>>);
        static_assert(!std::is_void_v<Result> && !std::is_reference_v<Result>);
        std::stop_callback cancelled(token, [this] { changed.notify_all(); });
        Acquire(token);
        const Permit primary{this};
        struct Work {
            Second* action;
            std::optional<Result> result;
            std::exception_ptr error;
        } work{&second, {}, {}};
        Job job{[](void* data) noexcept {
            auto& work = *static_cast<Work*>(data);
            try {
                work.result.emplace(std::invoke(*work.action));
            } catch (...) {
                work.error = std::current_exception();
            }
        }, &work};
        if (!Start(job, parallel, token)) {
            auto firstResult = std::invoke(first);
            CheckStop(token);
            auto secondResult = std::invoke(second);
            CheckStop(token);
            return std::array<Result, 2>{std::move(firstResult), std::move(secondResult)};
        }
        std::optional<Result> firstResult;
        std::exception_ptr firstError;
        try {
            firstResult.emplace(std::invoke(first));
        } catch (...) {
            firstError = std::current_exception();
        }
        Wait(job);
        if (firstError) std::rethrow_exception(firstError);
        if (work.error) std::rethrow_exception(work.error);
        CheckStop(token);
        return std::array<Result, 2>{std::move(*firstResult), std::move(*work.result)};
    }

    Counters Totals() const {
        std::lock_guard lock(mutex);
        return counters;
    }

    void Stop() {
        std::lock_guard shutdown(shutdownMutex);
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        if (worker.joinable()) worker.join();
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] { return counters.active == 0; });
    }

private:
    struct Job {
        void (*execute)(void*) noexcept;
        void* state;
        bool done = false;
    };

    struct Permit {
        PreparationPool* pool;
        ~Permit() { pool->Release(); }
    };

    void Acquire(std::stop_token token) {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return stopping || token.stop_requested() || counters.active < 2; });
        if (stopping || token.stop_requested()) throw PreparationStopped{};
        ++counters.active;
        counters.peakActive = std::max(counters.peakActive, counters.active);
    }

    void Release() {
        {
            std::lock_guard lock(mutex);
            --counters.active;
        }
        changed.notify_all();
    }

    void CheckStop(std::stop_token token) const {
        std::lock_guard lock(mutex);
        if (stopping || token.stop_requested()) throw PreparationStopped{};
    }

    bool Start(Job& job, bool parallel, std::stop_token token) {
        std::lock_guard lock(mutex);
        if (stopping || token.stop_requested()) throw PreparationStopped{};
        if (!parallel || counters.active >= 2 || workerActive) {
            ++counters.serialPairs;
            return false;
        }
        if (!worker.joinable()) {
            try {
                worker = std::thread([this] { Run(); });
            } catch (const std::system_error&) {
                ++counters.serialPairs;
                return false;
            }
            ++counters.workerStarts;
        }
        ++counters.active;
        counters.peakActive = std::max(counters.peakActive, counters.active);
        ++counters.parallelPairs;
        workerActive = true;
        pending = &job;
        changed.notify_all();
        return true;
    }

    void Wait(const Job& job) {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return job.done; });
    }

    void Run() {
        for (;;) {
            Job* job;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [this] { return stopping || pending != nullptr; });
                if (pending == nullptr) return;
                job = std::exchange(pending, nullptr);
            }
            job->execute(job->state);
            {
                std::lock_guard lock(mutex);
                job->done = true;
                workerActive = false;
                --counters.active;
            }
            changed.notify_all();
        }
    }

    mutable std::mutex mutex;
    std::mutex shutdownMutex;
    std::condition_variable changed;
    std::thread worker;
    Job* pending = nullptr;
    bool workerActive = false;
    bool stopping = false;
    Counters counters;
};

}

#endif
