#pragma once

#include <functional>
#include <string>
#include <vector>

namespace rewire {

struct ClaudeSession {
    int pid = 0;
    std::string session_id;
    std::string cwd;
    std::string socket_path;
    std::string name;
    std::string status;
    std::string proc_start;
    long long updated_at = 0;
    bool reachable = false;
};

struct OutboundMessage {
    std::string socket_path;
    std::string session_id;
    std::string text;
    std::string from_address;
    std::string from_name = "rewire";
    std::string token;
};

struct SendResult {
    bool ok = false;
    std::string message_id;
    std::string error;
};

SendResult send_message(const OutboundMessage& message);
std::vector<ClaudeSession> list_sessions();
const ClaudeSession* match_project(const std::vector<ClaudeSession>& sessions, const std::string& project);

class ClaudeLink {
public:
    ClaudeLink();
    ~ClaudeLink();

    ClaudeLink(const ClaudeLink&) = delete;
    ClaudeLink& operator=(const ClaudeLink&) = delete;

    bool start(const std::string& project_root);
    void stop();
    bool listening() const;
    std::string address() const;
    void set_wake(std::function<void()> wake);
    SendResult send(const ClaudeSession& target, const std::string& text);
    std::vector<std::string> drain();

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace rewire
