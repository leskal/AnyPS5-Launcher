#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/Job.hpp"

namespace launcher {

struct JobOutcome {
    std::string title;
    bool succeeded = false;
    bool cancelled = false;
    std::string message;
};

class BackgroundJob {
public:
    using Work = std::function<std::string(JobControl&)>;

    ~BackgroundJob() {
        control_->Cancel();
        if (worker_.joinable()) worker_.join();
    }

    bool Busy() const { return busy_; }
    bool Cancellable() const { return cancellable_; }
    const std::string& Title() const { return title_; }
    JobControl& Control() { return *control_; }

    void Start(std::string title, bool cancellable, Work work) {
        if (busy_) return;
        if (worker_.joinable()) worker_.join();
        control_ = std::make_shared<JobControl>();
        title_ = std::move(title);
        cancellable_ = cancellable;
        busy_ = true;
        worker_ = std::thread([this, control = control_, work = std::move(work)] {
            JobOutcome outcome{title_, false, false, {}};
            try {
                outcome.message = work(*control);
                outcome.succeeded = true;
            } catch (const Cancelled&) {
                outcome.cancelled = true;
                outcome.message = "Cancelled; nothing was written.";
            } catch (const std::exception& failure) {
                outcome.message = failure.what();
            }
            std::lock_guard lock(mutex_);
            outcome_ = std::move(outcome);
            busy_ = false;
        });
    }

    std::optional<JobOutcome> TakeOutcome() {
        std::lock_guard lock(mutex_);
        auto outcome = std::move(outcome_);
        outcome_.reset();
        return outcome;
    }

private:
    std::thread worker_;
    std::atomic<bool> busy_ = false;
    bool cancellable_ = false;
    std::string title_;
    std::shared_ptr<JobControl> control_ = std::make_shared<JobControl>();
    std::mutex mutex_;
    std::optional<JobOutcome> outcome_;
};

}
