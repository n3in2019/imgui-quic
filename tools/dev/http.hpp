#pragma once

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <csignal>
#include <chrono>
#include <sstream>

namespace dev {
inline volatile std::sig_atomic_t stopped = 0;
inline void stop(int) { stopped = 1; }
inline int listen_http(const std::string& address, int port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1)
        throw std::runtime_error("Address must be an IPv4 address");
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error("Cannot create HTTP socket");
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) || listen(fd, 16)) {
        close(fd);
        throw std::runtime_error("Cannot listen for HTTP on " + address + ":" + std::to_string(port));
    }
    return fd;
}
inline void send_all(int fd, const std::string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
        auto n = send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR && !stopped) continue;
        if (n <= 0) return;
        offset += static_cast<size_t>(n);
    }
}
inline void serve(int fd, const fs::path& root) {
    timeval timeout{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    std::string request;
    char buffer[2048];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
        if (stopped || std::chrono::steady_clock::now() >= deadline) return;
        auto n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) return;
        request.append(buffer, static_cast<size_t>(n));
    }
    std::istringstream input(request);
    std::string method, target, version;
    input >> method >> target >> version;
    std::string status = "404 Not Found", body = "Not found\n", mime = "text/plain";
    if (method != "GET" && method != "HEAD") {
        status = "405 Method Not Allowed"; body = "Use GET or HEAD\n";
    } else if (!target.empty() && target.front() == '/' && target.find('%') == std::string::npos && target.find('\\') == std::string::npos) {
        target = target.substr(0, target.find('?'));
        if (target == "/") target = "/index.html";
        std::error_code error;
        auto path = fs::canonical(root / target.substr(1), error);
        // Canonical containment also rejects symlinks outside the frontend root.
        auto relative = path.lexically_relative(root);
        if (!error && !relative.empty() && *relative.begin() != ".." && fs::is_regular_file(path, error) && !error) {
            body = read(path); status = "200 OK";
            const auto ext = path.extension().string();
            if (ext == ".html") mime = "text/html; charset=utf-8";
            else if (ext == ".js") mime = "text/javascript; charset=utf-8";
            else if (ext == ".css") mime = "text/css; charset=utf-8";
            else if (ext == ".png") mime = "image/png";
            else if (ext == ".jpg" || ext == ".jpeg") mime = "image/jpeg";
            else if (ext == ".svg") mime = "image/svg+xml";
            else if (ext == ".wasm") mime = "application/wasm";
            else mime = "application/octet-stream";
        }
    }
    send_all(fd, "HTTP/1.1 " + status + "\r\nContent-Type: " + mime +
        "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n");
    if (method != "HEAD") send_all(fd, body);
}
inline int run(int listener, const fs::path& root, const fs::path& native) {
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
    pid_t child = fork();
    if (child < 0) { close(listener); throw std::runtime_error("fork failed"); }
    if (child == 0) {
        close(listener);
        execl(native.c_str(), native.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    bool reaped = false;
    while (!stopped) {
        auto result = waitpid(child, &status, WNOHANG);
        if (result == child) { reaped = true; break; }
        pollfd event{listener, POLLIN, 0};
        if (poll(&event, 1, 100) > 0 && (event.revents & POLLIN)) {
            int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
            if (client >= 0) {
                try { serve(client, root); } catch (...) { /* A failed request must not stop the launcher. */ }
                close(client);
            }
        }
    }
    close(listener);
    if (!reaped) {
        kill(child, SIGTERM);
        for (int i = 0; i < 30; ++i) {
            if (waitpid(child, &status, WNOHANG) == child) { reaped = true; break; }
            poll(nullptr, 0, 100);
        }
        if (!reaped) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
} // namespace dev
