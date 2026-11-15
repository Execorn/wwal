// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_recursive_fs_watcher.cpp
 * @brief Scoped Recursive inotify / epoll Watcher & Debounce Verification Suite
 */

#include "studio/core/recursive_fs_watcher.hpp"

#include <fcntl.h>
#include <sys/stat.h>
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
#include <unordered_set>
#include <vector>

using namespace wwal::studio;

namespace {

void cleanup_test_dir(const std::filesystem::path &dir)
{
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

template <typename Pred>
bool wait_until(Pred &&pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

void test_dual_probe_fallback()
{
    std::cout << "[FS Watcher 1/12] Testing watch root registration & scoped watch count..."
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
    std::cout << "[FS Watcher 2/12] Testing dynamic subdirectory recursive attachment..."
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

    assert(wait_until([&]() { return events_count.load() > 0; }) &&
           "Events must be captured from dynamically created subdirectories");
    watcher.stop();

    cleanup_test_dir(test_root);
    std::cout << "  -> Dynamic recursive subdirectory attachment passed." << std::endl;
}

void test_burst_write_coalescing()
{
    std::cout << "[FS Watcher 3/12] Testing 1,000 file write burst coalescing..." << std::endl;

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
    std::cout << "[FS Watcher 4/12] Testing start()-before-addWatchRoot() & stop()->start() "
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

    watcher.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    assert(watcher.addWatchRoot(test_root));
    std::filesystem::create_directories(test_root / "431960" / "424242");
    {
        std::ofstream ofs(test_root / "431960" / "424242" / "project.json");
        ofs << "{\"title\":\"First Run\"}";
    }
    assert(wait_until([&]() { return seen_first_run.load() >= 1; }) &&
           "Watcher must stay alive when start() precedes addWatchRoot()");

    watcher.stop();
    second_phase.store(true);
    watcher.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    std::filesystem::create_directories(test_root / "431960" / "535353");
    {
        std::ofstream ofs(test_root / "431960" / "535353" / "project.json");
        ofs << "{\"title\":\"Second Run\"}";
    }
    assert(wait_until([&]() { return seen_second_run.load() >= 1; }) &&
           "Watcher must re-open descriptors and resume after stop()->start()");
    watcher.stop();

    cleanup_test_dir(test_root);
    std::cout << "  -> start()-before-add and stop()->start() restart passed." << std::endl;
}

void test_child_unlink_vs_item_root_delete()
{
    std::cout << "[FS Watcher 5/12] Testing child temp unlink (ItemModified) vs item root delete "
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

    std::filesystem::remove(item_dir / "shader.tmp");
    assert(wait_until([&]() {
        std::lock_guard<std::mutex> lk(mtx);
        return !events_log.empty();
    }));

    {
        std::lock_guard<std::mutex> lk(mtx);
        for (const auto &ev : events_log) {
            if (ev.published_file_id == 777111ULL) {
                assert(ev.action != FsWatchAction::ItemDeleted &&
                       "Unlinking child temp file must NEVER emit ItemDeleted");
            }
        }
        events_log.clear();
    }

    std::filesystem::remove_all(item_dir);
    assert(wait_until([&]() {
               std::lock_guard<std::mutex> lk(mtx);
               for (const auto &ev : events_log) {
                   if (ev.published_file_id == 777111ULL &&
                       ev.action == FsWatchAction::ItemDeleted) {
                       return true;
                   }
               }
               return false;
           }) &&
           "Deleting item root directory itself must emit ItemDeleted");
    watcher.stop();

    cleanup_test_dir(test_root);
    std::cout << "  -> Child temp unlink vs item root delete verified." << std::endl;
}

void test_sibling_prefix_roots_ws_vs_ws2()
{
    std::cout << "[FS Watcher 6/12] Testing lexically_relative boundary (/ws vs /ws2)..."
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

    watcher.removeWatchRoot(ws1);
    assert(watcher.totalWatches() == 3 && "Removing /ws must not strip sibling /ws2 watches");

    {
        std::ofstream ofs(ws2 / "431960" / "222" / "project.json");
        ofs << "{\"title\":\"Sibling Alive\"}";
    }
    assert(wait_until([&]() { return saw_ws2_event.load(); }) &&
           "/ws2 must still emit events after /ws removal");
    watcher.stop();

    cleanup_test_dir(base);
    std::cout << "  -> Sibling /ws vs /ws2 boundary isolation passed." << std::endl;
}

void test_teardown_discards_pending_and_open_fd_coalescing()
{
    std::cout << "[FS Watcher 7/12] Testing silent stop() discard & open-FD streaming silence..."
              << std::endl;

    std::filesystem::path test_root = "/tmp/test_wwal_watcher_teardown";
    cleanup_test_dir(test_root);
    std::filesystem::path item_dir = test_root / "431960" / "888999";
    std::filesystem::create_directories(item_dir);

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

    std::atomic<int> stream_callbacks{0};
    RecursiveFsWatcher stream_watcher(
        [&](const std::vector<FsWatchEvent> &) { stream_callbacks.fetch_add(1); }, 40);
    stream_watcher.addWatchRoot(test_root);
    stream_watcher.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    std::filesystem::path stream_file = item_dir / "streaming_download.part";
    int fd = ::open(stream_file.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
    assert(fd >= 0);

    assert(wait_until([&]() { return stream_callbacks.load() >= 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
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
    assert(wait_until([&]() { return stream_callbacks.load() >= 1; }) &&
           "Closing written file descriptor must emit IN_CLOSE_WRITE notification");
    stream_watcher.stop();

    cleanup_test_dir(test_root);
    std::cout << "  -> Silent stop() discard & open-FD IN_CLOSE_WRITE coalescing passed."
              << std::endl;
}

void test_cookie_paired_item_renamed_and_self_move()
{
    std::cout << "[FS Watcher 8/12] Testing cookie-paired ItemRenamed (files & nested subdirs), "
                 "prefix migration & external move..."
              << std::endl;

    std::filesystem::path base = "/tmp/test_wwal_watcher_rename";
    cleanup_test_dir(base);
    std::filesystem::path test_root = base / "root";
    std::filesystem::path outside = base / "outside";
    std::filesystem::create_directories(test_root / "parent_a" / "child_old");
    std::filesystem::create_directories(test_root / "parent_b");
    std::filesystem::create_directories(test_root / "431960" / "1001");
    std::filesystem::create_directories(outside);
    {
        std::ofstream ofs(test_root / "parent_a" / "child_old" / "alpha.txt");
        ofs << "hello";
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
        60);

    assert(watcher.addWatchRoot(test_root));
    watcher.start();
    const size_t initialWatches = watcher.totalWatches();
    assert(initialWatches == 6); // root, parent_a, child_old, parent_b, 431960, 1001

    // 1. File rename inside watched subdirectory
    std::filesystem::rename(test_root / "parent_a" / "child_old" / "alpha.txt",
                            test_root / "parent_a" / "child_old" / "beta.txt");
    assert(wait_until([&]() {
               std::lock_guard<std::mutex> lk(mtx);
               for (const auto &ev : events_log) {
                   if (ev.action == FsWatchAction::ItemRenamed &&
                       ev.full_path.filename() == "beta.txt" &&
                       ev.old_path.filename() == "alpha.txt") {
                       return true;
                   }
               }
               return false;
           }) &&
           "File rename must emit cookie-paired ItemRenamed with old_path and full_path");

    // 2. Nested directory rename across parents + watch prefix migration
    {
        std::lock_guard<std::mutex> lk(mtx);
        events_log.clear();
    }
    std::filesystem::rename(test_root / "parent_a" / "child_old",
                            test_root / "parent_b" / "child_new");
    assert(wait_until([&]() {
               std::lock_guard<std::mutex> lk(mtx);
               for (const auto &ev : events_log) {
                   if (ev.action == FsWatchAction::ItemRenamed &&
                       ev.full_path == (test_root / "parent_b" / "child_new") &&
                       ev.old_path == (test_root / "parent_a" / "child_old")) {
                       return true;
                   }
               }
               return false;
           }) &&
           "Nested directory rename must emit ItemRenamed");
    assert(watcher.totalWatches() == initialWatches &&
           "Directory rename must migrate watches without leaking or losing watch count");

    // Verify migrated watch emits events for files created inside the renamed directory.
    {
        std::lock_guard<std::mutex> lk(mtx);
        events_log.clear();
    }
    {
        std::ofstream ofs(test_root / "parent_b" / "child_new" / "post_rename.txt");
        ofs << "migrated";
    }
    assert(
        wait_until([&]() {
            std::lock_guard<std::mutex> lk(mtx);
            for (const auto &ev : events_log) {
                if (ev.full_path == (test_root / "parent_b")) {
                    return true;
                }
            }
            return false;
        }) &&
        "Migrated watch on renamed directory must report subsequent child events under new root");

    // 3. Item root rename (431960/1001 -> 431960/2002)
    {
        std::lock_guard<std::mutex> lk(mtx);
        events_log.clear();
    }
    std::filesystem::rename(test_root / "431960" / "1001", test_root / "431960" / "2002");
    assert(wait_until([&]() {
               std::lock_guard<std::mutex> lk(mtx);
               for (const auto &ev : events_log) {
                   if (ev.action == FsWatchAction::ItemRenamed && ev.published_file_id == 2002ULL &&
                       ev.full_path == (test_root / "431960" / "2002") &&
                       ev.old_path == (test_root / "431960" / "1001")) {
                       return true;
                   }
               }
               return false;
           }) &&
           "Item root rename must emit ItemRenamed with updated published_file_id");
    assert(watcher.totalWatches() == initialWatches);

    // 4. Unpaired IN_MOVED_FROM (moved outside watched root) expires into ItemDeleted & removes
    // watch
    {
        std::lock_guard<std::mutex> lk(mtx);
        events_log.clear();
    }
    std::filesystem::rename(test_root / "431960" / "2002", outside / "2002");
    assert(wait_until([&]() {
               std::lock_guard<std::mutex> lk(mtx);
               for (const auto &ev : events_log) {
                   if (ev.action == FsWatchAction::ItemDeleted && ev.published_file_id == 2002ULL) {
                       return true;
                   }
               }
               return false;
           }) &&
           "Unpaired move out of watched tree must expire into ItemDeleted");
    assert(watcher.totalWatches() == initialWatches - 1 &&
           "Watch for moved-out directory must be cleaned up");

    // 5. Moving the watch root itself triggers IN_MOVE_SELF and cleans up stale watches.
    std::filesystem::rename(test_root, outside / "moved_root");
    assert(wait_until([&]() { return watcher.totalWatches() == 0; }) &&
           "IN_MOVE_SELF on moved watch root must drop all stale watches");

    watcher.stop();
    cleanup_test_dir(base);
    std::cout << "  -> Cookie-paired ItemRenamed & IN_MOVE_SELF cleanup passed." << std::endl;
}

void test_remove_watch_root_purges_pending_events()
{
    std::cout << "[FS Watcher 9/12] Testing removeWatchRoot() buffered event purge..." << std::endl;

    std::filesystem::path base = "/tmp/test_wwal_watcher_purge";
    cleanup_test_dir(base);
    std::filesystem::path ws1 = base / "ws1";
    std::filesystem::path ws2 = base / "ws2";
    std::filesystem::create_directories(ws1 / "431960" / "1111");
    std::filesystem::create_directories(ws2 / "431960" / "2222");

    std::mutex mtx;
    std::vector<FsWatchEvent> events_log;
    RecursiveFsWatcher watcher(
        [&](const std::vector<FsWatchEvent> &events) {
            std::lock_guard<std::mutex> lk(mtx);
            for (const auto &ev : events) {
                events_log.push_back(ev);
            }
        },
        200);

    assert(watcher.addWatchRoot(ws1));
    assert(watcher.addWatchRoot(ws2));
    watcher.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    {
        std::ofstream ofs1(ws1 / "431960" / "1111" / "pending.txt");
        ofs1 << "should be purged";
        std::ofstream ofs2(ws2 / "431960" / "2222" / "keep.txt");
        ofs2 << "should arrive";
    }

    // Give inotify a moment to buffer both events before the 200ms debounce deadline expires.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    watcher.removeWatchRoot(ws1);

    assert(wait_until([&]() {
        std::lock_guard<std::mutex> lk(mtx);
        for (const auto &ev : events_log) {
            if (ev.published_file_id == 2222ULL) {
                return true;
            }
        }
        return false;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    watcher.stop();

    {
        std::lock_guard<std::mutex> lk(mtx);
        for (const auto &ev : events_log) {
            assert(ev.published_file_id != 1111ULL &&
                   "removeWatchRoot() must purge buffered events for the removed root");
        }
    }

    cleanup_test_dir(base);
    std::cout << "  -> removeWatchRoot() buffered event purge passed." << std::endl;
}

void test_symlink_and_file_root_rejection()
{
    std::cout << "[FS Watcher 10/12] Testing regular file & symlink rejection in addWatchRoot()..."
              << std::endl;

    std::filesystem::path base = "/tmp/test_wwal_watcher_reject";
    cleanup_test_dir(base);
    std::filesystem::path realDir = base / "real_dir";
    std::filesystem::path extDir = base / "external_dir";
    std::filesystem::create_directories(realDir);
    std::filesystem::create_directories(extDir);

    std::filesystem::path regFile = base / "regular.txt";
    {
        std::ofstream ofs(regFile);
        ofs << "file";
    }
    std::filesystem::path symDir = base / "sym_dir";
    std::filesystem::create_directory_symlink(realDir, symDir);
    std::filesystem::path brokenSym = base / "broken_sym";
    std::filesystem::create_symlink(base / "does_not_exist", brokenSym);
    std::filesystem::create_directory_symlink(extDir, realDir / "child_sym");

    RecursiveFsWatcher watcher([](const std::vector<FsWatchEvent> &) {}, 80);

    assert(!watcher.addWatchRoot(regFile) && "Regular file root must be rejected");
    assert(!watcher.addWatchRoot(symDir) && "Symlink directory root must be rejected");
    assert(!watcher.addWatchRoot(symDir / "") &&
           "Symlink directory root with trailing slash must be rejected");
    assert(!watcher.addWatchRoot(brokenSym) && "Broken symlink root must be rejected");
    assert(!watcher.addWatchRoot(base / "missing") && "Non-existent root must be rejected");
    assert(watcher.totalWatches() == 0);

    assert(watcher.addWatchRoot(realDir));
    assert(watcher.totalWatches() == 1 &&
           "Symlinked child directories must not be recursively watched");
    watcher.stop();

    cleanup_test_dir(base);
    std::cout << "  -> Regular file & symlink root rejection passed." << std::endl;
}

void test_callback_stop_and_concurrent_stop_stress()
{
    std::cout << "[FS Watcher 11/12] Testing stop() inside callback & concurrent stop() race "
                 "safety..."
              << std::endl;

    std::filesystem::path test_root = "/tmp/test_wwal_watcher_conc_stop";
    cleanup_test_dir(test_root);
    std::filesystem::create_directories(test_root / "431960" / "3030");

    // Part A: Callback invokes watcher.stop() without self-deadlock, then restarts cleanly.
    std::atomic<int> cbCount{0};
    std::unique_ptr<RecursiveFsWatcher> cbWatcher;
    cbWatcher = std::make_unique<RecursiveFsWatcher>(
        [&](const std::vector<FsWatchEvent> &) {
            cbCount.fetch_add(1);
            cbWatcher->stop();
        },
        30);

    assert(cbWatcher->addWatchRoot(test_root));
    cbWatcher->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        std::ofstream ofs(test_root / "431960" / "3030" / "trigger.txt");
        ofs << "stop self";
    }
    assert(wait_until([&]() { return cbCount.load() >= 1; }) &&
           "Callback calling stop() must complete without deadlock");
    cbWatcher->stop(); // External idempotent stop after callback-stop
    assert(cbCount.load() == 1);

    // Restart after callback-initiated stop()
    cbWatcher->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        std::ofstream ofs(test_root / "431960" / "3030" / "trigger2.txt");
        ofs << "restart ok";
    }
    assert(wait_until([&]() { return cbCount.load() >= 2; }) &&
           "Watcher must restart cleanly after callback-initiated stop()");
    cbWatcher->stop();
    cbWatcher.reset();

    // Part B: Concurrent stop() from 4 threads while writer thread hammers filesystem events.
    for (int iter = 0; iter < 15; ++iter) {
        RecursiveFsWatcher stressWatcher([](const std::vector<FsWatchEvent> &) {}, 10);
        assert(stressWatcher.addWatchRoot(test_root));
        stressWatcher.start();

        std::atomic<bool> stopWriters{false};
        std::thread writer([&]() {
            int k = 0;
            while (!stopWriters.load(std::memory_order_relaxed)) {
                std::ofstream ofs(test_root / "431960" / "3030" / "burst.txt", std::ios::trunc);
                ofs << (k++);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        std::vector<std::thread> stoppers;
        for (int t = 0; t < 4; ++t) {
            stoppers.emplace_back([&]() { stressWatcher.stop(); });
        }
        for (auto &t : stoppers) {
            t.join();
        }
        stopWriters.store(true, std::memory_order_relaxed);
        writer.join();
    }

    cleanup_test_dir(test_root);
    std::cout << "  -> Callback stop() & concurrent stop() stress passed." << std::endl;
}

void test_overflow_rescan_and_min_heap_coalescing()
{
    std::cout << "[FS Watcher 12/12] Testing IN_Q_OVERFLOW rescan, stale wd cleanup & min-heap "
                 "multi-item coalescing..."
              << std::endl;

    // Part A: Real IN_Q_OVERFLOW kernel overflow + subdir rescan + CatalogOverflowReconcile
    size_t maxQueuedEvents = 0;
    {
        std::ifstream ifs("/proc/sys/fs/inotify/max_queued_events");
        if (ifs) {
            ifs >> maxQueuedEvents;
        }
    }

    if (maxQueuedEvents > 0 && maxQueuedEvents <= 65536) {
        std::filesystem::path ovRoot = "/tmp/test_wwal_watcher_overflow";
        cleanup_test_dir(ovRoot);
        std::filesystem::create_directories(ovRoot / "431960");
        std::filesystem::path spamFileA = ovRoot / "431960" / "spam_a.tmp";
        std::filesystem::path spamFileB = ovRoot / "431960" / "spam_b.tmp";
        {
            std::ofstream ofsA(spamFileA);
            ofsA << "a";
            std::ofstream ofsB(spamFileB);
            ofsB << "b";
        }

        std::atomic<bool> sawOverflow{false};
        std::atomic<bool> sawMissedItemEvent{false};
        RecursiveFsWatcher ovWatcher(
            [&](const std::vector<FsWatchEvent> &events) {
                for (const auto &ev : events) {
                    if (ev.action == FsWatchAction::CatalogOverflowReconcile) {
                        sawOverflow.store(true);
                    }
                    if (ev.published_file_id == 654321ULL) {
                        sawMissedItemEvent.store(true);
                    }
                }
            },
            40);

        assert(ovWatcher.addWatchRoot(ovRoot));
        assert(ovWatcher.totalWatches() == 2); // ovRoot, ovRoot/431960

        // Alternate across two distinct filenames so the kernel inotify queue cannot
        // tail-merge adjacent identical events before reaching max_queued_events.
        const size_t totalSpam = maxQueuedEvents + 1024;
        for (size_t i = 0; i < totalSpam; ++i) {
            const auto &target = (i & 1U) ? spamFileB : spamFileA;
            ::chmod(target.c_str(), (i & 2U) ? 0644 : 0600);
        }
        std::filesystem::create_directories(ovRoot / "431960" / "654321");

        ovWatcher.start();
        assert(wait_until([&]() { return sawOverflow.load(); }, std::chrono::milliseconds(3000)) &&
               "IN_Q_OVERFLOW must emit CatalogOverflowReconcile");
        assert(ovWatcher.totalWatches() == 3 &&
               "IN_Q_OVERFLOW rescan must attach watches to missed subdirectories");

        {
            std::ofstream ofs(ovRoot / "431960" / "654321" / "recovered.json");
            ofs << "{}";
        }
        assert(wait_until([&]() { return sawMissedItemEvent.load(); }) &&
               "Re-attached watch from IN_Q_OVERFLOW rescan must capture subsequent events");
        ovWatcher.stop();
        cleanup_test_dir(ovRoot);
    }

    // Part B: Min-heap deadline scheduling across 400 distinct items + stale old_wd re-watch check
    std::filesystem::path heapRoot = "/tmp/test_wwal_watcher_heap";
    cleanup_test_dir(heapRoot);
    std::filesystem::create_directories(heapRoot / "431960");
    constexpr int NUM_ITEMS = 400;
    for (int i = 0; i < NUM_ITEMS; ++i) {
        std::filesystem::create_directories(heapRoot / "431960" / std::to_string(10000 + i));
    }

    std::mutex mtx;
    std::unordered_set<uint64_t> seenIds;
    RecursiveFsWatcher heapWatcher(
        [&](const std::vector<FsWatchEvent> &events) {
            std::lock_guard<std::mutex> lk(mtx);
            for (const auto &ev : events) {
                if (ev.published_file_id >= 10000ULL &&
                    ev.published_file_id < 10000ULL + NUM_ITEMS) {
                    seenIds.insert(ev.published_file_id);
                }
            }
        },
        40);

    assert(heapWatcher.addWatchRoot(heapRoot));
    // Re-adding the same root must not inflate watch count.
    assert(heapWatcher.addWatchRoot(heapRoot));
    assert(heapWatcher.totalWatches() == static_cast<size_t>(NUM_ITEMS + 2));

    heapWatcher.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    for (int i = 0; i < NUM_ITEMS; ++i) {
        std::ofstream ofs(heapRoot / "431960" / std::to_string(10000 + i) / "item.json");
        ofs << i;
    }

    assert(wait_until(
               [&]() {
                   std::lock_guard<std::mutex> lk(mtx);
                   return seenIds.size() == static_cast<size_t>(NUM_ITEMS);
               },
               std::chrono::milliseconds(3000)) &&
           "Min-heap debounce must flush all 400 distinct items");

    heapWatcher.stop();
    cleanup_test_dir(heapRoot);
    std::cout << "  -> IN_Q_OVERFLOW rescan & min-heap multi-item coalescing passed." << std::endl;
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
    test_cookie_paired_item_renamed_and_self_move();
    test_remove_watch_root_purges_pending_events();
    test_symlink_and_file_root_rejection();
    test_callback_stop_and_concurrent_stop_stress();
    test_overflow_rescan_and_min_heap_coalescing();

    std::cout << "==========================================================" << std::endl;
    std::cout << "  >>> RECURSIVE FS WATCHER TESTS PASSED CLEANLY (100%) <<<" << std::endl;
    std::cout << "==========================================================" << std::endl;

    return 0;
}
