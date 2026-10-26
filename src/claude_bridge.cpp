#include "claude_bridge.hpp"

#include "json.hpp"
#include "util.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits.h>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace rewire {
namespace {

namespace fs = std::filesystem;

std::string home_dir() {
    const char* home = std::getenv("HOME");
    return home ? home : "";
}

std::string sha256_hex(const std::string& data) {
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.data(), static_cast<CC_LONG>(data.size()), hash);
    static const char* hex = "0123456789abcdef";
    std::string out(CC_SHA256_DIGEST_LENGTH * 2, '0');
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
        out[static_cast<std::size_t>(i) * 2] = hex[hash[i] >> 4];
        out[static_cast<std::size_t>(i) * 2 + 1] = hex[hash[i] & 0xf];
    }
    return out;
}

std::string random_hex(std::size_t bytes) {
    std::string raw(bytes, '\0');
    std::ifstream in("/dev/urandom", std::ios::binary);
    in.read(raw.data(), static_cast<std::streamsize>(bytes));
    static const char* hex = "0123456789abcdef";
    std::string out(bytes * 2, '0');
    for (std::size_t i = 0; i < bytes; ++i) {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        out[i * 2] = hex[c >> 4];
        out[i * 2 + 1] = hex[c & 0xf];
    }
    return out;
}

std::string uuid_v4() {
    unsigned char bytes[16];
    std::ifstream in("/dev/urandom", std::ios::binary);
    in.read(reinterpret_cast<char*>(bytes), 16);
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(hex[bytes[i] >> 4]);
        out.push_back(hex[bytes[i] & 0xf]);
    }
    return out;
}

std::string shell_capture(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return {};
    std::string out;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe)) out += buffer;
    pclose(pipe);
    return out;
}

std::string proc_start_of(int pid) {
    return trim_copy(shell_capture("ps -o lstart= -p " + std::to_string(pid) + " 2>/dev/null"));
}

bool pid_alive(int pid) {
    return pid > 0 && kill(pid, 0) == 0;
}

bool same_text(std::string a, std::string b) {
    return collapse_ws(a, 200) == collapse_ws(b, 200);
}

// Claude stores procStart in UTC. `ps -o lstart=` prints the same instant in
// local time, so a plain string compare misses every live session.
bool same_proc_start(const std::string& stored, const std::string& live) {
    if (same_text(stored, live)) {
        return true;
    }
    auto parse = [](const std::string& text, std::tm& out) {
        std::memset(&out, 0, sizeof(out));
        return strptime(text.c_str(), "%a %b %d %H:%M:%S %Y", &out) != nullptr;
    };
    std::tm stored_tm{};
    std::tm live_tm{};
    if (!parse(stored, stored_tm) || !parse(live, live_tm)) {
        return false;
    }
    stored_tm.tm_isdst = 0;
    live_tm.tm_isdst = 0;
    std::time_t stored_time = timegm(&stored_tm);
    std::time_t live_time = timegm(&live_tm);
    if (stored_time < 0 || live_time < 0) {
        return false;
    }
    long long delta = std::llabs(static_cast<long long>(stored_time) - static_cast<long long>(live_time));
    if (delta == 0) {
        return true;
    }
    std::time_t now = std::time(nullptr);
    std::tm local_tm{};
    std::tm utc_tm{};
    localtime_r(&now, &local_tm);
    gmtime_r(&now, &utc_tm);
    local_tm.tm_isdst = 0;
    utc_tm.tm_isdst = 0;
    long long offset = std::llabs(static_cast<long long>(timegm(&local_tm)) - static_cast<long long>(timegm(&utc_tm)));
    return delta == offset || delta + 3600 == offset || delta == offset + 3600;
}

bool probe_socket(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        close(fd);
        return true;
    }
    if (errno != EINPROGRESS) {
        close(fd);
        return false;
    }
    pollfd pfd{fd, POLLOUT, 0};
    rc = poll(&pfd, 1, 200);
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    close(fd);
    return rc > 0 && err == 0;
}

std::string peer_token_for(int pid, const std::string& proc_start) {
    fs::path dir = fs::path(home_dir()) / ".claude" / "sessions";
    std::error_code error;
    if (!fs::exists(dir, error)) return {};
    std::string prefix = std::to_string(pid) + ".";
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0 || name.size() < 4 || name.substr(name.size() - 4) != ".key") {
            continue;
        }
        json::Value value = json::parse(read_text(entry.path().string()));
        const json::Value* token = value.find("peerToken");
        const json::Value* started = value.find("procStart");
        if (!token) continue;
        if (started && !proc_start.empty() && !same_proc_start(started->as_string(), proc_start)) {
            continue;
        }
        return token->as_string();
    }
    return {};
}

json::Value string_value(std::string text) {
    json::Value value;
    value.type = json::Value::Type::String;
    value.text = std::move(text);
    return value;
}

json::Value number_value(long long number) {
    json::Value value;
    value.type = json::Value::Type::Number;
    value.number = static_cast<double>(number);
    value.text = std::to_string(number);
    return value;
}

std::string sanitize_name(std::string name) {
    std::string out;
    for (unsigned char c : name) {
        if (c < 0x20 || c == '"' || c == '<' || c == '>') continue;
        out.push_back(static_cast<char>(c));
        if (out.size() >= 80) break;
    }
    return trim_copy(out);
}

std::string wrap_envelope(const std::string& from, const std::string& name, const std::string& body) {
    std::string safe = body;
    const std::string closing = "</cross-session-message";
    for (std::size_t at = 0; (at = safe.find(closing, at)) != std::string::npos; at += 2) {
        safe.replace(at, closing.size(), "<\\/cross-session-message");
    }
    std::string attrs;
    if (!from.empty()) {
        attrs += " from=\"" + from + "\"";
    }
    std::string clean = sanitize_name(name);
    if (!clean.empty()) {
        attrs += " from-name=\"" + clean + "\"";
    }
    return "<cross-session-message" + attrs + ">\n" + safe + "\n</cross-session-message>";
}

std::string envelope_body(const std::string& content) {
    const std::string open = "<cross-session-message";
    auto start = content.find(open);
    if (start == std::string::npos) return content;
    auto header_end = content.find('>', start);
    auto close = content.rfind("</cross-session-message>");
    if (header_end == std::string::npos || close == std::string::npos || close < header_end) {
        return content;
    }
    return trim_copy(content.substr(header_end + 1, close - header_end - 1));
}

bool write_all(int fd, const std::string& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::string read_available(int fd) {
    std::string out;
    char buffer[4096];
    while (out.size() < 1024 * 1024) {
        pollfd pfd{fd, POLLIN, 0};
        int rc = poll(&pfd, 1, 300);
        if (rc <= 0) break;
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        out.append(buffer, static_cast<std::size_t>(n));
    }
    return out;
}

bool socket_dir_ok(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    if (st.st_uid != getuid()) return false;
    if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) return false;
    return true;
}

std::string describe_frame(const json::Value& frame) {
    const json::Value* type = frame.find("type");
    if (!type) return {};
    if (type->as_string() == "control") {
        const json::Value* action = frame.find("action");
        const json::Value* status = frame.find("status");
        const json::Value* reason = frame.find("reason");
        std::string line = "Claude ";
        line += action ? action->as_string() : "control";
        if (status) line += " " + status->as_string();
        if (reason && !reason->as_string().empty()) line += " (" + reason->as_string() + ")";
        return line;
    }
    if (type->as_string() == "user") {
        const json::Value* message = frame.find("message");
        if (!message) return "Claude sent a message.";
        const json::Value* content = message->find("content");
        if (!content) return "Claude sent a message.";
        return "Claude: " + envelope_body(content->as_string());
    }
    return {};
}

}  // namespace

SendResult send_message(const OutboundMessage& message) {
    SendResult result;
    if (message.socket_path.empty()) {
        result.error = "Claude session has no inbox socket.";
        return result;
    }
    if (message.text.empty()) {
        result.error = "Message is empty.";
        return result;
    }
    if (!message.from_address.empty() && message.from_address.rfind("uds:", 0) != 0) {
        result.error = "Reply address must start with uds:.";
        return result;
    }
    result.message_id = uuid_v4();
    std::string content = message.text;
    if (!message.from_name.empty()) {
        content = wrap_envelope(message.from_address, message.from_name, message.text);
    }
    json::Value payload;
    payload.type = json::Value::Type::Object;
    json::Value body;
    body.type = json::Value::Type::Object;
    body.object.emplace_back("role", string_value("user"));
    body.object.emplace_back("content", string_value(content));
    payload.object.emplace_back("msgV", number_value(1));
    payload.object.emplace_back("msg_id", string_value(result.message_id));
    payload.object.emplace_back("type", string_value("user"));
    payload.object.emplace_back("message", std::move(body));
    payload.object.emplace_back("priority", string_value("next"));
    if (!message.from_address.empty()) {
        payload.object.emplace_back("from", string_value(message.from_address));
    }
    if (!message.session_id.empty()) {
        payload.object.emplace_back("session_id", string_value(message.session_id));
    }
    std::string bytes;
    if (!message.token.empty()) {
        json::Value auth;
        auth.type = json::Value::Type::Object;
        auth.object.emplace_back("type", string_value("auth"));
        auth.object.emplace_back("token", string_value(message.token));
        bytes += json::dump(auth);
        bytes.push_back('\n');
    }
    bytes += json::dump(payload);
    bytes.push_back('\n');
    if (bytes.size() > 1024 * 1024) {
        result.error = "Message is too large for the Claude inbox.";
        return result;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        result.error = "Could not open a socket.";
        return result;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (message.socket_path.size() >= sizeof(addr.sun_path)) {
        close(fd);
        result.error = "Socket path is too long.";
        return result;
    }
    std::strncpy(addr.sun_path, message.socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        result.error = "Could not connect to the Claude session inbox.";
        close(fd);
        return result;
    }
    if (!write_all(fd, bytes)) {
        result.error = "The Claude session closed before the message was written.";
        close(fd);
        return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    shutdown(fd, SHUT_WR);
    (void)read_available(fd);
    close(fd);
    result.ok = true;
    return result;
}

std::vector<ClaudeSession> list_sessions() {
    std::vector<ClaudeSession> sessions;
    fs::path dir = fs::path(home_dir()) / ".claude" / "sessions";
    std::error_code error;
    if (!fs::exists(dir, error)) return sessions;
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        std::string name = entry.path().filename().string();
        if (name.size() < 6 || name.substr(name.size() - 5) != ".json") continue;
        if (name.find('.') != name.size() - 5) continue;
        json::Value value = json::parse(read_text(entry.path().string()));
        const json::Value* pid = value.find("pid");
        const json::Value* socket = value.find("messagingSocketPath");
        if (!pid || !socket) continue;
        ClaudeSession session;
        session.pid = static_cast<int>(pid->as_int());
        session.socket_path = socket->as_string();
        if (const json::Value* id = value.find("sessionId")) session.session_id = id->as_string();
        if (const json::Value* cwd = value.find("cwd")) session.cwd = cwd->as_string();
        if (const json::Value* label = value.find("name")) session.name = label->as_string();
        if (const json::Value* status = value.find("status")) session.status = status->as_string();
        if (const json::Value* started = value.find("procStart")) session.proc_start = started->as_string();
        if (const json::Value* updated = value.find("updatedAt")) session.updated_at = updated->as_int();
        if (const json::Value* version = value.find("version")) {
            if (version->as_string().rfind("rewire", 0) == 0) continue;
        }
        if (session.name.empty()) session.name = "claude-" + std::to_string(session.pid);
        if (!pid_alive(session.pid)) continue;
        std::string live = proc_start_of(session.pid);
        if (!session.proc_start.empty() && !live.empty() && !same_proc_start(session.proc_start, live)) continue;
        session.reachable = probe_socket(session.socket_path);
        if (!session.reachable) continue;
        if (session.status.empty()) session.status = "unknown";
        sessions.push_back(std::move(session));
    }
    std::sort(sessions.begin(), sessions.end(), [](const ClaudeSession& a, const ClaudeSession& b) {
        return a.updated_at > b.updated_at;
    });
    return sessions;
}

const ClaudeSession* match_project(const std::vector<ClaudeSession>& sessions, const std::string& project) {
    std::error_code error;
    fs::path canon = fs::weakly_canonical(project, error);
    std::string root = error ? project : canon.string();
    const ClaudeSession* best = nullptr;
    int best_score = 0;
    for (const auto& session : sessions) {
        std::error_code cwd_error;
        fs::path session_cwd = fs::weakly_canonical(session.cwd, cwd_error);
        std::string cwd = cwd_error ? session.cwd : session_cwd.string();
        int score = 0;
        if (cwd == root) {
            score = 100000 + static_cast<int>(cwd.size());
        } else if (root.rfind(cwd + "/", 0) == 0) {
            score = 50000 + static_cast<int>(cwd.size());
        } else if (cwd.rfind(root + "/", 0) == 0) {
            score = 10000 + static_cast<int>(cwd.size());
        }
        if (score > best_score) {
            best_score = score;
            best = &session;
        }
    }
    return best;
}

struct ClaudeLink::Impl {
    std::string project;
    std::string socket_path;
    std::string address;
    std::string session_id;
    std::string session_file;
    std::string key_file;
    std::string proc_start;
    int listen_fd = -1;
    std::atomic<bool> stop{false};
    std::thread thread;
    std::mutex mutex;
    std::vector<std::string> log;
    std::function<void()> wake;
    bool up = false;

    void push(std::string line) {
        std::function<void()> notify;
        {
            std::lock_guard<std::mutex> lock(mutex);
            log.push_back(std::move(line));
            if (log.size() > 40) {
                log.erase(log.begin(), log.begin() + static_cast<std::ptrdiff_t>(log.size() - 40));
            }
            notify = wake;
        }
        if (notify) notify();
    }

    void handle_connection(int fd) {
        std::string bytes = read_available(fd);
        std::size_t start = 0;
        while (start < bytes.size()) {
            auto end = bytes.find('\n', start);
            if (end == std::string::npos) end = bytes.size();
            std::string line = bytes.substr(start, end - start);
            start = end + (end < bytes.size() ? 1 : 0);
            if (trim_copy(line).empty()) continue;
            json::Value frame = json::parse(line);
            std::string described = describe_frame(frame);
            if (!described.empty()) push(described);
        }
    }

    void loop() {
        while (!stop.load()) {
            pollfd pfd{listen_fd, POLLIN, 0};
            int rc = poll(&pfd, 1, 200);
            if (rc <= 0) continue;
            int conn = accept(listen_fd, nullptr, nullptr);
            if (conn < 0) continue;
            int one = 1;
            setsockopt(conn, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
            handle_connection(conn);
            close(conn);
        }
    }
};

ClaudeLink::ClaudeLink() : impl_(new Impl) {}

ClaudeLink::~ClaudeLink() {
    stop();
    delete impl_;
}

bool ClaudeLink::start(const std::string& project_root) {
    stop();
    impl_->stop = false;
    impl_->project = project_root;
    const std::string dir = "/tmp/cc-socks";
    if (!socket_dir_ok(dir)) {
        impl_->push("Claude reply inbox was not opened. Diagram edits can still be sent.");
        return false;
    }
    int pid = static_cast<int>(::getpid());
    impl_->socket_path = dir + "/" + std::to_string(pid) + ".sock";
    ::unlink(impl_->socket_path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, impl_->socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return false;
    }
    chmod(impl_->socket_path.c_str(), 0600);
    if (listen(fd, 16) != 0) {
        close(fd);
        ::unlink(impl_->socket_path.c_str());
        return false;
    }
    impl_->listen_fd = fd;
    impl_->address = "uds:" + impl_->socket_path;
    impl_->session_id = uuid_v4();
    impl_->proc_start = proc_start_of(pid);
    std::string token = random_hex(16);
    char real_buf[PATH_MAX];
    std::string real_socket = impl_->socket_path;
    if (realpath(impl_->socket_path.c_str(), real_buf)) real_socket = real_buf;
    std::string digest = sha256_hex(real_socket);
    impl_->session_file = home_dir() + "/.claude/sessions/" + std::to_string(pid) + ".json";
    impl_->key_file = home_dir() + "/.claude/sessions/" + std::to_string(pid) + "." + digest + ".key";
    json::Value session;
    session.type = json::Value::Type::Object;
    long long now = now_ms();
    session.object.emplace_back("pid", number_value(pid));
    session.object.emplace_back("sessionId", string_value(impl_->session_id));
    session.object.emplace_back("cwd", string_value(project_root));
    session.object.emplace_back("startedAt", number_value(now));
    session.object.emplace_back("procStart", string_value(impl_->proc_start));
    session.object.emplace_back("version", string_value("rewire-0.1"));
    session.object.emplace_back("peerProtocol", number_value(1));
    session.object.emplace_back("kind", string_value("interactive"));
    session.object.emplace_back("entrypoint", string_value("cli"));
    session.object.emplace_back("messagingSocketPath", string_value(impl_->socket_path));
    session.object.emplace_back("name", string_value("rewire"));
    session.object.emplace_back("nameSource", string_value("explicit"));
    session.object.emplace_back("status", string_value("idle"));
    session.object.emplace_back("updatedAt", number_value(now));
    write_text(impl_->session_file, json::dump(session));
    json::Value key;
    key.type = json::Value::Type::Object;
    key.object.emplace_back("peerToken", string_value(token));
    key.object.emplace_back("procStart", string_value(impl_->proc_start));
    write_text(impl_->key_file, json::dump(key));
    chmod(impl_->key_file.c_str(), 0600);
    impl_->up = true;
    impl_->thread = std::thread([this] { impl_->loop(); });
    return true;
}

void ClaudeLink::stop() {
    if (!impl_) return;
    impl_->stop = true;
    if (impl_->listen_fd >= 0) {
        shutdown(impl_->listen_fd, SHUT_RDWR);
        close(impl_->listen_fd);
        impl_->listen_fd = -1;
    }
    if (impl_->thread.joinable()) impl_->thread.join();
    if (!impl_->socket_path.empty()) ::unlink(impl_->socket_path.c_str());
    if (!impl_->session_file.empty()) ::unlink(impl_->session_file.c_str());
    if (!impl_->key_file.empty()) ::unlink(impl_->key_file.c_str());
    impl_->up = false;
    impl_->socket_path.clear();
    impl_->address.clear();
}

bool ClaudeLink::listening() const { return impl_ && impl_->up; }

std::string ClaudeLink::address() const { return impl_ ? impl_->address : std::string(); }

void ClaudeLink::set_wake(std::function<void()> wake) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->wake = std::move(wake);
}

SendResult ClaudeLink::send(const ClaudeSession& target, const std::string& text) {
    OutboundMessage message;
    message.socket_path = target.socket_path;
    message.session_id = target.session_id;
    message.text = text;
    message.from_address = address();
    message.from_name = "rewire";
    message.token = peer_token_for(target.pid, target.proc_start);
    return send_message(message);
}

std::vector<std::string> ClaudeLink::drain() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<std::string> out;
    out.swap(impl_->log);
    return out;
}

}  // namespace rewire
