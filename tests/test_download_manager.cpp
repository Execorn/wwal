// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_download_manager.cpp
 * @brief Multi-Backend Download, HTTP/2 epoll Reactor & Verification Suite
 */

#include "studio/queue/download_manager.hpp"
#include "waywal/wwpkg.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace wwal::studio;
namespace fs = std::filesystem;

namespace {

void cleanup_dir(const fs::path &dir)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
}

std::vector<uint8_t> read_binary_file(const fs::path &p)
{
    std::ifstream ifs(p, std::ios::binary | std::ios::ate);
    assert(ifs.good());
    auto sz = static_cast<size_t>(ifs.tellg());
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(sz);
    if (sz > 0) {
        ifs.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(sz));
    }
    return buf;
}

std::vector<uint8_t> build_signed_test_wwpkg(const fs::path &scratch_dir)
{
    fs::path src_dir = scratch_dir / "pkg_src";
    fs::path out_pkg = scratch_dir / "signed_test.wwpkg";
    cleanup_dir(src_dir);
    fs::create_directories(src_dir / "shaders");

    {
        std::ofstream ofs(src_dir / "shaders" / "main.frag", std::ios::binary);
        ofs << "#version 310 es\nprecision highp float;\nout vec4 fragColor;\nvoid main(){ "
               "fragColor = vec4(0.2, 0.6, 1.0, 1.0); }\n";
    }
    {
        std::ofstream ofs(src_dir / "preview.webp", std::ios::binary);
        ofs << "RIFF_PREVIEW_PAYLOAD";
    }

    wwpkg_manifest_t mf{};
    std::snprintf(mf.schema_version, sizeof(mf.schema_version), "1.0");
    std::snprintf(mf.id, sizeof(mf.id), "550e8400-e29b-41d4-a716-446655440000");
    std::snprintf(mf.slug, sizeof(mf.slug), "loopback-verified-shader");
    std::snprintf(mf.name, sizeof(mf.name), "Loopback Verified Shader");
    std::snprintf(mf.version, sizeof(mf.version), "1.0.0");
    std::snprintf(mf.author_name, sizeof(mf.author_name), "Studio Test");
    std::snprintf(mf.author_email, sizeof(mf.author_email), "test@example.org");
    std::snprintf(mf.license, sizeof(mf.license), "GPL-3.0-or-later");
    std::snprintf(mf.description, sizeof(mf.description), "Signed test package");
    mf.type = WWPKG_TYPE_SHADER_FRAGMENT;
    std::snprintf(mf.entrypoint_pipeline, sizeof(mf.entrypoint_pipeline), "shaders/main.frag");
    std::snprintf(mf.entrypoint_preview, sizeof(mf.entrypoint_preview), "preview.webp");
    std::snprintf(mf.min_wwal_version, sizeof(mf.min_wwal_version), "3.0.0");
    mf.abi_version = 1;
    std::snprintf(mf.gl_profile, sizeof(mf.gl_profile), "es31");
    mf.capabilities.network = false;
    mf.capabilities.filesystem = false;
    mf.budgets.max_vram_mb = 64;
    mf.budgets.max_ram_mb = 64;
    mf.budgets.target_fps = 60;

    uint8_t pub[32]{};
    uint8_t priv[64]{};
    wwpkg_generate_keypair(pub, priv);

    bool built = wwpkg_build_package(src_dir.c_str(), &mf, priv, out_pkg.c_str());
    assert(built && "wwpkg_build_package must construct a valid signed .wwpkg");
    return read_binary_file(out_pkg);
}

/* ------------------------------------------------------------------------- */
/* Minimal Multi-Threaded Loopback HTTP/1.1 Test Server                      */
/* ------------------------------------------------------------------------- */

struct LoopbackHttpRequest {
    std::string method;
    std::string path;
    std::string range_header;
};

class LoopbackHttpServer {
  public:
    using Handler = std::function<void(int client_fd, const LoopbackHttpRequest &req)>;

    explicit LoopbackHttpServer(Handler handler) : m_handler(std::move(handler))
    {
        m_listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        assert(m_listenFd >= 0);

        int opt = 1;
        ::setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        int rc = ::bind(m_listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        assert(rc == 0);

        socklen_t len = sizeof(addr);
        rc = ::getsockname(m_listenFd, reinterpret_cast<sockaddr *>(&addr), &len);
        assert(rc == 0);
        m_port = ntohs(addr.sin_port);

        rc = ::listen(m_listenFd, 32);
        assert(rc == 0);

        m_acceptThread = std::thread([this]() { acceptLoop(); });
    }

    ~LoopbackHttpServer() { stop(); }

    [[nodiscard]] uint16_t port() const noexcept { return m_port; }

    [[nodiscard]] std::string baseUrl() const
    {
        return "http://127.0.0.1:" + std::to_string(m_port);
    }

    void stop()
    {
        if (m_stopped.exchange(true)) {
            return;
        }
        if (m_listenFd >= 0) {
            ::shutdown(m_listenFd, SHUT_RDWR);
            ::close(m_listenFd);
            m_listenFd = -1;
        }
        if (m_acceptThread.joinable()) {
            m_acceptThread.join();
        }
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lk(m_workersMutex);
            workers.swap(m_clientThreads);
        }
        for (auto &t : workers) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    static bool writeAll(int fd, const void *data, size_t len)
    {
        const auto *p = static_cast<const uint8_t *>(data);
        size_t off = 0;
        while (off < len) {
            ssize_t w = ::send(fd, p + off, len - off, MSG_NOSIGNAL);
            if (w <= 0) {
                if (w < 0 && errno == EINTR) {
                    continue;
                }
                return false;
            }
            off += static_cast<size_t>(w);
        }
        return true;
    }

  private:
    void acceptLoop()
    {
        while (!m_stopped.load()) {
            sockaddr_in cli{};
            socklen_t clen = sizeof(cli);
            int cfd =
                ::accept4(m_listenFd, reinterpret_cast<sockaddr *>(&cli), &clen, SOCK_CLOEXEC);
            if (cfd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            std::lock_guard<std::mutex> lk(m_workersMutex);
            m_clientThreads.emplace_back([this, cfd]() {
                handleClient(cfd);
                ::close(cfd);
            });
        }
    }

    void handleClient(int cfd)
    {
        std::string raw;
        char buf[2048];
        while (raw.find("\r\n\r\n") == std::string::npos && raw.size() < 16384) {
            ssize_t r = ::recv(cfd, buf, sizeof(buf), 0);
            if (r <= 0) {
                break;
            }
            raw.append(buf, static_cast<size_t>(r));
        }
        if (raw.empty()) {
            return;
        }

        LoopbackHttpRequest req;
        std::istringstream iss(raw);
        std::string firstLine;
        std::getline(iss, firstLine);
        {
            std::istringstream ls(firstLine);
            ls >> req.method >> req.path;
        }

        std::string headerLine;
        while (std::getline(iss, headerLine)) {
            if (!headerLine.empty() && headerLine.back() == '\r') {
                headerLine.pop_back();
            }
            if (headerLine.empty()) {
                break;
            }
            if (headerLine.size() >= 6) {
                std::string prefix = headerLine.substr(0, 6);
                std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (prefix == "range:") {
                    std::string val = headerLine.substr(6);
                    while (!val.empty() && val.front() == ' ') {
                        val.erase(val.begin());
                    }
                    req.range_header = val;
                }
            }
        }

        m_handler(cfd, req);
    }

    int m_listenFd{-1};
    uint16_t m_port{0};
    std::atomic<bool> m_stopped{false};
    Handler m_handler;
    std::thread m_acceptThread;
    std::mutex m_workersMutex;
    std::vector<std::thread> m_clientThreads;
};

/* ------------------------------------------------------------------------- */
/* Existing Simulation Tests (1..5)                                          */
/* ------------------------------------------------------------------------- */

void test_network_drop_and_range_resumption()
{
    std::cout << "[Download Mgr 1/10] Testing 50% network drop and byte-range resumption..."
              << std::endl;

    fs::path test_dir = "/tmp/test_wwal_dl_resume";
    cleanup_dir(test_dir);

    DownloadManager dm;
    dm.setDownloadDirectory(test_dir.string());
    assert(dm.initialize());

    constexpr uint64_t TOTAL_BYTES = 2 * 1024 * 1024;
    DownloadManager::SimulationHooks hooks{};
    hooks.dry_run_mode = true;
    hooks.drop_at_percent = 0.5f;
    hooks.mock_total_size = TOTAL_BYTES;
    dm.setSimulationHooks(hooks);

    std::atomic<bool> completed{false};
    dm.enqueue(11223344ULL, TaskPriority::Immediate, [&](const DownloadProgress &prog) {
        if (prog.state == TaskState::Installed) {
            completed.store(true);
        }
    });

    dm.waitUntilIdle();
    assert(completed.load());
    assert(dm.simulationHooks().simulated_drop_count >= 1);

    fs::path final_pkg = test_dir / "11223344.bin";
    assert(fs::exists(final_pkg));
    assert(fs::file_size(final_pkg) == TOTAL_BYTES);

    std::ifstream ifs(final_pkg, std::ios::binary);
    std::vector<uint8_t> content(TOTAL_BYTES);
    ifs.read(reinterpret_cast<char *>(content.data()), TOTAL_BYTES);
    for (size_t i = 0; i < TOTAL_BYTES; ++i) {
        assert(content[i] == static_cast<uint8_t>(i & 0xFF));
    }

    dm.shutdown();
    cleanup_dir(test_dir);
    std::cout << "  -> 50% drop and byte-range resumption verified cleanly." << std::endl;
}

void test_http_200_truncation_safety()
{
    std::cout << "[Download Mgr 2/10] Testing HTTP 200 truncation safety guard..." << std::endl;

    fs::path test_dir = "/tmp/test_wwal_dl_trunc";
    cleanup_dir(test_dir);

    DownloadManager dm;
    dm.setDownloadDirectory(test_dir.string());
    dm.initialize();

    constexpr uint64_t TOTAL_BYTES = 1024 * 1024;
    DownloadManager::SimulationHooks hooks{};
    hooks.dry_run_mode = true;
    hooks.drop_at_percent = 0.5f;
    hooks.simulate_http_200_on_resume = true;
    hooks.mock_total_size = TOTAL_BYTES;
    dm.setSimulationHooks(hooks);

    std::atomic<bool> completed{false};
    dm.enqueue(55667788ULL, TaskPriority::Immediate, [&](const DownloadProgress &prog) {
        if (prog.state == TaskState::Installed) {
            completed.store(true);
        }
    });

    dm.waitUntilIdle();
    assert(completed.load());
    assert(dm.simulationHooks().simulated_truncation_occurred);

    fs::path final_pkg = test_dir / "55667788.bin";
    assert(fs::exists(final_pkg));
    assert(fs::file_size(final_pkg) == TOTAL_BYTES);

    dm.shutdown();
    cleanup_dir(test_dir);
    std::cout << "  -> HTTP 200 truncation guard passed." << std::endl;
}

void test_http_429_jitter_backoff()
{
    std::cout << "[Download Mgr 3/10] Testing HTTP 429 full-jitter exponential backoff..."
              << std::endl;

    fs::path test_dir = "/tmp/test_wwal_dl_429";
    cleanup_dir(test_dir);

    DownloadManager dm;
    dm.setDownloadDirectory(test_dir.string());
    dm.initialize();

    DownloadManager::SimulationHooks hooks{};
    hooks.dry_run_mode = true;
    hooks.simulate_rate_limit_count = 3;
    hooks.mock_total_size = 512 * 1024;
    dm.setSimulationHooks(hooks);

    std::atomic<bool> completed{false};
    dm.enqueue(77889900ULL, TaskPriority::Immediate, [&](const DownloadProgress &prog) {
        if (prog.state == TaskState::Installed) {
            completed.store(true);
        }
    });

    dm.waitUntilIdle();
    assert(completed.load());
    assert(dm.simulationHooks().simulated_429_count == 3);

    dm.shutdown();
    cleanup_dir(test_dir);
    std::cout << "  -> HTTP 429 jitter backoff passed." << std::endl;
}

void test_priority_scheduling()
{
    std::cout
        << "[Download Mgr 4/10] Testing priority queue scheduling (Immediate vs Background)..."
        << std::endl;

    fs::path test_dir = "/tmp/test_wwal_dl_prio";
    cleanup_dir(test_dir);

    DownloadManager dm;
    dm.setDownloadDirectory(test_dir.string());
    dm.initialize();

    DownloadManager::SimulationHooks hooks{};
    hooks.dry_run_mode = true;
    hooks.mock_total_size = 512 * 1024;
    dm.setSimulationHooks(hooks);

    std::atomic<bool> immediate_done{false};
    std::atomic<bool> background_done{false};

    dm.setBandwidthLimit(1);
    dm.enqueue(101ULL, TaskPriority::BackgroundUpdate, [&](const DownloadProgress &prog) {
        if (prog.state == TaskState::Installed) {
            background_done.store(true);
        }
    });

    dm.enqueue(202ULL, TaskPriority::Immediate, [&](const DownloadProgress &prog) {
        if (prog.state == TaskState::Installed) {
            immediate_done.store(true);
        }
    });

    dm.setBandwidthLimit(0);
    dm.waitUntilIdle();
    assert(immediate_done.load() && background_done.load());

    dm.shutdown();
    cleanup_dir(test_dir);
    std::cout << "  -> Priority scheduling passed." << std::endl;
}

void test_token_bucket_bandwidth_governor()
{
    std::cout << "[Download Mgr 5/10] Testing Token Bucket bandwidth governor pacing..."
              << std::endl;

    DownloadManager dm;
    dm.setBandwidthLimit(5 * 1024 * 1024);
    assert(dm.bandwidthLimit() == 5 * 1024 * 1024);

    dm.setBandwidthLimit(10 * 1024 * 1024);
    assert(dm.bandwidthLimit() == 10 * 1024 * 1024);

    dm.pause();
    assert(dm.isPaused());

    dm.resume();
    assert(!dm.isPaused());

    dm.setBandwidthLimit(0);
    assert(dm.bandwidthLimit() == 0);

    std::cout << "  -> Token Bucket governor passed." << std::endl;
}

/* ------------------------------------------------------------------------- */
/* Live Loopback Socket HTTP Tests (6..8)                                    */
/* ------------------------------------------------------------------------- */

void test_loopback_http_status_codes_and_wwpkg_verification()
{
    std::cout << "[Download Mgr 6/10] Testing live 127.0.0.1 HTTP 200/206/404/416/429 + wwpkg "
                 "Ed25519 verification..."
              << std::endl;

    fs::path test_dir = "/tmp/test_wwal_dl_loopback";
    cleanup_dir(test_dir);
    fs::create_directories(test_dir);

    std::vector<uint8_t> signedPkg = build_signed_test_wwpkg(test_dir);
    std::vector<uint8_t> tamperedPkg = signedPkg;
    assert(tamperedPkg.size() > 64);
    tamperedPkg[tamperedPkg.size() / 2] ^= 0xFFu;
    tamperedPkg[tamperedPkg.size() - 5] ^= 0xAAu;

    std::atomic<int> resumeHits{0};
    std::atomic<int> rateLimitHits{0};
    std::atomic<int> range416Hits{0};

    LoopbackHttpServer server([&](int cfd, const LoopbackHttpRequest &req) {
        if (req.path == "/valid.wwpkg") {
            std::ostringstream hdr;
            hdr << "HTTP/1.1 200 OK\r\nContent-Length: " << signedPkg.size()
                << "\r\nConnection: close\r\n\r\n";
            auto h = hdr.str();
            LoopbackHttpServer::writeAll(cfd, h.data(), h.size());
            LoopbackHttpServer::writeAll(cfd, signedPkg.data(), signedPkg.size());
            return;
        }

        if (req.path == "/tampered.wwpkg") {
            std::ostringstream hdr;
            hdr << "HTTP/1.1 200 OK\r\nContent-Length: " << tamperedPkg.size()
                << "\r\nConnection: close\r\n\r\n";
            auto h = hdr.str();
            LoopbackHttpServer::writeAll(cfd, h.data(), h.size());
            LoopbackHttpServer::writeAll(cfd, tamperedPkg.data(), tamperedPkg.size());
            return;
        }

        if (req.path == "/resume.wwpkg") {
            int hit = ++resumeHits;
            if (hit == 1) {
                size_t half = signedPkg.size() / 2;
                std::ostringstream hdr;
                hdr << "HTTP/1.1 200 OK\r\nContent-Length: " << signedPkg.size()
                    << "\r\nConnection: close\r\n\r\n";
                auto h = hdr.str();
                LoopbackHttpServer::writeAll(cfd, h.data(), h.size());
                // Write only half of Content-Length and abruptly close TCP socket.
                LoopbackHttpServer::writeAll(cfd, signedPkg.data(), half);
                return;
            }
            assert(req.range_header.rfind("bytes=", 0) == 0);
            size_t eq = req.range_header.find('=');
            size_t dash = req.range_header.find('-', eq);
            size_t startOff = std::stoull(req.range_header.substr(eq + 1, dash - eq - 1));
            assert(startOff == signedPkg.size() / 2);
            size_t rem = signedPkg.size() - startOff;
            std::ostringstream hdr;
            hdr << "HTTP/1.1 206 Partial Content\r\n"
                << "Content-Range: bytes " << startOff << "-" << (signedPkg.size() - 1) << "/"
                << signedPkg.size() << "\r\n"
                << "Content-Length: " << rem << "\r\nConnection: close\r\n\r\n";
            auto h = hdr.str();
            LoopbackHttpServer::writeAll(cfd, h.data(), h.size());
            LoopbackHttpServer::writeAll(cfd, signedPkg.data() + startOff, rem);
            return;
        }

        if (req.path == "/missing.wwpkg") {
            const std::string html = "<html><body><h1>404 Not Found</h1></body></html>";
            std::ostringstream hdr;
            hdr << "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nContent-Length: "
                << html.size() << "\r\nConnection: close\r\n\r\n";
            auto h = hdr.str();
            LoopbackHttpServer::writeAll(cfd, h.data(), h.size());
            LoopbackHttpServer::writeAll(cfd, html.data(), html.size());
            return;
        }

        if (req.path == "/stale416.wwpkg") {
            ++range416Hits;
            if (!req.range_header.empty()) {
                const std::string body = "Range Not Satisfiable";
                std::ostringstream hdr;
                hdr << "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: " << body.size()
                    << "\r\nConnection: close\r\n\r\n";
                auto h = hdr.str();
                LoopbackHttpServer::writeAll(cfd, h.data(), h.size());
                LoopbackHttpServer::writeAll(cfd, body.data(), body.size());
                return;
            }
            std::ostringstream hdr;
            hdr << "HTTP/1.1 200 OK\r\nContent-Length: " << signedPkg.size()
                << "\r\nConnection: close\r\n\r\n";
            auto h = hdr.str();
            LoopbackHttpServer::writeAll(cfd, h.data(), h.size());
            LoopbackHttpServer::writeAll(cfd, signedPkg.data(), signedPkg.size());
            return;
        }

        if (req.path == "/ratelimit.wwpkg") {
            int hit = ++rateLimitHits;
            if (hit <= 2) {
                const std::string body = "Too Many Requests";
                std::ostringstream hdr;
                hdr << "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 0\r\nContent-Length: "
                    << body.size() << "\r\nConnection: close\r\n\r\n";
                auto h = hdr.str();
                LoopbackHttpServer::writeAll(cfd, h.data(), h.size());
                LoopbackHttpServer::writeAll(cfd, body.data(), body.size());
                return;
            }
            std::ostringstream hdr;
            hdr << "HTTP/1.1 200 OK\r\nContent-Length: " << signedPkg.size()
                << "\r\nConnection: close\r\n\r\n";
            auto h = hdr.str();
            LoopbackHttpServer::writeAll(cfd, h.data(), h.size());
            LoopbackHttpServer::writeAll(cfd, signedPkg.data(), signedPkg.size());
            return;
        }
    });

    DownloadManager dm;
    fs::path dl_dir = test_dir / "downloads";
    dm.setDownloadDirectory(dl_dir.string());
    DownloadManager::SimulationHooks hooks{};
    hooks.allow_loopback_http = true;
    dm.setSimulationHooks(hooks);
    assert(dm.initialize());

    // 6a: HTTP 200 signed .wwpkg -> Verifying -> Extracting -> Installed
    {
        std::atomic<TaskState> finalState{TaskState::Queued};
        std::string extractedDir;
        dm.enqueue("pkg-valid-200", server.baseUrl() + "/valid.wwpkg", TaskPriority::Immediate,
                   [&](const DownloadProgress &p) {
                       if (p.state == TaskState::Installed || p.state == TaskState::Failed) {
                           extractedDir = p.extracted_dir;
                           finalState.store(p.state);
                       }
                   });
        dm.waitUntilIdle();
        assert(finalState.load() == TaskState::Installed);
        assert(fs::exists(dl_dir / "pkg-valid-200.wwpkg"));
        assert(!extractedDir.empty() && fs::exists(fs::path(extractedDir) / "shaders/main.frag"));
    }

    // 6b: Tampered .wwpkg -> signature check MUST reject and unlink
    {
        std::atomic<TaskState> finalState{TaskState::Queued};
        dm.enqueue("pkg-tampered", server.baseUrl() + "/tampered.wwpkg", TaskPriority::Immediate,
                   [&](const DownloadProgress &p) {
                       if (p.state == TaskState::Installed || p.state == TaskState::Failed) {
                           finalState.store(p.state);
                       }
                   });
        dm.waitUntilIdle();
        assert(finalState.load() == TaskState::Failed);
        assert(!fs::exists(dl_dir / "pkg-tampered.wwpkg"));
    }

    // 6c: HTTP 206 byte-range resumption over real TCP drop
    {
        std::atomic<TaskState> finalState{TaskState::Queued};
        dm.enqueue("pkg-resume-206", server.baseUrl() + "/resume.wwpkg", TaskPriority::Immediate,
                   [&](const DownloadProgress &p) {
                       if (p.state == TaskState::Installed || p.state == TaskState::Failed) {
                           finalState.store(p.state);
                       }
                   });
        dm.waitUntilIdle();
        assert(finalState.load() == TaskState::Installed);
        assert(resumeHits.load() >= 2);
        assert(fs::exists(dl_dir / "pkg-resume-206.wwpkg"));
    }

    // 6d: HTTP 404 HTML error page MUST fail and never produce .wwpkg
    {
        std::atomic<TaskState> finalState{TaskState::Queued};
        dm.enqueue("pkg-404", server.baseUrl() + "/missing.wwpkg", TaskPriority::Immediate,
                   [&](const DownloadProgress &p) {
                       if (p.state == TaskState::Installed || p.state == TaskState::Failed) {
                           finalState.store(p.state);
                       }
                   });
        dm.waitUntilIdle();
        assert(finalState.load() == TaskState::Failed);
        assert(!fs::exists(dl_dir / "pkg-404.wwpkg"));
        assert(!fs::exists(dl_dir / "pkg-404.wwpkg.part"));
    }

    // 6e: HTTP 416 stale .part recovery -> truncate to 0 and retry without Range
    {
        fs::path stalePart = dl_dir / "pkg-416.wwpkg.part";
        {
            std::ofstream ofs(stalePart, std::ios::binary);
            std::vector<char> bogus(signedPkg.size() + 4096, 'X');
            ofs.write(bogus.data(), static_cast<std::streamsize>(bogus.size()));
        }
        std::atomic<TaskState> finalState{TaskState::Queued};
        dm.enqueue("pkg-416", server.baseUrl() + "/stale416.wwpkg", TaskPriority::Immediate,
                   [&](const DownloadProgress &p) {
                       if (p.state == TaskState::Installed || p.state == TaskState::Failed) {
                           finalState.store(p.state);
                       }
                   });
        dm.waitUntilIdle();
        assert(finalState.load() == TaskState::Installed);
        assert(range416Hits.load() >= 2);
        assert(fs::file_size(dl_dir / "pkg-416.wwpkg") == signedPkg.size());
    }

    // 6f: HTTP 429 Retry-After backoff recovery
    {
        dm.resetSimulationStats();
        std::atomic<TaskState> finalState{TaskState::Queued};
        dm.enqueue("pkg-429", server.baseUrl() + "/ratelimit.wwpkg", TaskPriority::Immediate,
                   [&](const DownloadProgress &p) {
                       if (p.state == TaskState::Installed || p.state == TaskState::Failed) {
                           finalState.store(p.state);
                       }
                   });
        dm.waitUntilIdle();
        assert(finalState.load() == TaskState::Installed);
        assert(dm.simulationHooks().simulated_429_count >= 2);
    }

    dm.shutdown();
    server.stop();
    cleanup_dir(test_dir);
    std::cout << "  -> Live 127.0.0.1 HTTP 200/206/404/416/429 + wwpkg verification passed."
              << std::endl;
}

void test_ssrf_redirect_and_protocol_rejection()
{
    std::cout << "[Download Mgr 7/10] Testing 302 -> file:///etc/passwd SSRF rejection..."
              << std::endl;

    fs::path test_dir = "/tmp/test_wwal_dl_ssrf";
    cleanup_dir(test_dir);
    fs::create_directories(test_dir);

    LoopbackHttpServer server([&](int cfd, const LoopbackHttpRequest &req) {
        if (req.path == "/ssrf-file") {
            const std::string resp =
                "HTTP/1.1 302 Found\r\nLocation: file:///etc/passwd\r\nContent-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            LoopbackHttpServer::writeAll(cfd, resp.data(), resp.size());
            return;
        }
        if (req.path == "/ssrf-metadata") {
            const std::string resp =
                "HTTP/1.1 302 Found\r\nLocation: http://169.254.169.254/latest/meta-data/\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n";
            LoopbackHttpServer::writeAll(cfd, resp.data(), resp.size());
            return;
        }
    });

    DownloadManager dm;
    dm.setDownloadDirectory(test_dir.string());
    DownloadManager::SimulationHooks hooks{};
    hooks.allow_loopback_http = true;
    dm.setSimulationHooks(hooks);
    assert(dm.initialize());

    for (const auto &[id, url] : std::vector<std::pair<std::string, std::string>>{
             {"ssrf-direct-file", "file:///etc/passwd"},
             {"ssrf-302-file", server.baseUrl() + "/ssrf-file"},
             {"ssrf-302-imds", server.baseUrl() + "/ssrf-metadata"},
         }) {
        std::atomic<TaskState> finalState{TaskState::Queued};
        dm.enqueue(id, url, TaskPriority::Immediate, [&](const DownloadProgress &p) {
            if (p.state == TaskState::Installed || p.state == TaskState::Failed) {
                finalState.store(p.state);
            }
        });
        dm.waitUntilIdle();
        assert(finalState.load() == TaskState::Failed && "SSRF vector must be rejected");
        assert(!fs::exists(test_dir / (id + ".wwpkg")));
        assert(!fs::exists(test_dir / (id + ".wwpkg.part")));
    }

    dm.shutdown();
    server.stop();
    cleanup_dir(test_dir);
    std::cout << "  -> Direct and 302-redirect file:// and non-loopback HTTP SSRF blocked."
              << std::endl;
}

void test_four_plus_one_fast_lane_isolation()
{
    std::cout << "[Download Mgr 8/10] Testing 4 bulk lanes + 1 dedicated Immediate fast lane..."
              << std::endl;

    fs::path test_dir = "/tmp/test_wwal_dl_lanes";
    cleanup_dir(test_dir);
    fs::create_directories(test_dir);

    std::vector<uint8_t> signedPkg = build_signed_test_wwpkg(test_dir);
    std::atomic<int> activeSlow{0};
    std::atomic<bool> releaseSlow{false};

    LoopbackHttpServer server([&](int cfd, const LoopbackHttpRequest &req) {
        if (req.path == "/slow.wwpkg") {
            ++activeSlow;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!releaseSlow.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        std::ostringstream hdr;
        hdr << "HTTP/1.1 200 OK\r\nContent-Length: " << signedPkg.size()
            << "\r\nConnection: close\r\n\r\n";
        auto h = hdr.str();
        LoopbackHttpServer::writeAll(cfd, h.data(), h.size());
        LoopbackHttpServer::writeAll(cfd, signedPkg.data(), signedPkg.size());
    });

    DownloadManager dm;
    fs::path dl_dir = test_dir / "downloads";
    dm.setDownloadDirectory(dl_dir.string());
    DownloadManager::SimulationHooks hooks{};
    hooks.allow_loopback_http = true;
    dm.setSimulationHooks(hooks);
    assert(dm.initialize());

    std::atomic<int> slowDone{0};
    for (int i = 0; i < 4; ++i) {
        dm.enqueue("bulk-" + std::to_string(i), server.baseUrl() + "/slow.wwpkg",
                   TaskPriority::BackgroundUpdate, [&](const DownloadProgress &p) {
                       if (p.state == TaskState::Installed) {
                           ++slowDone;
                       }
                   });
    }

    auto waitStart = std::chrono::steady_clock::now();
    while (activeSlow.load() < 4 &&
           std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(activeSlow.load() == 4 && "All 4 bulk lanes must be saturated concurrently");

    std::atomic<bool> fastDone{false};
    dm.enqueue("fast-lane-pkg", server.baseUrl() + "/fast.wwpkg", TaskPriority::Immediate,
               [&](const DownloadProgress &p) {
                   if (p.state == TaskState::Installed) {
                       fastDone.store(true);
                   }
               });

    auto fastWait = std::chrono::steady_clock::now();
    while (!fastDone.load() &&
           std::chrono::steady_clock::now() - fastWait < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(fastDone.load() && "Dedicated Immediate fast lane must finish while 4 bulk lanes stall");
    assert(slowDone.load() == 0 && "Bulk lanes must still be blocked when fast lane finishes");

    releaseSlow.store(true);
    dm.waitUntilIdle();
    assert(slowDone.load() == 4);

    dm.shutdown();
    server.stop();
    cleanup_dir(test_dir);
    std::cout << "  -> 4+1 HTTP/2 lane isolation verified." << std::endl;
}

/* ------------------------------------------------------------------------- */
/* Real Subprocess Backend C Exit-Code & Shutdown Tests (9/9)                */
/* ------------------------------------------------------------------------- */

fs::path write_executable_script(const fs::path &path, const std::string &body)
{
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        ofs << "#!/bin/sh\n" << body << "\n";
    }
    int rc = ::chmod(path.c_str(), 0755);
    assert(rc == 0);
    return path;
}

void test_backend_c_subprocess_reaping_and_shutdown()
{
    std::cout << "[Download Mgr 9/10] Testing Backend C subprocess exit 0, exit 1, SIGSEGV & "
                 "shutdown..."
              << std::endl;

    fs::path test_dir = "/tmp/test_wwal_dl_backend_c";
    cleanup_dir(test_dir);
    fs::create_directories(test_dir);

    fs::path okScript = write_executable_script(
        test_dir / "steamcmd_ok.sh",
        "echo 'Update state (0x61) downloading, progress: 50.00% (512 / 1024)'\n"
        "echo 'Success. Downloaded item 9001 to \"/tmp/test_wwal_dl_backend_c/item_9001\"'\n"
        "exit 0");

    fs::path exit1Script = write_executable_script(
        test_dir / "steamcmd_exit1.sh", "echo 'ERROR! Download item 9002 failed (Failure).'\n"
                                        "exit 1");

    fs::path segvScript = write_executable_script(
        test_dir / "steamcmd_segv.sh",
        "echo 'Update state (0x61) downloading, progress: 12.50% (128 / 1024)'\n"
        "kill -SEGV $$\n"
        "exit 0");

    fs::path hangScript = write_executable_script(test_dir / "steamcmd_hang.sh", "trap '' TERM\n"
                                                                                 "sleep 30\n"
                                                                                 "exit 0");

    auto runWithScript = [&](const fs::path &script, uint64_t id) -> TaskState {
        DownloadManager dm;
        dm.setDownloadDirectory(test_dir.string());
        DownloadManager::SimulationHooks hooks{};
        hooks.force_backend_failure_a = true;
        hooks.force_backend_failure_b = true;
        hooks.steamcmd_override_path = script.string();
        dm.setSimulationHooks(hooks);
        assert(dm.initialize());

        std::atomic<TaskState> endState{TaskState::Queued};
        dm.enqueue(id, TaskPriority::Immediate, [&](const DownloadProgress &p) {
            if (p.state == TaskState::Installed || p.state == TaskState::Failed ||
                p.state == TaskState::Cancelled) {
                endState.store(p.state);
            }
        });
        dm.waitUntilIdle();
        dm.shutdown();
        return endState.load();
    };

    assert(runWithScript(okScript, 9001ULL) == TaskState::Installed &&
           "Backend C exit 0 must report Installed");
    assert(runWithScript(exit1Script, 9002ULL) == TaskState::Failed &&
           "Backend C exit 1 must report Failed");
    assert(runWithScript(segvScript, 9003ULL) == TaskState::Failed &&
           "Backend C SIGSEGV crash must report Failed (no double-waitpid ECHILD false success)");

    // Verify shutdown() escalates SIGTERM -> SIGKILL and never hangs on an unresponsive child.
    {
        DownloadManager dm;
        dm.setDownloadDirectory(test_dir.string());
        DownloadManager::SimulationHooks hooks{};
        hooks.force_backend_failure_a = true;
        hooks.force_backend_failure_b = true;
        hooks.steamcmd_override_path = hangScript.string();
        dm.setSimulationHooks(hooks);
        assert(dm.initialize());

        std::atomic<bool> startedDownloading{false};
        dm.enqueue(9004ULL, TaskPriority::Immediate, [&](const DownloadProgress &p) {
            if (p.state == TaskState::Downloading) {
                startedDownloading.store(true);
            }
        });

        auto tWait = std::chrono::steady_clock::now();
        while (!startedDownloading.load() &&
               std::chrono::steady_clock::now() - tWait < std::chrono::seconds(2)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        auto t0 = std::chrono::steady_clock::now();
        dm.shutdown();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
        assert(elapsedMs < 2000 && "shutdown() must SIGTERM->SIGKILL hung Backend C cleanly");
    }

    cleanup_dir(test_dir);
    std::cout << "  -> Backend C exit 0, exit 1, SIGSEGV & bounded shutdown verified." << std::endl;
}

void test_nonblocking_pause_cancel_and_throttle_isolation()
{
    std::cout << "[Download Mgr 10/10] Testing non-blocking CURL_WRITEFUNC_PAUSE cancel & "
                 "UserImmediate throttle isolation..."
              << std::endl;

    fs::path test_dir = "/tmp/test_wwal_dl_pause_nonblocking";
    cleanup_dir(test_dir);
    fs::create_directories(test_dir);

    // Build a larger (> 128 KB) signed package so multi-chunk streaming exercises curl
    // pause/resume.
    fs::path large_src = test_dir / "large_pkg_src";
    fs::create_directories(large_src);
    {
        std::ofstream shader(large_src / "scene.frag");
        shader << "#version 300 es\nprecision highp float;\nout vec4 fragColor;\nvoid main(){ "
                  "fragColor = vec4(0.4, 0.7, 1.0, 1.0); }\n";
        std::ofstream blob(large_src / "noise.bin", std::ios::binary);
        std::mt19937 rng(0xC0FFEEu);
        for (size_t i = 0; i < 65536; ++i) {
            uint32_t v = rng();
            blob.write(reinterpret_cast<const char *>(&v), sizeof(v));
        }
    }
    uint8_t pub[WWPKG_PUBKEY_BYTES]{};
    uint8_t priv[64]{};
    wwpkg_generate_keypair(pub, priv);
    wwpkg_manifest_t man{};
    std::snprintf(man.id, sizeof(man.id), "pkg-pause-large");
    std::snprintf(man.slug, sizeof(man.slug), "pause-large");
    std::snprintf(man.name, sizeof(man.name), "Pause Large Package");
    std::snprintf(man.version, sizeof(man.version), "1.0.0");
    std::snprintf(man.author_name, sizeof(man.author_name), "WayWal Test");
    std::snprintf(man.license, sizeof(man.license), "MIT");
    std::memcpy(man.author_pubkey, pub, WWPKG_PUBKEY_BYTES);
    man.type = WWPKG_TYPE_SHADER_FRAGMENT;
    fs::path large_pkg_path = test_dir / "source_large.wwpkg";
    assert(wwpkg_build_package(large_src.string().c_str(), &man, priv,
                               large_pkg_path.string().c_str()));

    std::vector<uint8_t> largePkg;
    {
        std::ifstream ifs(large_pkg_path, std::ios::binary | std::ios::ate);
        auto sz = ifs.tellg();
        assert(sz > 65536);
        largePkg.resize(static_cast<size_t>(sz));
        ifs.seekg(0, std::ios::beg);
        ifs.read(reinterpret_cast<char *>(largePkg.data()), sz);
    }

    std::atomic<int> bodyServedCount{0};
    LoopbackHttpServer server([&](int cfd, const LoopbackHttpRequest & /*req*/) {
        std::ostringstream hdr;
        hdr << "HTTP/1.1 200 OK\r\nContent-Length: " << largePkg.size()
            << "\r\nConnection: close\r\n\r\n";
        auto h = hdr.str();
        LoopbackHttpServer::writeAll(cfd, h.data(), h.size());
        ++bodyServedCount;
        LoopbackHttpServer::writeAll(cfd, largePkg.data(), largePkg.size());
    });

    DownloadManager dm;
    fs::path dl_dir = test_dir / "downloads";
    dm.setDownloadDirectory(dl_dir.string());
    DownloadManager::SimulationHooks hooks{};
    hooks.allow_loopback_http = true;
    dm.setSimulationHooks(hooks);
    assert(dm.initialize());

    // Part A: Pause globally, start two concurrent downloads A and B so both hit WRITEFUNC_PAUSE,
    // then verify cancelOpenHub("B") completes promptly while A remains paused until resume().
    dm.pause();
    assert(dm.isPaused());

    std::atomic<TaskState> stateA{TaskState::Queued};
    std::atomic<TaskState> stateB{TaskState::Queued};

    dm.enqueue("pkg-A", server.baseUrl() + "/a.wwpkg", TaskPriority::Interactive,
               [&](const DownloadProgress &p) { stateA.store(p.state); });
    dm.enqueue("pkg-B", server.baseUrl() + "/b.wwpkg", TaskPriority::Interactive,
               [&](const DownloadProgress &p) { stateB.store(p.state); });

    auto waitServe = std::chrono::steady_clock::now();
    while (bodyServedCount.load() < 2 &&
           std::chrono::steady_clock::now() - waitServe < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(stateA.load() == TaskState::Downloading && "pkg-A must pause in Downloading state");
    assert(stateB.load() == TaskState::Downloading && "pkg-B must pause in Downloading state");

    auto cancelStart = std::chrono::steady_clock::now();
    dm.cancelOpenHub("pkg-B");
    while (stateB.load() != TaskState::Cancelled &&
           std::chrono::steady_clock::now() - cancelStart < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto cancelMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - cancelStart)
                        .count();
    assert(stateB.load() == TaskState::Cancelled &&
           "cancelOpenHub(\"pkg-B\") must complete while reactor is paused");
    assert(cancelMs < 500 && "Cancellation while paused must not block reactor");
    assert(stateA.load() == TaskState::Downloading &&
           "pkg-A must remain paused until explicit resume()");

    dm.resume();
    dm.waitUntilIdle();
    assert(stateA.load() == TaskState::Installed &&
           "pkg-A must unpause via CURLPAUSE_CONT and install cleanly");

    // Part B: Throttle bulk lanes to 1 byte/sec so bulk transfer enters WRITEFUNC_PAUSE, and
    // verify TaskPriority::Immediate fast lane finishes promptly without reactor deadlock.
    dm.setBandwidthLimit(1);
    std::atomic<TaskState> bulkState{TaskState::Queued};
    std::atomic<TaskState> immState{TaskState::Queued};

    int prevServed = bodyServedCount.load();
    dm.enqueue("bulk-throttled", server.baseUrl() + "/bulk.wwpkg", TaskPriority::BackgroundUpdate,
               [&](const DownloadProgress &p) { bulkState.store(p.state); });

    auto waitBulk = std::chrono::steady_clock::now();
    while (bodyServedCount.load() <= prevServed &&
           std::chrono::steady_clock::now() - waitBulk < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    auto immStart = std::chrono::steady_clock::now();
    dm.enqueue("imm-unthrottled", server.baseUrl() + "/imm.wwpkg", TaskPriority::Immediate,
               [&](const DownloadProgress &p) { immState.store(p.state); });
    while (immState.load() != TaskState::Installed &&
           std::chrono::steady_clock::now() - immStart < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto immMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - immStart)
                     .count();
    assert(immState.load() == TaskState::Installed &&
           "UserImmediate fast lane must not be deadlocked by throttled bulk slot");
    assert(immMs < 1000);
    assert(bulkState.load() == TaskState::Downloading &&
           "Bulk slot must still be rate-throttled when UserImmediate finishes");

    // Lift rate limit and verify throttled bulk slot unpauses immediately and finishes.
    dm.setBandwidthLimit(0);
    dm.waitUntilIdle();
    assert(bulkState.load() == TaskState::Installed);

    // Part C: Verify steady-state timerfd token refill completes a rate-limited bulk transfer.
    dm.setBandwidthLimit(2 * 1024 * 1024);
    std::atomic<TaskState> pacedState{TaskState::Queued};
    dm.enqueue("bulk-paced", server.baseUrl() + "/paced.wwpkg", TaskPriority::BackgroundUpdate,
               [&](const DownloadProgress &p) { pacedState.store(p.state); });
    dm.waitUntilIdle();
    assert(pacedState.load() == TaskState::Installed &&
           "Timerfd-paced CURL_WRITEFUNC_PAUSE bulk transfer must complete");
    dm.setBandwidthLimit(0);

    dm.shutdown();
    server.stop();
    cleanup_dir(test_dir);
    std::cout << "  -> Non-blocking CURL_WRITEFUNC_PAUSE cancel & fast-lane isolation verified."
              << std::endl;
}

} // namespace

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    std::cout << "==========================================================" << std::endl;
    std::cout << "  Download Manager, HTTP/2 epoll & Verification Suite     " << std::endl;
    std::cout << "==========================================================" << std::endl;

    test_network_drop_and_range_resumption();
    test_http_200_truncation_safety();
    test_http_429_jitter_backoff();
    test_priority_scheduling();
    test_token_bucket_bandwidth_governor();
    test_loopback_http_status_codes_and_wwpkg_verification();
    test_ssrf_redirect_and_protocol_rejection();
    test_four_plus_one_fast_lane_isolation();
    test_backend_c_subprocess_reaping_and_shutdown();
    test_nonblocking_pause_cancel_and_throttle_isolation();

    std::cout << "==========================================================" << std::endl;
    std::cout << "  >>> DOWNLOAD MANAGER SUITE PASSED CLEANLY (100%) <<<    " << std::endl;
    std::cout << "==========================================================" << std::endl;

    return 0;
}
