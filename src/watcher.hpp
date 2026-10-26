#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace rewire {

class SourceWatcher {
public:
    explicit SourceWatcher(std::string root);
    ~SourceWatcher();

    SourceWatcher(const SourceWatcher&) = delete;
    SourceWatcher& operator=(const SourceWatcher&) = delete;

    void start(std::function<void()> on_change);
    void stop();

private:
    std::string fingerprint() const;

    std::string root_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::function<void()> on_change_;
};

}  // namespace rewire
