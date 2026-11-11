// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_recursive_fs_watcher.cpp
 * @brief Scoped Recursive inotify / epoll Watcher & Debounce Verification Suite
 */

#include "studio/core/recursive_fs_watcher.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace wwal::studio;

namespace {

void cleanup_test_dir(const std::filesystem::path &dir)
{
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void test_dual_probe_fallback()
{
    std::cout << "[FS Watcher 1/6] Testing watch root registration & scoped watch count..."
              << std::endl;

    std::filesystem::path test_dir = "/tmp/test_wwal_watcher_probe";
    cleanup_test_dir(test_dir);
    std::filesystem::create_directories(test_dir / "431960" / "1001" / "deep" / "nested" /
                                        "assets");

    std::atomic<bool> event_received{false};
    RecursiveFsWatcher watcher(
        [&](const std::vector<FsWatchEvent> &) { event_received.store(true); }, 100);

    bool added = watcher.addWatchRoot(test_dir);
    assert(added && "Adding watch root must succeed");

    watcher.start();
    // Depth <= 2 watches root (0), 431960 (1), and 1001 (2), skipping deep/nested/assets (>= 3).
    assert(watcher.totalWatches() == 3 &&
           "Watches must be bounded to depth <= 2 relative to registered root");
    watcher.stop();

    cleanup_test_dir(test_dir);
    std::cout << "  -> Scoped depth <= 2 watch registration passed." << std::endl;
}

void test_dynamic_subdirectory_recursion()
{
    std::cout << "[FS Watcher 2/6] Testing dynamic subdirectory recursive attachment..."
              << std::endl;

    std::filesystem::path test_root = "/tmp/test_wwal_watcher_recurse";
    cleanup_test_dir(test_root);
    std::filesystem::create_directories(test_root);

    std::atomic<int> events_count{0};
    RecursiveFsWatcher watcher(
        [&](const std::vector<FsWatchEvent> &events) {
            events_count.fetch_add(static_cast<int>(events.size()));
        },
        120);

    watcher.addWatchRoot(test_root);
    watcher.start();

    std::filesystem::path sub1 = test_root / "subdir1";
    std::filesystem::path sub2 = sub1 / "subdir2";
    std::filesystem::create_directories(sub2);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::filesystem::path deep_file = sub2 / "test.txt";
    {
        std::ofstream ofs(deep_file);
        ofs << "testing dynamic recursion";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(events_count.load() > 0 &&
           "Events must be captured from dynamically created subdirectories");
    watcher.stop();

    cleanup_test_dir(test_root);
    std::cout << "  -> Dynamic recursive subdirectory attachment passed." << std::endl;
}

void test_burst_write_coalescing()
{
    std::cout << "[FS Watcher 3/6] Testing 1,000 file write burst coalescing..." << std::endl;

    std::filesystem::path test_root = "/tmp/test_wwal_watcher_burst";
    cleanup_test_dir(test_root);
    std::filesystem::path item_dir = test_root / "431960" / "99887766";
    std::filesystem::create_directories(item_dir);

    std::atomic<int> callback_invocations{0};
    std::vector<FsWatchEvent> received_events;
    std::mutex events_mutex;

    RecursiveFsWatcher watcher(
        [&](const std::vector<FsWatchEvent> &events) {
            callback_invocations.fetch_add(1);
            std::lock_guard<std::mutex> lock(events_mutex);
            for (const auto &ev : events) {
                received_events.push_back(ev);
            }
        },
        300);

    watcher.addWatchRoot(test_root);
    watcher.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    constexpr int NUM_WRITES = 1000;
    std::filesystem::path target_file = item_dir / "scene.pkg";
    for (int i = 0; i < NUM_WRITES; ++i) {
        std::ofstream ofs(target_file, std::ios::app);
        ofs << "chunk_" << i << "\n";
        if (i % 200 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    watcher.stop();

    int invocations = callback_invocations.load();
    assert(invocations >= 1 && invocations <= 10 &&
           "1,000 writes must be coalesced by 300ms debounce filter");

    bool found_item_id = false;
    {
        std::lock_guard<std::mutex> lock(events_mutex);
        for (const auto &ev : received_events) {
            if (ev.published_file_id == 99887766ULL) {
                found_item_id = true;
                break;
            }
        }
    }
    assert(found_item_id && "Coalesced event must report correct published_file_id");

    cleanup_test_dir(test_root);
    std::cout << "  -> 1,000-write burst debouncing passed." << std::endl;
}

void test_start_before_add_and_restart_after_stop()
{
    std::cout << "[FS Watcher 4/6] Testing start()-before-addWatchRoot() & stop()->start() "
                 "restart..."
              << std::endl;

    std::filesystem::path test_root = "/tmp/test_wwal_watcher_lifecycle";
    cleanup_test_dir(test_root);
    std::filesystem::create_directories(test_root / "431960");

    std::atomic<int> seen_first_run{0};
    std::atomic<int> seen_second_run{0};
    std::atomic<bool> second_phase{false};

    RecursiveFsWatcher watcher(
        [&](const std::vector<FsWatchEvent> &events) {
            for (const auto &ev : events) {
                if (ev.published_file_id == 424242ULL || ev.published_file_id == 535353ULL) {
                    if (second_phase.load()) {
                        seen_second_run.fetch_add(1);
                    } else {
                        seen_first_run.fetch_add(1);
                    }
                }
            }
        },
        80);

    // Call start() BEFORE addWatchRoot()
    watcher.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    assert(watcher.addWatchRoot(test_root));
    std::filesystem::create_directories(test_root / "431960" / "424242");
    {
        std::ofstream ofs(test_root / "431960" / "424242" / "project.json");
        ofs << "{\"title\":\"First Run\"}";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(220));
    assert(seen_first_run.load() >= 1 &&
           "Watcher must stay alive when start() precedes addWatchRoot()");

    // Stop completely and restart without re-calling addWatchRoot()
    watcher.stop();
    second_phase.store(true);
    watcher.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    std::filesystem::create_directories(test_root / "431960" / "535353");
    {
        std::ofstream ofs(test_root / "431960" / "535353" / "project.json");
        ofs << "{\"title\":\"Second Run\"}";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(220));
    watcher.stop();

    assert(seen_second_run.load() >= 1 &&
           "Watcher must re-open descriptors and resume after stop()->start()");

    cleanup_test_dir(test_root);
    std::cout << "  -> start()-before-add and stop()->start() restart passed." << std::endl;
}

void test_child_unlink_vs_item_root_delete()
{
    std::cout << "[FS Watcher 5/6] Testing child temp unlink (ItemModified) vs item root delete "
                 "(ItemDeleted)..."
              << std::endl;

    std::filesystem::path test_root = "/tmp/test_wwal_watcher_del";
    cleanup_test_dir(test_root);
    std::filesystem::path item_dir = test_root / "431960" / "777111";
    std::filesystem::create_directories(item_dir);
    {
        std::ofstream ofs(item_dir / "shader.tmp");
        ofs << "temp";
    }

    std::mutex mtx;
    std::vector<FsWatchEvent> events_log;
    RecursiveFsWatcher watcher(
        [&](const std::vector<FsWatchEvent> &events) {
            std::lock_guard<std::mutex> lk(mtx);
            for (const auto &ev : events) {
                events_log.push_back(ev);
            }
        },
        80);

    watcher.addWatchRoot(test_root);
    watcher.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Unlink ONLY a child temp file inside 777111 while 777111 itself remains intact.
    std::filesystem::remove(item_dir / "shader.tmp");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    {
        std::lock_guard<std::mutex> lk(mtx);
        assert(!events_log.empty());
        for (const auto &ev : events_log) {
            if (ev.published_file_id == 777111ULL) {
                assert(ev.action != FsWatchAction::ItemDeleted &&
                       "Unlinking child temp file must NEVER emit ItemDeleted");
            }
        }
        events_log.clear();
    }

    // Now delete the item root directory 777111 itself -> MUST emit ItemDeleted.
    std::filesystem::remove_all(item_dir);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    watcher.stop();

    bool saw_root_deleted = false;
    {
        std::lock_guard<std::mutex> lk(mtx);
        for (const auto &ev : events_log) {
            if (ev.published_file_id == 777111ULL && ev.action == FsWatchAction::ItemDeleted) {
                saw_root_deleted = true;
            }
        }
    }
    assert(saw_root_deleted && "Deleting item root directory itself must emit ItemDeleted");

    cleanup_test_dir(test_root);
    std::cout << "  -> Child temp unlink vs item root delete verified." << std::endl;
}

void test_sibling_prefix_roots_ws_vs_ws2()
{
    std::cout << "[FS Watcher 6/6] Testing lexically_relative boundary (/ws vs /ws2)..."
              << std::endl;

    std::filesystem::path base = "/tmp/test_wwal_watcher_siblings";
    cleanup_test_dir(base);
    std::filesystem::path ws1 = base / "ws";
    std::filesystem::path ws2 = base / "ws2";
    std::filesystem::create_directories(ws1 / "431960" / "111");
    std::filesystem::create_directories(ws2 / "431960" / "222");

    std::atomic<bool> saw_ws2_event{false};
    RecursiveFsWatcher watcher(
        [&](const std::vector<FsWatchEvent> &events) {
            for (const auto &ev : events) {
                if (ev.published_file_id == 222ULL) {
                    saw_ws2_event.store(true);
                }
            }
        },
        80);

    watcher.addWatchRoot(ws1);
    watcher.addWatchRoot(ws2);
    watcher.start();
    assert(watcher.totalWatches() == 6);

    // Removing /ws must leave /ws2's 3 watches intact.
    watcher.removeWatchRoot(ws1);
    assert(watcher.totalWatches() == 3 && "Removing /ws must not strip sibling /ws2 watches");

    {
        std::ofstream ofs(ws2 / "431960" / "222" / "project.json");
        ofs << "{\"title\":\"Sibling Alive\"}";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    watcher.stop();

    assert(saw_ws2_event.load() && "/ws2 must still emit events after /ws removal");

    cleanup_test_dir(base);
    std::cout << "  -> Sibling /ws vs /ws2 boundary isolation passed." << std::endl;
}

void test_teardown_discards_pending_and_open_fd_coalescing()
{
    std::cout << "[FS Watcher 7/7] Testing silent stop() discard & open-FD streaming silence..."
              << std::endl;

    std::filesystem::path test_root = "/tmp/test_wwal_watcher_teardown";
    cleanup_test_dir(test_root);
    std::filesystem::path item_dir = test_root / "431960" / "888999";
    std::filesystem::create_directories(item_dir);

    // Part A: Pending debounce events must be discarded without invoking callback on stop()/dtor.
    std::atomic<int> teardown_callbacks{0};
    {
        RecursiveFsWatcher watcher(
            [&](const std::vector<FsWatchEvent> &) { teardown_callbacks.fetch_add(1); }, 2500);
        watcher.addWatchRoot(test_root);
        watcher.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        {
            std::ofstream ofs(item_dir / "pending.json");
            ofs << "{\"title\":\"Pending Before Stop\"}";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        watcher.stop();
    }
    assert(teardown_callbacks.load() == 0 &&
           "stop() and destructor must discard pending debounce events without firing callback");

    // Part B: Streaming write() syscalls on an open FD must not emit intermediate events until
    // close() triggers IN_CLOSE_WRITE.
    std::atomic<int> stream_callbacks{0};
    RecursiveFsWatcher stream_watcher(
        [&](const std::vector<FsWatchEvent> &) { stream_callbacks.fetch_add(1); }, 40);
    stream_watcher.addWatchRoot(test_root);
    stream_watcher.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    std::filesystem::path stream_file = item_dir / "streaming_download.part";
    int fd = ::open(stream_file.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
    assert(fd >= 0);

    // Allow initial IN_CREATE event from O_CREAT to flush through the 40 ms debounce window.
    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    stream_callbacks.store(0);

    const char chunk[] = "0123456789abcdef";
    for (int i = 0; i < 10; ++i) {
        ssize_t w = ::write(fd, chunk, sizeof(chunk) - 1);
        assert(w == static_cast<ssize_t>(sizeof(chunk) - 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    assert(stream_callbacks.load() == 0 &&
           "Streaming write() calls without close() must not trigger intermediate modify storms");

    ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    stream_watcher.stop();
    assert(stream_callbacks.load() >= 1 &&
           "Closing written file descriptor must emit IN_CLOSE_WRITE notification");

    cleanup_test_dir(test_root);
    std::cout << "  -> Silent stop() discard & open-FD IN_CLOSE_WRITE coalescing passed."
              << std::endl;
}

} // namespace

int main()
{
    std::cout << "==========================================================" << std::endl;
    std::cout << "  Scoped Recursive inotify / epoll Watcher Test Suite     " << std::endl;
    std::cout << "==========================================================" << std::endl;

    test_dual_probe_fallback();
    test_dynamic_subdirectory_recursion();
    test_burst_write_coalescing();
    test_start_before_add_and_restart_after_stop();
    test_child_unlink_vs_item_root_delete();
    test_sibling_prefix_roots_ws_vs_ws2();
    test_teardown_discards_pending_and_open_fd_coalescing();

    std::cout << "==========================================================" << std::endl;
    std::cout << "  >>> RECURSIVE FS WATCHER TESTS PASSED CLEANLY (100%) <<<" << std::endl;
    std::cout << "==========================================================" << std::endl;

    return 0;
}
