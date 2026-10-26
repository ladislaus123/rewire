#include "watcher.hpp"

#include "parser.hpp"

#include <chrono>
#include <filesystem>
#include <sstream>

namespace rewire {
namespace fs = std::filesystem;

SourceWatcher::SourceWatcher(std::string root) : root_(std::move(root)) {}

SourceWatcher::~SourceWatcher() { stop(); }

std::string SourceWatcher::fingerprint() const {
    std::ostringstream out;
    for (const auto& file : list_source_files(root_)) {
        std::error_code error;
        auto size = fs::file_size(file, error);
        auto when = fs::last_write_time(file, error);
        out << file << ' ' << static_cast<unsigned long long>(size) << ' '
            << static_cast<long long>(when.time_since_epoch().count()) << '\n';
    }
    return out.str();
}

void SourceWatcher::start(std::function<void()> on_change) {
    stop();
    stop_ = false;
    on_change_ = std::move(on_change);
    std::string seen = fingerprint();
    thread_ = std::thread([this, seen]() mutable {
        while (!stop_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            if (stop_.load()) break;
            std::string now = fingerprint();
            if (now == seen) continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            now = fingerprint();
            if (now == seen || stop_.load()) continue;
            seen = now;
            if (on_change_) on_change_();
        }
    });
}

void SourceWatcher::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

}  // namespace rewire
