// Local development credentials and native process launcher; no scripting runtime.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
namespace fs = std::filesystem;
std::string read(const fs::path& p) {
    std::ifstream in(p); if (!in) throw std::runtime_error("Cannot read " + p.string());
    return {std::istreambuf_iterator<char>(in), {}};
}
void write(const fs::path& p, const std::string& text) {
    std::ofstream out(p); out << text;
    if (!out) throw std::runtime_error("Cannot write " + p.string());
}
void command(std::vector<std::string> args, const fs::path& output) {
    std::vector<char*> argv; for (auto& s : args) argv.push_back(s.data()); argv.push_back(nullptr);
    const pid_t pid = fork(); if (pid < 0) throw std::runtime_error("fork failed");
    if (pid == 0) {
        int fd = open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0) _exit(126);
        close(fd); execvp(argv[0], argv.data()); _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) throw std::runtime_error("waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status)) throw std::runtime_error("OpenSSL command failed; install openssl CLI");
}
std::string escape(const std::string& s) {
    const char* hex = "0123456789ABCDEF"; std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c=='-' || c=='_' || c=='.' || c=='~') out += char(c);
        else { out += '%'; out += hex[c>>4]; out += hex[c&15]; }
    } return out;
}
#include "http.hpp"

int main(int argc, char** argv) try {
    std::map<std::string,std::string> options{{"--example","demo"},{"--build-directory","build"},
        {"--credentials","build/webtransport_dev"},{"--page-url","http://127.0.0.1:8888/"},{"--quic-port","4433"},{"--address","127.0.0.1"},{"--port","4433"},{"--frontend","frontend"}};
    bool prepare = false, external = false;
    for (int i=1;i<argc;++i) {
        std::string key=argv[i];
        if (key=="--prepare-only") { prepare=true; continue; }
        if (key=="--help") {
            std::cout << "imgui_quic_dev [--example minimal|demo] [--build-directory build] [--credentials directory]\n"
                         "  [--address 127.0.0.1] [--port 4433] [--frontend frontend] [--prepare-only]\n  [--page-url URL (external frontend)]\n"; return 0;
        }
        if (!options.count(key) || i+1==argc) throw std::runtime_error("Invalid option: "+key);
        options[key]=argv[++i];
        if (key=="--page-url") external=true;
        if (key=="--quic-port") options["--port"]=options[key];
    }
    if (!external) options["--page-url"]="http://" + std::string(options["--address"]=="0.0.0.0" ? "127.0.0.1" : options["--address"]) + ":" + options["--port"] + "/";
    const auto& page=options["--page-url"]; auto scheme=page.find("://");
    if (scheme==std::string::npos || (page.substr(0,scheme)!="http" && page.substr(0,scheme)!="https") ||
        page.find_first_of("@#\r\n\t ")!=std::string::npos) throw std::runtime_error("Invalid page URL");
    auto end=page.find_first_of("/?",scheme+3);
    std::string origin=page.substr(0,end);
    if (origin.size()==scheme+3) throw std::runtime_error("Empty page host");
    size_t parsed=0; int port=std::stoi(options["--port"],&parsed);
    if (parsed!=options["--port"].size() || port<1 || port>65535) throw std::runtime_error("Invalid QUIC port");
    if (options["--example"]!="demo" && options["--example"]!="minimal") throw std::runtime_error("Invalid example");
    auto native=fs::absolute(fs::path(options["--build-directory"])/"examples"/
        (options["--example"]=="minimal"?"example_minimal":"example_core_cpp_draw"));
    if (!prepare && !fs::is_regular_file(native)) throw std::runtime_error("Build examples first: "+native.string());
    umask(0077);
    auto dir=fs::absolute(options["--credentials"]); fs::create_directories(dir);
    int lock=open((dir/".lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
    if (lock<0 || flock(lock,LOCK_EX)<0) throw std::runtime_error("Cannot lock credentials");
    auto cert=dir/"cert.pem", key=dir/"key.pem", token=dir/"token";
    int count=int(fs::exists(cert))+int(fs::exists(key))+int(fs::exists(token));
    if (count && count!=3) throw std::runtime_error("Incomplete credentials; restore or use a new directory");
    auto output=dir/".openssl-output";
    if (!count) {
        // Generate in a private staging directory; never overwrite existing credentials.
        std::string pattern=(dir/".generate-XXXXXX").string();
        if (!mkdtemp(pattern.data())) throw std::runtime_error("Cannot create credential staging directory");
        fs::path temp=pattern;
        try {
            command({"openssl","req","-x509","-newkey","ec","-pkeyopt","ec_paramgen_curve:P-256",
                "-nodes","-sha256","-days","13","-subj","/CN=localhost","-addext","subjectAltName=DNS:localhost,IP:127.0.0.1",
                "-keyout",(temp/"key.pem").string(),"-out",(temp/"cert.pem").string()},output);
            command({"openssl","rand","-hex","32"},temp/"token");
            for (const char* name : {"key.pem","cert.pem","token"}) fs::rename(temp/name,dir/name);
            fs::remove(temp);
        } catch (...) { fs::remove_all(temp); throw; }
    }
    command({"openssl","x509","-in",cert.string(),"-noout","-fingerprint","-sha256"},output);
    auto fingerprint=read(output); fs::remove(output);
    auto equal=fingerprint.find('='); if (equal==std::string::npos) throw std::runtime_error("Invalid fingerprint");
    std::string pin;
    for (unsigned char c : fingerprint.substr(equal+1)) if (std::isxdigit(c)) pin+=char(std::tolower(c));
    if (pin.size()!=64) throw std::runtime_error("Invalid SHA256 fingerprint");
    auto secret=read(token); while (!secret.empty() && std::isspace(static_cast<unsigned char>(secret.back()))) secret.pop_back();
    if (secret.size()<32) throw std::runtime_error("Token must contain at least 32 bytes");
    const std::string endpoint="https://" + std::string(options["--address"]=="0.0.0.0" ? "127.0.0.1" : options["--address"]) + ":"+std::to_string(port)+"/wt";
    write(dir/"url.txt",page+"#wt-url="+escape(endpoint)+"&wt-token="+escape(secret)+"&wt-cert="+pin+"\n");
    std::cout << "Open the connection URL in " << (dir/"url.txt") << ".\n" << std::flush;
    if (prepare) return 0;
    for (const auto& value : std::map<std::string,std::string>{{"IMGUI_QUIC_CERT",cert.string()},
        {"IMGUI_QUIC_KEY",key.string()},{"IMGUI_QUIC_TOKEN_FILE",token.string()},
        {"IMGUI_QUIC_HOST",options["--address"]},{"IMGUI_QUIC_ORIGINS",origin},{"IMGUI_QUIC_PORT",std::to_string(port)}})
        if (setenv(value.first.c_str(),value.second.c_str(),1)) throw std::runtime_error("setenv failed");
    close(lock);
    if (!external) {
        auto root=fs::canonical(options["--frontend"]);
        if (!fs::is_regular_file(root/"index.html")) throw std::runtime_error("Frontend index.html not found");
        int listener=dev::listen_http(options["--address"],port);
        std::cout << "Frontend: " << page << " (TCP); WebTransport: UDP " << port << "\n" << std::flush;
        return dev::run(listener,root,native);
    }
    execl(native.c_str(),native.c_str(),static_cast<char*>(nullptr));
    throw std::runtime_error("Cannot start native example");
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
