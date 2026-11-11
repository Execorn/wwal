// SPDX-License-Identifier: GPL-3.0-or-later
#include "workshop_browser_widget.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMetaObject>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "grid/fuzzy_catalog_index.hpp"
#include "grid/hover_preview_controller.hpp"
#include "grid/virtualized_grid_widget.hpp"
#include "queue/download_manager.hpp"

#include <waywal/thumb_cache.h>
#include <waywal/we_appworkshop.h>
#include <waywal/we_compat_scanner.h>
#include <waywal/we_vdf.h>

#if __has_include("core/catalog_index_manager.hpp")
#include "core/catalog_index_manager.hpp"
#elif __has_include("studio/core/catalog_index_manager.hpp")
#include "studio/core/catalog_index_manager.hpp"
#endif

namespace wwal::studio {

namespace {

using CompatScannerPtr = std::unique_ptr<we_compat_scanner_t, decltype(&we_compat_scanner_destroy)>;
using CompatReportPtr = std::unique_ptr<we_compat_report_t, decltype(&we_compat_report_free)>;

struct WorkshopRecord {
    uint64_t published_file_id{0};
    uint64_t owner_steam_id{0};
    QString title;
    QString type{"scene"};
    QString file_path;
    QString preview_path;
    uint64_t file_size_bytes{0};
    uint64_t time_created{0};
    uint64_t time_updated{0};
    uint64_t manifest_id{0};
    uint32_t subscriptions{0};
    uint32_t favorited{0};
    uint32_t views{0};
    float rating{0.0f};
    uint32_t install_state{WAYWAL_UGC_STATE_NONE};
    uint32_t storage_backend{0};
    we_compat_tier_t compat_tier{WE_TIER_1_NATIVE};
    uint32_t compat_score{100};
    float aspect_ratio{1.7777778f};
    uint32_t dominant_color{0x2b303c};
    uint64_t tag_mask{0};
    QString tags_string;
    QString description;

    float estimated_vram_mb{64.0f};
    uint32_t estimated_fps{60};
    std::vector<QString> issue_messages;
    std::vector<QString> repair_descriptions;
    bool compat_details_loaded{false};

    bool last_download_failed{false};
    bool last_download_cancelled{false};
    QString last_error_message;
};

QString formatBytes(uint64_t bytes)
{
    constexpr double KiB = 1024.0;
    constexpr double MiB = KiB * 1024.0;
    constexpr double GiB = MiB * 1024.0;

    if (bytes >= GiB) {
        return QString::asprintf("%.2f GB", static_cast<double>(bytes) / GiB);
    }
    if (bytes >= MiB) {
        return QString::asprintf("%.1f MB", static_cast<double>(bytes) / MiB);
    }
    if (bytes >= KiB) {
        return QString::asprintf("%.1f KB", static_cast<double>(bytes) / KiB);
    }
    return QString::asprintf("%llu B", static_cast<unsigned long long>(bytes));
}

void populateIssuesFromReport(const we_compat_report_t *rep, WorkshopRecord &rec)
{
    rec.compat_tier = rep->tier;
    rec.compat_score = rep->compatibility_score;
    rec.estimated_vram_mb = rep->summary.estimated_vram_mb;
    rec.estimated_fps = rep->summary.estimated_fps > 0 ? rep->summary.estimated_fps : 60u;
    rec.issue_messages.clear();
    rec.repair_descriptions.clear();

    for (uint32_t i = 0; i < rep->issue_count && i < WE_SCAN_MAX_ISSUES; ++i) {
        const char *sev = rep->issues[i].severity == WE_ISSUE_SEV_FATAL     ? "FATAL"
                          : rep->issues[i].severity == WE_ISSUE_SEV_ERROR   ? "ERROR"
                          : rep->issues[i].severity == WE_ISSUE_SEV_WARNING ? "WARN"
                                                                            : "INFO";
        rec.issue_messages.push_back(QString("[%1] %2: %3")
                                         .arg(QString::fromUtf8(sev))
                                         .arg(QString::fromUtf8(rep->issues[i].code))
                                         .arg(QString::fromUtf8(rep->issues[i].message)));
    }

    for (uint32_t i = 0; i < rep->repair_count && i < WE_SCAN_MAX_REPAIRS; ++i) {
        rec.repair_descriptions.push_back(
            QString("Auto-Repair [%1]: %2")
                .arg(QString::fromUtf8(rep->repairs[i].transformation_id))
                .arg(QString::fromUtf8(rep->repairs[i].description)));
    }
}

WorkshopItemRecord toCatalogRecord(const WorkshopRecord &rec)
{
    WorkshopItemRecord out{};
    out.published_file_id = rec.published_file_id;
    out.owner_steam_id = rec.owner_steam_id;
    out.title = rec.title.toStdString();
    out.type = rec.type.toStdString();
    out.file_path = rec.file_path.toStdString();
    out.preview_path = rec.preview_path.toStdString();
    out.file_size_bytes = rec.file_size_bytes;
    out.time_created = rec.time_created;
    out.time_updated = rec.time_updated;
    out.manifest_id = rec.manifest_id;
    out.subscriptions = rec.subscriptions;
    out.favorited = rec.favorited;
    out.views = rec.views;
    out.rating = rec.rating;
    out.install_state = rec.install_state;
    out.storage_backend = rec.storage_backend;
    out.compat_tier = static_cast<uint32_t>(rec.compat_tier);
    out.compat_score = rec.compat_score;
    out.aspect_ratio = rec.aspect_ratio;
    out.dominant_color = rec.dominant_color;
    out.tag_mask = rec.tag_mask;
    out.tags_string = rec.tags_string.toStdString();
    out.description = rec.description.toStdString();
    out.signature_verified = true;
    return out;
}

WorkshopRecord fromCatalogRecord(const WorkshopItemRecord &in)
{
    WorkshopRecord rec{};
    rec.published_file_id = in.published_file_id;
    rec.owner_steam_id = in.owner_steam_id;
    rec.title = QString::fromStdString(in.title);
    rec.type = QString::fromStdString(in.type);
    rec.file_path = QString::fromStdString(in.file_path);
    rec.preview_path = QString::fromStdString(in.preview_path);
    rec.file_size_bytes = in.file_size_bytes;
    rec.time_created = in.time_created;
    rec.time_updated = in.time_updated;
    rec.manifest_id = in.manifest_id;
    rec.subscriptions = in.subscriptions;
    rec.favorited = in.favorited;
    rec.views = in.views;
    rec.rating = in.rating;
    rec.install_state = in.install_state;
    rec.storage_backend = in.storage_backend;
    rec.compat_tier = static_cast<we_compat_tier_t>(std::clamp<uint32_t>(in.compat_tier, 1u, 4u));
    rec.compat_score = in.compat_score;
    rec.aspect_ratio = in.aspect_ratio;
    rec.dominant_color = in.dominant_color;
    rec.tag_mask = in.tag_mask;
    rec.tags_string = QString::fromStdString(in.tags_string);
    rec.description = QString::fromStdString(in.description);
    return rec;
}

std::string resolveConfinedPreviewPath(const std::filesystem::path &itemDir,
                                       std::string_view rawPreview)
{
    if (rawPreview.empty() || rawPreview.front() == '/' ||
        rawPreview.find('\\') != std::string_view::npos ||
        rawPreview.find('\0') != std::string_view::npos) {
        return {};
    }

    std::filesystem::path relInput(rawPreview);
    if (relInput.is_absolute()) {
        return {};
    }

    const std::filesystem::path normItem = itemDir.lexically_normal();
    const std::filesystem::path cand = (normItem / relInput).lexically_normal();
    const std::filesystem::path rel = cand.lexically_relative(normItem);
    if (rel.empty() || rel.is_absolute()) {
        return {};
    }
    const std::string firstComp = rel.begin()->string();
    if (firstComp.empty() || firstComp == "." || firstComp == "..") {
        return {};
    }

    std::filesystem::path cur = normItem;
    std::error_code ec;
    std::filesystem::file_status st{};
    for (const auto &part : rel) {
        if (part == "." || part == "..") {
            return {};
        }
        cur /= part;
        st = std::filesystem::symlink_status(cur, ec);
        if (ec || std::filesystem::is_symlink(st)) {
            return {};
        }
    }
    if (!std::filesystem::is_regular_file(st)) {
        return {};
    }
    return cur.string();
}

std::vector<std::filesystem::path> collectWorkshopRoots()
{
    std::vector<std::filesystem::path> roots;
    std::unordered_set<std::string> seenNorm;
    auto pushUnique = [&](const std::filesystem::path &p) {
        if (p.empty()) {
            return;
        }
        std::string norm = p.lexically_normal().string();
        if (seenNorm.insert(norm).second) {
            roots.push_back(std::filesystem::path(norm));
        }
    };

    const char *envDir = std::getenv("WWAL_WORKSHOP_DIR");
    if (envDir && *envDir) {
        pushUnique(std::filesystem::path(envDir));
    }

    waywal_steam_library_t libs[32];
    const size_t libCount = waywal_steam_discover_libraries(libs, 32);
    for (size_t i = 0; i < libCount; ++i) {
        if (libs[i].path[0] != '\0') {
            pushUnique(std::filesystem::path(libs[i].path) / "steamapps" / "workshop" / "content" /
                       "431960");
        }
    }

    const char *home = std::getenv("HOME");
    if (home && *home) {
        std::filesystem::path h(home);
        pushUnique(h / ".local/share/Steam/steamapps/workshop/content/431960");
        pushUnique(h / ".steam/steam/steamapps/workshop/content/431960");
        pushUnique(h / ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/"
                       "workshop/content/431960");
        pushUnique(h / "snap/steam/common/.local/share/Steam/steamapps/workshop/content/431960");
    }
    return roots;
}

std::vector<WorkshopRecord> scanWorkshopRootsWorker(const std::atomic<bool> &cancelFlag)
{
    std::vector<WorkshopRecord> results;
    std::unordered_set<uint64_t> seenIds;

    constexpr auto dirOpts = std::filesystem::directory_options::skip_permission_denied;

    for (const auto &root : collectWorkshopRoots()) {
        if (cancelFlag.load(std::memory_order_relaxed)) {
            break;
        }

        std::error_code ec;
        if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
            continue;
        }

        // Parse Steam appworkshop_431960.acf / .vdf two levels up if present
        std::unordered_map<uint64_t, waywal_workshop_item_manifest_t> manifestById;
        std::filesystem::path workshopBase = root.parent_path().parent_path();
        for (const char *manName : {"appworkshop_431960.acf", "appworkshop_431960.vdf"}) {
            std::filesystem::path manPath = workshopBase / manName;
            if (std::filesystem::is_regular_file(manPath, ec)) {
                waywal_appworkshop_manifest_t parsed{};
                if (waywal_appworkshop_parse_file(manPath.string().c_str(), &parsed)) {
                    for (size_t i = 0; i < parsed.item_count; ++i) {
                        manifestById[parsed.items[i].published_file_id] = parsed.items[i];
                    }
                    waywal_appworkshop_free(&parsed);
                    break;
                }
            }
        }

        std::filesystem::directory_iterator dirIt(root, dirOpts, ec);
        if (ec) {
            continue;
        }

        for (const auto &entry : dirIt) {
            if (cancelFlag.load(std::memory_order_relaxed)) {
                break;
            }

            std::error_code itemEc;
            if (!entry.is_directory(itemEc)) {
                continue;
            }

            std::string dirName = entry.path().filename().string();
            char *endPtr = nullptr;
            uint64_t pubId = std::strtoull(dirName.c_str(), &endPtr, 10);
            if (pubId == 0 || !endPtr || *endPtr != '\0' || seenIds.contains(pubId)) {
                continue;
            }
            seenIds.insert(pubId);

            WorkshopRecord rec;
            rec.published_file_id = pubId;
            rec.file_path = QString::fromStdString(entry.path().string());
            rec.install_state = WAYWAL_UGC_STATE_INSTALLED | WAYWAL_UGC_STATE_SUBSCRIBED;
            rec.storage_backend = 0;

            auto manIt = manifestById.find(pubId);
            if (manIt != manifestById.end()) {
                rec.file_size_bytes = manIt->second.size_bytes;
                rec.time_updated = manIt->second.time_updated;
                rec.manifest_id = manIt->second.manifest_id;
                if (manIt->second.state_flags != 0) {
                    rec.install_state = manIt->second.state_flags | WAYWAL_UGC_STATE_INSTALLED;
                }
            }

            // Parse root-level keys from project.json (never nested property titles)
            std::filesystem::path projJson = entry.path() / "project.json";
            QString previewRel;
            if (std::filesystem::is_regular_file(projJson, itemEc)) {
                QFile qf(QString::fromStdString(projJson.string()));
                if (qf.open(QIODevice::ReadOnly)) {
                    QByteArray raw = qf.read(2 * 1024 * 1024);
                    QJsonParseError jerr{};
                    QJsonDocument doc = QJsonDocument::fromJson(raw, &jerr);
                    if (doc.isObject()) {
                        QJsonObject obj = doc.object();
                        if (obj.value(QStringLiteral("title")).isString()) {
                            rec.title = obj.value(QStringLiteral("title")).toString().trimmed();
                        }
                        if (obj.value(QStringLiteral("type")).isString()) {
                            rec.type =
                                obj.value(QStringLiteral("type")).toString().trimmed().toLower();
                        }
                        if (obj.value(QStringLiteral("description")).isString()) {
                            rec.description = obj.value(QStringLiteral("description")).toString();
                        }
                        if (obj.value(QStringLiteral("preview")).isString()) {
                            previewRel = obj.value(QStringLiteral("preview")).toString().trimmed();
                        }
                        if (obj.value(QStringLiteral("tags")).isArray()) {
                            QStringList tagList;
                            for (const auto &tv : obj.value(QStringLiteral("tags")).toArray()) {
                                if (tv.isString()) {
                                    QString t = tv.toString().trimmed();
                                    if (!t.isEmpty()) {
                                        tagList.push_back(t);
                                    }
                                }
                            }
                            rec.tags_string = tagList.join(QStringLiteral(", "));
                            rec.tag_mask =
                                WallpaperTags::parseTagsFromString(rec.tags_string.toStdString());
                        }
                    }
                }
            }

            if (rec.title.isEmpty()) {
                rec.title = QStringLiteral("Workshop Wallpaper %1").arg(pubId);
            }
            if (rec.tag_mask == 0) {
                rec.tag_mask = WallpaperTags::parseTagsFromString(rec.title.toStdString());
            }

            if (!previewRel.isEmpty()) {
                std::string resolved =
                    resolveConfinedPreviewPath(entry.path(), previewRel.toStdString());
                if (!resolved.empty()) {
                    rec.preview_path = QString::fromStdString(resolved);
                }
            }
            if (rec.preview_path.isEmpty()) {
                for (const char *ext : {"preview.jpg", "preview.png", "preview.gif"}) {
                    std::string resolved = resolveConfinedPreviewPath(entry.path(), ext);
                    if (!resolved.empty()) {
                        rec.preview_path = QString::fromStdString(resolved);
                        break;
                    }
                }
            }

            if (rec.file_size_bytes == 0) {
                std::filesystem::recursive_directory_iterator rIt(entry.path(), dirOpts, itemEc);
                if (!itemEc) {
                    for (const auto &f : rIt) {
                        if (cancelFlag.load(std::memory_order_relaxed)) {
                            break;
                        }
                        std::error_code fEc;
                        if (f.is_regular_file(fEc)) {
                            uint64_t sz = f.file_size(fEc);
                            if (!fEc) {
                                rec.file_size_bytes += sz;
                            }
                        }
                    }
                }
            }

            if (rec.time_updated == 0) {
                auto ftime = std::filesystem::last_write_time(entry.path(), itemEc);
                if (!itemEc) {
                    auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
                    auto secs =
                        std::chrono::duration_cast<std::chrono::seconds>(sysTime.time_since_epoch())
                            .count();
                    if (secs > 0) {
                        rec.time_updated = static_cast<uint64_t>(secs);
                    }
                }
            }

            results.push_back(std::move(rec));
        }
    }

    return results;
}

constexpr const char *STYLE_PANEL_BG = "background-color: #14171c; color: #f1f5f9;";
constexpr const char *STYLE_DRAWER_BG =
    "background-color: #1a1e26; border-left: 1px solid #2b303c;";
constexpr const char *STYLE_TOOLBAR_COMBO =
    "QComboBox { background-color: #1e222b; color: #f1f5f9; border: 1px solid #334155; "
    "border-radius: 6px; padding: 4px 10px; min-height: 24px; font-size: 12px; }"
    "QComboBox::drop-down { border: none; width: 20px; }"
    "QComboBox QAbstractItemView { background-color: #1e222b; color: #f1f5f9; "
    "selection-background-color: #0284c7; selection-color: #ffffff; border: 1px solid #334155; }";

constexpr const char *STYLE_SEARCH_BAR =
    "QLineEdit { background-color: #1e222b; color: #f1f5f9; border: 1px solid #334155; "
    "border-radius: 6px; padding: 4px 10px; font-size: 12px; }"
    "QLineEdit:focus { border: 1px solid #38bdf8; }";

constexpr const char *STYLE_BUTTON_PRIMARY =
    "QPushButton { background-color: #0284c7; color: #ffffff; border: none; "
    "border-radius: 6px; padding: 6px 14px; font-weight: bold; font-size: 12px; }"
    "QPushButton:hover { background-color: #0369a1; }"
    "QPushButton:disabled { background-color: #334155; color: #64748b; }";

constexpr const char *STYLE_BUTTON_SECONDARY =
    "QPushButton { background-color: #2b303c; color: #e2e8f0; border: 1px solid #334155; "
    "border-radius: 6px; padding: 6px 12px; font-size: 12px; }"
    "QPushButton:hover { background-color: #384252; color: #ffffff; }"
    "QPushButton:disabled { background-color: #1e222b; color: #64748b; }";

} // namespace

struct WorkshopBrowserWidget::Impl {
    WorkshopBrowserWidget *owner{nullptr};

    std::vector<WorkshopRecord> allRecords;
    std::vector<WorkshopRecord> filteredRecords;
    std::unordered_map<uint64_t, size_t> idToIndex;
    std::unordered_map<uint64_t, size_t> allIdToIndex;

    std::thread scanThread;
    std::atomic<bool> scanCancel{false};
    uint64_t scanGeneration{0};

    std::thread compatScanThread;
    uint64_t compatScanGeneration{0};
    std::unordered_map<uint64_t, WorkshopRecord> compatDetailsCache;
    std::unordered_map<uint64_t, uint32_t> activeDownloadFlags;

    std::mutex dbWriteMutex;
    std::condition_variable dbWriteCv;
    std::queue<WorkshopItemRecord> dbWriteQueue;
    bool dbWriterStopping{false};
    std::thread dbWriteThread;

    QString activeQuery;
    int activeCategoryIndex{0};
    int activeTierIndex{0};
    int activeSortIndex{0};

    QTimer *hoverDwellTimer{nullptr};
    int pendingHoverIndex{-1};
    int activeHoverIndex{-1};

    QTimer *searchDebounceTimer{nullptr};

    int selectedItemIndex{-1};
    uint64_t selectedPublishedFileId{0};

    QSplitter *mainSplitter{nullptr};
    QComboBox *tierFilterCombo{nullptr};
    QLabel *statusTelemetryLabel{nullptr};

    QWidget *inspectorDrawer{nullptr};
    QScrollArea *inspectorScrollArea{nullptr};
    QLabel *inspectorTitleLabel{nullptr};
    QLabel *inspectorIdLabel{nullptr};
    QLabel *tierBadgeLabel{nullptr};
    QLabel *compatScoreLabel{nullptr};
    QProgressBar *compatScoreBar{nullptr};
    QLabel *vramFpsMetricsLabel{nullptr};
    QLabel *issuesHeaderLabel{nullptr};
    QListWidget *issuesListWidget{nullptr};
    QLabel *repairsHeaderLabel{nullptr};
    QListWidget *repairsListWidget{nullptr};
    QPushButton *runAutoRepairBtn{nullptr};
    QLabel *installStatusBadge{nullptr};

    QWidget *downloadProgressBox{nullptr};
    QProgressBar *downloadProgressBar{nullptr};
    QLabel *downloadTelemetryLabel{nullptr};

    QPushButton *subscribeDownloadBtn{nullptr};
    QComboBox *monitorTargetCombo{nullptr};
    QPushButton *applyToMonitorBtn{nullptr};

    void startDbWriter();
    void stopDbWriter();
    void enqueueAsyncUpsert(WorkshopItemRecord item);
    void stopScanThread();
    void stopCompatThread();
    void requestAsyncCompatDetails(uint64_t pubId, const std::string &dirPath,
                                   we_compat_tier_t prevTier, uint32_t prevScore);
    void mergeDiscoveredRecords(std::vector<WorkshopRecord> records);
    void launchAsyncScan();
    void rebuildAllIndex();
    void executeQuery();
    void applySorting();
    void syncWithGridView();
    void updateInspector(const WorkshopRecord &rec);
    void handleDownloadProgress(const DownloadProgress &prog);
    void applyThemeStyles();
};

void WorkshopBrowserWidget::Impl::startDbWriter()
{
    if (!owner->m_indexMgr || !owner->m_indexMgr->isOpen() || dbWriteThread.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(dbWriteMutex);
        dbWriterStopping = false;
    }
    std::shared_ptr<CatalogIndexManager> indexMgr = owner->m_indexMgr;
    dbWriteThread = std::thread([this, indexMgr]() {
        while (true) {
            std::vector<WorkshopItemRecord> batch;
            {
                std::unique_lock<std::mutex> lk(dbWriteMutex);
                dbWriteCv.wait(lk, [this]() { return dbWriterStopping || !dbWriteQueue.empty(); });
                if (dbWriterStopping && dbWriteQueue.empty()) {
                    break;
                }
                while (!dbWriteQueue.empty()) {
                    batch.push_back(std::move(dbWriteQueue.front()));
                    dbWriteQueue.pop();
                }
            }
            if (!batch.empty() && indexMgr && indexMgr->isOpen()) {
                (void)indexMgr->upsertItems(batch);
            }
        }
    });
}

void WorkshopBrowserWidget::Impl::stopDbWriter()
{
    {
        std::lock_guard<std::mutex> lk(dbWriteMutex);
        dbWriterStopping = true;
    }
    dbWriteCv.notify_all();
    if (dbWriteThread.joinable()) {
        dbWriteThread.join();
    }
}

void WorkshopBrowserWidget::Impl::enqueueAsyncUpsert(WorkshopItemRecord item)
{
    if (!owner->m_indexMgr || !owner->m_indexMgr->isOpen() || !dbWriteThread.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(dbWriteMutex);
        dbWriteQueue.push(std::move(item));
    }
    dbWriteCv.notify_one();
}

void WorkshopBrowserWidget::Impl::stopScanThread()
{
    scanCancel.store(true, std::memory_order_relaxed);
    if (scanThread.joinable()) {
        scanThread.join();
    }
}

void WorkshopBrowserWidget::Impl::stopCompatThread()
{
    ++compatScanGeneration;
    if (compatScanThread.joinable()) {
        compatScanThread.join();
    }
}

void WorkshopBrowserWidget::Impl::requestAsyncCompatDetails(uint64_t pubId,
                                                            const std::string &dirPath,
                                                            we_compat_tier_t prevTier,
                                                            uint32_t prevScore)
{
    stopCompatThread();
    const uint64_t gen = ++compatScanGeneration;
    QPointer<WorkshopBrowserWidget> self(owner);

    compatScanThread = std::thread([self, gen, pubId, dirPath, prevTier, prevScore]() {
        std::error_code ec;
        std::filesystem::path fsDir(dirPath);
        if (!std::filesystem::is_directory(fsDir, ec) || !self) {
            return;
        }

        WorkshopRecord scanned{};
        scanned.published_file_id = pubId;
        scanned.compat_tier = prevTier;
        scanned.compat_score = prevScore;
        scanned.compat_details_loaded = true;

        CompatScannerPtr scanner(we_compat_scanner_create(true, true), &we_compat_scanner_destroy);
        CompatReportPtr rep(we_compat_report_alloc(), &we_compat_report_free);
        if (scanner && rep &&
            we_compat_scan_directory(scanner.get(), fsDir.string().c_str(), rep.get())) {
            populateIssuesFromReport(rep.get(), scanned);
        }

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(
            self,
            [self, gen, scanned = std::move(scanned), prevTier, prevScore]() mutable {
                if (!self || gen != self->m_impl->compatScanGeneration) {
                    return;
                }
                auto &impl = *self->m_impl;
                if (impl.compatDetailsCache.size() >= 256) {
                    impl.compatDetailsCache.clear();
                }
                impl.compatDetailsCache[scanned.published_file_id] = scanned;

                auto filtIt = impl.idToIndex.find(scanned.published_file_id);
                if (filtIt != impl.idToIndex.end()) {
                    auto &rec = impl.filteredRecords[filtIt->second];
                    rec.compat_details_loaded = true;
                    rec.compat_tier = scanned.compat_tier;
                    rec.compat_score = scanned.compat_score;
                    rec.estimated_vram_mb = scanned.estimated_vram_mb;
                    rec.estimated_fps = scanned.estimated_fps;
                    rec.issue_messages = scanned.issue_messages;
                    rec.repair_descriptions = scanned.repair_descriptions;
                    if (scanned.compat_tier != prevTier || scanned.compat_score != prevScore) {
                        impl.enqueueAsyncUpsert(toCatalogRecord(rec));
                    }
                    if (impl.selectedPublishedFileId == scanned.published_file_id) {
                        impl.updateInspector(rec);
                    }
                }
            },
            Qt::QueuedConnection);
    });
}

void WorkshopBrowserWidget::Impl::rebuildAllIndex()
{
    allIdToIndex.clear();
    allIdToIndex.reserve(allRecords.size());
    for (size_t i = 0; i < allRecords.size(); ++i) {
        allIdToIndex[allRecords[i].published_file_id] = i;
    }
}

void WorkshopBrowserWidget::Impl::mergeDiscoveredRecords(std::vector<WorkshopRecord> records)
{
    for (auto &incoming : records) {
        auto oldIt = allIdToIndex.find(incoming.published_file_id);
        if (oldIt == allIdToIndex.end()) {
            continue;
        }
        const auto &prev = allRecords[oldIt->second];
        if (prev.install_state & WAYWAL_UGC_STATE_DOWNLOADING) {
            incoming.install_state |= WAYWAL_UGC_STATE_DOWNLOADING;
        }
        if (prev.compat_details_loaded && prev.time_updated == incoming.time_updated &&
            prev.file_path == incoming.file_path) {
            incoming.compat_details_loaded = true;
            incoming.compat_tier = prev.compat_tier;
            incoming.compat_score = prev.compat_score;
            incoming.estimated_vram_mb = prev.estimated_vram_mb;
            incoming.estimated_fps = prev.estimated_fps;
            incoming.issue_messages = prev.issue_messages;
            incoming.repair_descriptions = prev.repair_descriptions;
        }
    }

    if (!records.empty() || allRecords.empty()) {
        allRecords = std::move(records);
        rebuildAllIndex();
    }
    executeQuery();
}

void WorkshopBrowserWidget::Impl::launchAsyncScan()
{
    stopScanThread();
    scanCancel.store(false, std::memory_order_relaxed);
    const uint64_t gen = ++scanGeneration;

    QPointer<WorkshopBrowserWidget> self(owner);
    std::shared_ptr<CatalogIndexManager> indexMgr = owner->m_indexMgr;
    if (indexMgr && indexMgr->isOpen()) {
        // Drop duplicate 50k in-memory cache; query SQLite WAL + RCU pool directly on demand.
        allRecords.clear();
        allRecords.shrink_to_fit();
        allIdToIndex.clear();
        executeQuery();
    }

    scanThread = std::thread([this, self, gen, indexMgr]() {
        if (indexMgr && indexMgr->isOpen()) {
            for (const auto &root : collectWorkshopRoots()) {
                if (scanCancel.load(std::memory_order_relaxed)) {
                    return;
                }
                std::error_code ec;
                if (std::filesystem::exists(root, ec) && std::filesystem::is_directory(root, ec)) {
                    (void)indexMgr->reconcileDirectory(root, 0);
                }
            }

            if (scanCancel.load(std::memory_order_relaxed) || !self) {
                return;
            }
            QMetaObject::invokeMethod(
                self,
                [self, gen]() {
                    if (!self || gen != self->m_impl->scanGeneration) {
                        return;
                    }
                    self->m_impl->executeQuery();
                },
                Qt::QueuedConnection);
            return;
        }

        auto discovered = scanWorkshopRootsWorker(scanCancel);
        if (scanCancel.load(std::memory_order_relaxed) || !self) {
            return;
        }

        QMetaObject::invokeMethod(
            self,
            [self, gen, records = std::move(discovered)]() mutable {
                if (!self || gen != self->m_impl->scanGeneration) {
                    return;
                }
                self->m_impl->mergeDiscoveredRecords(std::move(records));
            },
            Qt::QueuedConnection);
    });
}

void WorkshopBrowserWidget::Impl::executeQuery()
{
    filteredRecords.clear();
    idToIndex.clear();

    QString targetType;
    switch (activeCategoryIndex) {
    case 1:
        targetType = QStringLiteral("scene");
        break;
    case 2:
        targetType = QStringLiteral("video");
        break;
    case 3:
        targetType = QStringLiteral("shader");
        break;
    case 4:
        targetType = QStringLiteral("web");
        break;
    default:
        break;
    }

    we_compat_tier_t targetTier = WE_TIER_UNKNOWN;
    switch (activeTierIndex) {
    case 1:
        targetTier = WE_TIER_1_NATIVE;
        break;
    case 2:
        targetTier = WE_TIER_2_TRANSPILED;
        break;
    case 3:
        targetTier = WE_TIER_3_DEGRADED;
        break;
    case 4:
        targetTier = WE_TIER_4_BLOCKED;
        break;
    default:
        break;
    }

    const QString trimmedQuery = activeQuery.trimmed();
    if (owner->m_indexMgr && owner->m_indexMgr->isOpen()) {
        std::vector<WorkshopItemRecord> dbRows;
        if (trimmedQuery.isEmpty()) {
            CatalogFilter filter{};
            filter.type = targetType.toStdString();
            filter.compat_tier = static_cast<uint32_t>(targetTier);
            switch (activeSortIndex) {
            case 0:
                filter.sort_order = CatalogSortOrder::SubscriptionsDesc;
                break;
            case 1:
                filter.sort_order = CatalogSortOrder::RatingDesc;
                break;
            case 2:
                filter.sort_order = CatalogSortOrder::RecentlyUpdated;
                break;
            case 3:
                filter.sort_order = CatalogSortOrder::CompatibilityScoreDesc;
                break;
            case 4:
                filter.sort_order = CatalogSortOrder::TitleAsc;
                break;
            default:
                filter.sort_order = CatalogSortOrder::SubscriptionsDesc;
                break;
            }
            filter.limit = 2000;
            filter.offset = 0;
            dbRows = owner->m_indexMgr->queryByFilters(filter);
        } else {
            bool isExactId = false;
            const uint64_t pubId = trimmedQuery.toULongLong(&isExactId);
            if (isExactId && pubId != 0) {
                if (auto exact = owner->m_indexMgr->getItem(pubId)) {
                    dbRows.push_back(std::move(*exact));
                }
            }
            auto searched = owner->m_indexMgr->searchFts5Bm25(trimmedQuery.toStdString(), 500, 0);
            for (auto &r : searched) {
                if (!isExactId || r.published_file_id != pubId) {
                    dbRows.push_back(std::move(r));
                }
            }
        }

        filteredRecords.reserve(dbRows.size());
        for (const auto &row : dbRows) {
            if (row.storage_backend != 0 && row.storage_backend != 1) {
                continue;
            }
            WorkshopRecord rec = fromCatalogRecord(row);
            if (!targetType.isEmpty() && rec.type != targetType) {
                continue;
            }
            if (targetTier != WE_TIER_UNKNOWN && rec.compat_tier != targetTier) {
                continue;
            }
            if (auto dlIt = activeDownloadFlags.find(rec.published_file_id);
                dlIt != activeDownloadFlags.end()) {
                rec.install_state |= dlIt->second;
            }
            if (auto cIt = compatDetailsCache.find(rec.published_file_id);
                cIt != compatDetailsCache.end()) {
                rec.compat_details_loaded = true;
                rec.estimated_vram_mb = cIt->second.estimated_vram_mb;
                rec.estimated_fps = cIt->second.estimated_fps;
                rec.issue_messages = cIt->second.issue_messages;
                rec.repair_descriptions = cIt->second.repair_descriptions;
            }
            filteredRecords.push_back(std::move(rec));
        }

        if (trimmedQuery.isEmpty()) {
            idToIndex.clear();
            selectedItemIndex = -1;
            for (size_t i = 0; i < filteredRecords.size(); ++i) {
                uint64_t id = filteredRecords[i].published_file_id;
                idToIndex[id] = i;
                if (selectedPublishedFileId != 0 && id == selectedPublishedFileId) {
                    selectedItemIndex = static_cast<int>(i);
                }
            }
        } else {
            applySorting();
        }
        syncWithGridView();
        return;
    }

    const QString queryLower = trimmedQuery.toLower();
    const QStringList queryTokens = queryLower.split(QChar(' '), Qt::SkipEmptyParts);

    for (const auto &rec : allRecords) {
        if (!targetType.isEmpty() && rec.type != targetType) {
            continue;
        }
        if (targetTier != WE_TIER_UNKNOWN && rec.compat_tier != targetTier) {
            continue;
        }

        if (!queryTokens.isEmpty()) {
            const QString idStr = QString::number(rec.published_file_id);
            const QString titleLow = rec.title.toLower();
            const QString tagsLow = rec.tags_string.toLower();
            const QString descLow = rec.description.toLower();
            bool allTokensMatch = true;
            for (const QString &tok : queryTokens) {
                if (!titleLow.contains(tok) && !idStr.contains(tok) && !rec.type.contains(tok) &&
                    !tagsLow.contains(tok) && !descLow.contains(tok)) {
                    allTokensMatch = false;
                    break;
                }
            }
            if (!allTokensMatch) {
                continue;
            }
        }

        filteredRecords.push_back(rec);
    }

    applySorting();
    syncWithGridView();
}

void WorkshopBrowserWidget::Impl::applySorting()
{
    switch (activeSortIndex) {
    case 0:
        std::sort(filteredRecords.begin(), filteredRecords.end(),
                  [](const WorkshopRecord &a, const WorkshopRecord &b) {
                      if (a.subscriptions != b.subscriptions) {
                          return a.subscriptions > b.subscriptions;
                      }
                      return a.time_updated > b.time_updated;
                  });
        break;
    case 1:
        std::sort(filteredRecords.begin(), filteredRecords.end(),
                  [](const WorkshopRecord &a, const WorkshopRecord &b) {
                      if (std::abs(a.rating - b.rating) > 1e-4f) {
                          return a.rating > b.rating;
                      }
                      return a.time_updated > b.time_updated;
                  });
        break;
    case 2:
        std::sort(filteredRecords.begin(), filteredRecords.end(),
                  [](const WorkshopRecord &a, const WorkshopRecord &b) {
                      return a.time_updated > b.time_updated;
                  });
        break;
    case 3:
        std::sort(filteredRecords.begin(), filteredRecords.end(),
                  [](const WorkshopRecord &a, const WorkshopRecord &b) {
                      if (a.compat_tier != b.compat_tier) {
                          return a.compat_tier < b.compat_tier;
                      }
                      return a.compat_score > b.compat_score;
                  });
        break;
    case 4:
        std::sort(filteredRecords.begin(), filteredRecords.end(),
                  [](const WorkshopRecord &a, const WorkshopRecord &b) {
                      return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
                  });
        break;
    default:
        break;
    }

    idToIndex.clear();
    selectedItemIndex = -1;
    for (size_t i = 0; i < filteredRecords.size(); ++i) {
        uint64_t id = filteredRecords[i].published_file_id;
        idToIndex[id] = i;
        if (selectedPublishedFileId != 0 && id == selectedPublishedFileId) {
            selectedItemIndex = static_cast<int>(i);
        }
    }
}

void WorkshopBrowserWidget::Impl::syncWithGridView()
{
    if (!owner->m_gridView) {
        return;
    }

    auto &catIndex = owner->m_gridView->catalogIndex();
    catIndex.clear();

    for (const auto &rec : filteredRecords) {
        CatalogItem ci;
        ci.id = static_cast<uint32_t>(rec.published_file_id ^ (rec.published_file_id >> 32));
        ci.workshop_id = std::to_string(rec.published_file_id);
        ci.title = rec.title.toStdString();
        ci.path = rec.file_path.toStdString();
        ci.preview_path = rec.preview_path.toStdString();
        ci.aspect_ratio = rec.aspect_ratio;
        ci.dominant_color = rec.dominant_color;
        ci.file_size = rec.file_size_bytes;
        ci.date_modified = static_cast<int64_t>(rec.time_updated);
        ci.tag_mask = rec.tag_mask;

        if (rec.type == QStringLiteral("video")) {
            ci.badge = WallpaperBadge::Video;
        } else if (rec.type == QStringLiteral("shader")) {
            ci.badge = WallpaperBadge::Shader;
        } else if (rec.type == QStringLiteral("scene")) {
            ci.badge = WallpaperBadge::Scene;
        } else {
            ci.badge = WallpaperBadge::Image;
        }

        catIndex.addItem(std::move(ci));
    }

    if (statusTelemetryLabel) {
        const size_t totalCount =
            (owner->m_indexMgr && owner->m_indexMgr->isOpen())
                ? std::max(owner->m_indexMgr->totalItemCount(), filteredRecords.size())
                : allRecords.size();
        statusTelemetryLabel->setText(
            QString("%1 of %2 items  |  Steam: %3")
                .arg(filteredRecords.size())
                .arg(totalCount)
                .arg((owner->m_downloadMgr && owner->m_downloadMgr->isSteamClientRunning())
                         ? "<font color='#34d399'>Connected</font>"
                         : "<font color='#94a3b8'>Web API</font>"));
    }

    catIndex.refilter();
    owner->m_gridView->refreshCatalogView();
}

void WorkshopBrowserWidget::Impl::updateInspector(const WorkshopRecord &inRec)
{
    if (!inspectorDrawer) {
        return;
    }

    const WorkshopRecord &rec = inRec;
    if (!rec.compat_details_loaded && !rec.file_path.isEmpty()) {
        auto filtIt = idToIndex.find(rec.published_file_id);
        if (filtIt != idToIndex.end()) {
            filteredRecords[filtIt->second].compat_details_loaded = true;
        }
        requestAsyncCompatDetails(rec.published_file_id, rec.file_path.toStdString(),
                                  rec.compat_tier, rec.compat_score);
    }

    inspectorTitleLabel->setText(rec.title);
    inspectorIdLabel->setText(QString("ID %1  |  %2  |  %3 subscribers")
                                  .arg(rec.published_file_id)
                                  .arg(formatBytes(rec.file_size_bytes))
                                  .arg(rec.subscriptions));

    QString tierText;
    QString tierStyle;
    switch (rec.compat_tier) {
    case WE_TIER_1_NATIVE:
        tierText = QStringLiteral("Native (Compatible)");
        tierStyle = "background-color: #064e3b; color: #34d399; border: 1px solid #059669; "
                    "border-radius: 5px; padding: 4px 10px; font-weight: 600; font-size: 11px;";
        break;
    case WE_TIER_2_TRANSPILED:
        tierText = QStringLiteral("Transpiled (Auto-Repaired)");
        tierStyle = "background-color: #0c4a6e; color: #38bdf8; border: 1px solid #0284c7; "
                    "border-radius: 5px; padding: 4px 10px; font-weight: 600; font-size: 11px;";
        break;
    case WE_TIER_3_DEGRADED:
        tierText = QStringLiteral("Partial Support");
        tierStyle = "background-color: #451a03; color: #fbbf24; border: 1px solid #d97706; "
                    "border-radius: 5px; padding: 4px 10px; font-weight: 600; font-size: 11px;";
        break;
    case WE_TIER_4_BLOCKED:
        tierText = QStringLiteral("Unsupported");
        tierStyle = "background-color: #450a0a; color: #f87171; border: 1px solid #dc2626; "
                    "border-radius: 5px; padding: 4px 10px; font-weight: 600; font-size: 11px;";
        break;
    default:
        tierText = QStringLiteral("Unknown Compatibility");
        tierStyle =
            "background-color: #20242e; color: #94a3b8; border-radius: 5px; padding: 4px 10px;";
        break;
    }
    tierBadgeLabel->setText(tierText);
    tierBadgeLabel->setStyleSheet(tierStyle);

    compatScoreLabel->setText(QString("Compatibility: %1%").arg(rec.compat_score));
    compatScoreBar->setValue(static_cast<int>(std::min(rec.compat_score, 100u)));
    if (rec.compat_score >= 90) {
        compatScoreBar->setStyleSheet(
            "QProgressBar::chunk { background-color: #10b981; border-radius: 3px; }");
    } else if (rec.compat_score >= 70) {
        compatScoreBar->setStyleSheet(
            "QProgressBar::chunk { background-color: #0ea5e9; border-radius: 3px; }");
    } else if (rec.compat_score >= 50) {
        compatScoreBar->setStyleSheet(
            "QProgressBar::chunk { background-color: #f59e0b; border-radius: 3px; }");
    } else {
        compatScoreBar->setStyleSheet(
            "QProgressBar::chunk { background-color: #ef4444; border-radius: 3px; }");
    }

    vramFpsMetricsLabel->setText(QString("VRAM: <b>%1 MB</b>  |  Target: <b>%2 FPS</b>")
                                     .arg(QString::number(rec.estimated_vram_mb, 'f', 1))
                                     .arg(rec.estimated_fps));

    issuesListWidget->clear();
    if (rec.issue_messages.empty()) {
        auto *item = new QListWidgetItem("No compatibility issues found.");
        item->setForeground(QColor(52, 211, 153));
        issuesListWidget->addItem(item);
    } else {
        for (const auto &msg : rec.issue_messages) {
            auto *item = new QListWidgetItem(msg);
            if (msg.contains("FATAL") || msg.contains("ERROR")) {
                item->setForeground(QColor(248, 113, 113));
            } else if (msg.contains("WARN")) {
                item->setForeground(QColor(251, 191, 36));
            } else {
                item->setForeground(QColor(148, 163, 184));
            }
            issuesListWidget->addItem(item);
        }
    }

    repairsListWidget->clear();
    if (rec.repair_descriptions.empty()) {
        auto *item = new QListWidgetItem("No shader fixes needed.");
        item->setForeground(QColor(148, 163, 184));
        repairsListWidget->addItem(item);
        runAutoRepairBtn->setEnabled(false);
    } else {
        for (const auto &desc : rec.repair_descriptions) {
            auto *item = new QListWidgetItem(desc);
            item->setForeground(QColor(56, 189, 248));
            repairsListWidget->addItem(item);
        }
        runAutoRepairBtn->setEnabled(!rec.file_path.isEmpty());
    }

    installStatusBadge->setToolTip(rec.last_error_message);

    if (rec.install_state & WAYWAL_UGC_STATE_DOWNLOADING) {
        installStatusBadge->setText("<font color='#38bdf8'><b>Downloading...</b></font>");
        downloadProgressBox->setVisible(true);
        subscribeDownloadBtn->setText("Cancel Download");
        applyToMonitorBtn->setEnabled(false);
    } else if (rec.last_download_failed) {
        installStatusBadge->setText("<font color='#f87171'><b>Download Failed</b></font>");
        downloadProgressBox->setVisible(false);
        subscribeDownloadBtn->setText("Retry Download");
        applyToMonitorBtn->setEnabled((rec.install_state & WAYWAL_UGC_STATE_INSTALLED) != 0 &&
                                      rec.compat_tier != WE_TIER_4_BLOCKED);
    } else if (rec.last_download_cancelled) {
        installStatusBadge->setText("<font color='#94a3b8'>Cancelled</font>");
        downloadProgressBox->setVisible(false);
        subscribeDownloadBtn->setText((rec.install_state & WAYWAL_UGC_STATE_INSTALLED)
                                          ? "Reinstall"
                                          : "Subscribe & Download");
        applyToMonitorBtn->setEnabled((rec.install_state & WAYWAL_UGC_STATE_INSTALLED) != 0 &&
                                      rec.compat_tier != WE_TIER_4_BLOCKED);
    } else if (rec.install_state & WAYWAL_UGC_STATE_INSTALLED) {
        installStatusBadge->setText("<font color='#34d399'><b>Installed</b></font>");
        downloadProgressBox->setVisible(false);
        subscribeDownloadBtn->setText("Reinstall");
        applyToMonitorBtn->setEnabled(rec.compat_tier != WE_TIER_4_BLOCKED);
    } else if (rec.install_state & WAYWAL_UGC_STATE_NEEDS_UPDATE) {
        installStatusBadge->setText("<font color='#fbbf24'><b>Update Available</b></font>");
        downloadProgressBox->setVisible(false);
        subscribeDownloadBtn->setText("Update");
        applyToMonitorBtn->setEnabled(rec.compat_tier != WE_TIER_4_BLOCKED);
    } else if (rec.install_state & WAYWAL_UGC_STATE_SUBSCRIBED) {
        installStatusBadge->setText(
            "<font color='#38bdf8'><b>Subscribed (Not Downloaded)</b></font>");
        downloadProgressBox->setVisible(false);
        subscribeDownloadBtn->setText("Download");
        applyToMonitorBtn->setEnabled(false);
    } else {
        installStatusBadge->setText("<font color='#94a3b8'>Not Subscribed</font>");
        downloadProgressBox->setVisible(false);
        subscribeDownloadBtn->setText("Subscribe & Download");
        applyToMonitorBtn->setEnabled(false);
    }

    if (rec.compat_tier == WE_TIER_4_BLOCKED) {
        applyToMonitorBtn->setEnabled(false);
        subscribeDownloadBtn->setEnabled((rec.install_state & WAYWAL_UGC_STATE_DOWNLOADING) != 0);
    } else {
        subscribeDownloadBtn->setEnabled(true);
    }
}

void WorkshopBrowserWidget::Impl::handleDownloadProgress(const DownloadProgress &prog)
{
    const uint64_t id = prog.published_file_id;
    if (id == 0) {
        return;
    }

    auto applyState = [&](WorkshopRecord &rec) {
        if (prog.bytes_total > 0) {
            rec.file_size_bytes = prog.bytes_total;
        }
        switch (prog.state) {
        case TaskState::Queued:
        case TaskState::Probing:
        case TaskState::Downloading:
        case TaskState::Extracting:
        case TaskState::Verifying:
        case TaskState::Indexing:
            rec.install_state |= WAYWAL_UGC_STATE_DOWNLOADING;
            rec.last_download_failed = false;
            rec.last_download_cancelled = false;
            rec.last_error_message.clear();
            break;
        case TaskState::Installed:
            rec.install_state &= ~static_cast<uint32_t>(WAYWAL_UGC_STATE_DOWNLOADING);
            rec.install_state |= (WAYWAL_UGC_STATE_INSTALLED | WAYWAL_UGC_STATE_SUBSCRIBED);
            rec.last_download_failed = false;
            rec.last_download_cancelled = false;
            rec.last_error_message.clear();
            rec.compat_details_loaded = false;
            if (!prog.extracted_dir.empty()) {
                rec.file_path = QString::fromStdString(prog.extracted_dir);
            } else if (!prog.installed_path.empty()) {
                rec.file_path = QString::fromStdString(prog.installed_path);
            }
            break;
        case TaskState::Failed:
            rec.install_state &= ~static_cast<uint32_t>(WAYWAL_UGC_STATE_DOWNLOADING);
            rec.last_download_failed = true;
            rec.last_download_cancelled = false;
            rec.last_error_message = QString::fromStdString(prog.error_message);
            break;
        case TaskState::Cancelled:
            rec.install_state &= ~static_cast<uint32_t>(WAYWAL_UGC_STATE_DOWNLOADING);
            rec.last_download_failed = false;
            rec.last_download_cancelled = true;
            rec.last_error_message.clear();
            break;
        }
    };

    switch (prog.state) {
    case TaskState::Queued:
    case TaskState::Probing:
    case TaskState::Downloading:
    case TaskState::Extracting:
    case TaskState::Verifying:
    case TaskState::Indexing:
        activeDownloadFlags[id] = WAYWAL_UGC_STATE_DOWNLOADING;
        break;
    case TaskState::Installed:
        activeDownloadFlags[id] = WAYWAL_UGC_STATE_INSTALLED | WAYWAL_UGC_STATE_SUBSCRIBED;
        compatDetailsCache.erase(id);
        break;
    case TaskState::Failed:
    case TaskState::Cancelled:
        activeDownloadFlags.erase(id);
        break;
    }

    auto allIt = allIdToIndex.find(id);
    if (allIt != allIdToIndex.end()) {
        applyState(allRecords[allIt->second]);
        if (prog.state == TaskState::Installed) {
            enqueueAsyncUpsert(toCatalogRecord(allRecords[allIt->second]));
        }
    }

    auto filtIt = idToIndex.find(id);
    if (filtIt == idToIndex.end()) {
        return;
    }

    auto &rec = filteredRecords[filtIt->second];
    applyState(rec);
    if (prog.state == TaskState::Installed && allIt == allIdToIndex.end()) {
        enqueueAsyncUpsert(toCatalogRecord(rec));
    }

    if (selectedItemIndex == static_cast<int>(filtIt->second)) {
        if (prog.state == TaskState::Downloading || prog.state == TaskState::Verifying ||
            prog.state == TaskState::Queued) {
            owner->onDownloadProgressUpdated(id, std::min(prog.progress_pct, 99.9f),
                                             prog.speed_bytes_per_sec);
        } else {
            downloadProgressBox->setVisible(false);
            updateInspector(rec);
        }
    }
}

void WorkshopBrowserWidget::Impl::applyThemeStyles() { owner->setStyleSheet(STYLE_PANEL_BG); }

WorkshopBrowserWidget::WorkshopBrowserWidget(std::shared_ptr<CatalogIndexManager> indexMgr,
                                             std::shared_ptr<DownloadManager> downloadMgr,
                                             QWidget *parent)
    : QWidget(parent), m_indexMgr(std::move(indexMgr)), m_downloadMgr(std::move(downloadMgr)),
      m_impl(std::make_unique<Impl>())
{
    m_impl->owner = this;

    setupUi();
    wireSignals();

    m_impl->startDbWriter();
    m_impl->executeQuery();
    m_impl->launchAsyncScan();
}

WorkshopBrowserWidget::~WorkshopBrowserWidget()
{
    m_impl->stopScanThread();
    m_impl->stopCompatThread();
    m_impl->stopDbWriter();
    if (m_impl->hoverDwellTimer) {
        m_impl->hoverDwellTimer->stop();
    }
    if (m_impl->searchDebounceTimer) {
        m_impl->searchDebounceTimer->stop();
    }
}

QSize WorkshopBrowserWidget::sizeHint() const { return QSize(360, 600); }

QSize WorkshopBrowserWidget::minimumSizeHint() const { return QSize(200, 200); }

void WorkshopBrowserWidget::setupUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *toolbarFrame = new QFrame(this);
    toolbarFrame->setStyleSheet("background-color: #181b22; border-bottom: 1px solid #262b36;");
    auto *toolbarOuter = new QVBoxLayout(toolbarFrame);
    toolbarOuter->setContentsMargins(10, 8, 10, 8);
    toolbarOuter->setSpacing(8);

    auto *searchRow = new QHBoxLayout();
    searchRow->setContentsMargins(0, 0, 0, 0);
    searchRow->setSpacing(8);

    m_searchEdit = new QLineEdit(toolbarFrame);
    m_searchEdit->setPlaceholderText("Search Workshop wallpapers...");
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setFixedHeight(28);
    m_searchEdit->setStyleSheet(STYLE_SEARCH_BAR);
    searchRow->addWidget(m_searchEdit, 1);

    m_refreshBtn = new QToolButton(toolbarFrame);
    m_refreshBtn->setText("Rescan");
    m_refreshBtn->setFixedHeight(28);
    m_refreshBtn->setToolTip("Rescan local Steam Workshop folders");
    m_refreshBtn->setStyleSheet(
        "QToolButton { background-color: #20242e; color: #e2e8f0; border: 1px solid #2e3545; "
        "border-radius: 5px; padding: 4px 12px; font-weight: 500; font-size: 11px; }"
        "QToolButton:hover { background-color: #2a303d; border-color: #38bdf8; color: #ffffff; }");
    searchRow->addWidget(m_refreshBtn);
    toolbarOuter->addLayout(searchRow);

    auto *filterRow = new QHBoxLayout();
    filterRow->setContentsMargins(0, 0, 0, 0);
    filterRow->setSpacing(8);

    m_typeCombo = new QComboBox(toolbarFrame);
    m_typeCombo->setFixedHeight(26);
    m_typeCombo->addItems({"All Types", "Scene", "Video", "Shader", "Web"});
    m_typeCombo->setStyleSheet(STYLE_TOOLBAR_COMBO);
    filterRow->addWidget(m_typeCombo, 1);

    m_impl->tierFilterCombo = new QComboBox(toolbarFrame);
    m_impl->tierFilterCombo->setFixedHeight(26);
    m_impl->tierFilterCombo->addItems(
        {"All Tiers", "Native", "Transpiled", "Partial", "Unsupported"});
    m_impl->tierFilterCombo->setStyleSheet(STYLE_TOOLBAR_COMBO);
    filterRow->addWidget(m_impl->tierFilterCombo, 1);

    m_sortCombo = new QComboBox(toolbarFrame);
    m_sortCombo->setFixedHeight(26);
    m_sortCombo->addItems({"Subscribers", "Rating", "Recent", "Compatibility", "Title"});
    m_sortCombo->setStyleSheet(STYLE_TOOLBAR_COMBO);
    filterRow->addWidget(m_sortCombo, 1);
    toolbarOuter->addLayout(filterRow);

    rootLayout->addWidget(toolbarFrame);

    m_impl->mainSplitter = new QSplitter(Qt::Vertical, this);
    m_impl->mainSplitter->setStyleSheet(
        "QSplitter::handle { background-color: #262b36; height: 2px; }");

    m_gridView = new VirtualizedGridWidget(m_impl->mainSplitter);
    m_gridView->setEmbeddedMode(true);
    m_impl->mainSplitter->addWidget(m_gridView);

    m_impl->inspectorScrollArea = new QScrollArea(m_impl->mainSplitter);
    m_impl->inspectorScrollArea->setWidgetResizable(true);
    m_impl->inspectorScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_impl->inspectorScrollArea->setStyleSheet(STYLE_DRAWER_BG);

    m_impl->inspectorDrawer = new QWidget(m_impl->inspectorScrollArea);
    m_impl->inspectorDrawer->setMinimumWidth(0);
    auto *drawerLayout = new QVBoxLayout(m_impl->inspectorDrawer);
    drawerLayout->setContentsMargins(12, 12, 12, 12);
    drawerLayout->setSpacing(10);

    m_impl->inspectorTitleLabel =
        new QLabel("Select a wallpaper to view details", m_impl->inspectorDrawer);
    m_impl->inspectorTitleLabel->setTextFormat(Qt::PlainText);
    m_impl->inspectorTitleLabel->setStyleSheet(
        "font-size: 13px; font-weight: 600; color: #f1f5f9;");
    m_impl->inspectorTitleLabel->setWordWrap(true);
    m_impl->inspectorTitleLabel->setMinimumWidth(0);
    drawerLayout->addWidget(m_impl->inspectorTitleLabel);

    m_impl->inspectorIdLabel = new QLabel("No item selected", m_impl->inspectorDrawer);
    m_impl->inspectorIdLabel->setTextFormat(Qt::PlainText);
    m_impl->inspectorIdLabel->setStyleSheet("font-size: 11px; color: #94a3b8;");
    m_impl->inspectorIdLabel->setWordWrap(true);
    m_impl->inspectorIdLabel->setMinimumWidth(0);
    drawerLayout->addWidget(m_impl->inspectorIdLabel);

    m_impl->tierBadgeLabel = new QLabel("Native (Compatible)", m_impl->inspectorDrawer);
    m_impl->tierBadgeLabel->setTextFormat(Qt::PlainText);
    m_impl->tierBadgeLabel->setAlignment(Qt::AlignCenter);
    drawerLayout->addWidget(m_impl->tierBadgeLabel);

    auto *scoreBox = new QHBoxLayout();
    scoreBox->setSpacing(8);
    m_impl->compatScoreLabel = new QLabel("Compatibility: 100%", m_impl->inspectorDrawer);
    m_impl->compatScoreLabel->setTextFormat(Qt::PlainText);
    m_impl->compatScoreLabel->setStyleSheet("font-size: 11px; font-weight: 600; color: #e2e8f0;");
    scoreBox->addWidget(m_impl->compatScoreLabel);

    m_impl->compatScoreBar = new QProgressBar(m_impl->inspectorDrawer);
    m_impl->compatScoreBar->setRange(0, 100);
    m_impl->compatScoreBar->setValue(100);
    m_impl->compatScoreBar->setFixedHeight(8);
    m_impl->compatScoreBar->setTextVisible(false);
    m_impl->compatScoreBar->setStyleSheet(
        "QProgressBar { background: #20242e; border-radius: 4px; }");
    scoreBox->addWidget(m_impl->compatScoreBar, 1);
    drawerLayout->addLayout(scoreBox);

    m_impl->vramFpsMetricsLabel =
        new QLabel("VRAM: 64 MB  |  Target: 60 FPS", m_impl->inspectorDrawer);
    m_impl->vramFpsMetricsLabel->setStyleSheet(
        "font-size: 11px; color: #cbd5e1; background: #20242e; "
        "padding: 6px 10px; border-radius: 5px; border: 1px solid #2e3545;");
    drawerLayout->addWidget(m_impl->vramFpsMetricsLabel);

    m_impl->issuesHeaderLabel = new QLabel("<b>Compatibility Notes</b>", m_impl->inspectorDrawer);
    m_impl->issuesHeaderLabel->setStyleSheet("font-size: 11px; color: #94a3b8;");
    drawerLayout->addWidget(m_impl->issuesHeaderLabel);

    m_impl->issuesListWidget = new QListWidget(m_impl->inspectorDrawer);
    m_impl->issuesListWidget->setMaximumHeight(80);
    m_impl->issuesListWidget->setStyleSheet(
        "QListWidget { background: #14171f; border: 1px solid #2e3545; border-radius: 5px; "
        "font-size: 11px; padding: 4px; }");
    drawerLayout->addWidget(m_impl->issuesListWidget);

    m_impl->repairsHeaderLabel = new QLabel("<b>Suggested Fixes</b>", m_impl->inspectorDrawer);
    m_impl->repairsHeaderLabel->setStyleSheet("font-size: 11px; color: #94a3b8;");
    drawerLayout->addWidget(m_impl->repairsHeaderLabel);

    m_impl->repairsListWidget = new QListWidget(m_impl->inspectorDrawer);
    m_impl->repairsListWidget->setMaximumHeight(68);
    m_impl->repairsListWidget->setStyleSheet(
        "QListWidget { background: #14171f; border: 1px solid #2e3545; border-radius: 5px; "
        "font-size: 11px; padding: 4px; }");
    drawerLayout->addWidget(m_impl->repairsListWidget);

    m_impl->runAutoRepairBtn = new QPushButton("Repair Shader", m_impl->inspectorDrawer);
    m_impl->runAutoRepairBtn->setFixedHeight(28);
    m_impl->runAutoRepairBtn->setStyleSheet(STYLE_BUTTON_SECONDARY);
    m_impl->runAutoRepairBtn->setEnabled(false);
    drawerLayout->addWidget(m_impl->runAutoRepairBtn);

    auto *statusRow = new QHBoxLayout();
    statusRow->setSpacing(8);
    auto *statusTitle = new QLabel("Status:", m_impl->inspectorDrawer);
    statusTitle->setStyleSheet("font-size: 11px; color: #94a3b8;");
    statusRow->addWidget(statusTitle);
    m_impl->installStatusBadge = new QLabel("Not Subscribed", m_impl->inspectorDrawer);
    m_impl->installStatusBadge->setStyleSheet("font-size: 11px;");
    statusRow->addWidget(m_impl->installStatusBadge, 1);
    drawerLayout->addLayout(statusRow);

    m_impl->downloadProgressBox = new QWidget(m_impl->inspectorDrawer);
    auto *dlLayout = new QVBoxLayout(m_impl->downloadProgressBox);
    dlLayout->setContentsMargins(0, 0, 0, 0);
    dlLayout->setSpacing(6);

    m_impl->downloadProgressBar = new QProgressBar(m_impl->downloadProgressBox);
    m_impl->downloadProgressBar->setRange(0, 100);
    m_impl->downloadProgressBar->setValue(0);
    m_impl->downloadProgressBar->setFixedHeight(10);
    m_impl->downloadProgressBar->setStyleSheet(
        "QProgressBar { background: #20242e; border: 1px solid #2e3545; border-radius: 4px; "
        "text-align: center; font-size: 9px; color: white; }"
        "QProgressBar::chunk { background-color: #0284c7; border-radius: 3px; }");
    dlLayout->addWidget(m_impl->downloadProgressBar);

    m_impl->downloadTelemetryLabel = new QLabel("Downloading: 0%", m_impl->downloadProgressBox);
    m_impl->downloadTelemetryLabel->setTextFormat(Qt::PlainText);
    m_impl->downloadTelemetryLabel->setStyleSheet("font-size: 11px; color: #38bdf8;");
    dlLayout->addWidget(m_impl->downloadTelemetryLabel);

    m_impl->downloadProgressBox->setVisible(false);
    drawerLayout->addWidget(m_impl->downloadProgressBox);

    m_impl->subscribeDownloadBtn = new QPushButton("Subscribe & Download", m_impl->inspectorDrawer);
    m_impl->subscribeDownloadBtn->setFixedHeight(28);
    m_impl->subscribeDownloadBtn->setStyleSheet(STYLE_BUTTON_SECONDARY);
    drawerLayout->addWidget(m_impl->subscribeDownloadBtn);

    auto *applyRow = new QHBoxLayout();
    applyRow->setSpacing(8);
    m_impl->monitorTargetCombo = new QComboBox(m_impl->inspectorDrawer);
    m_impl->monitorTargetCombo->setFixedHeight(28);
    m_impl->monitorTargetCombo->addItems({"All Displays", "DP-1", "HDMI-A-1"});
    m_impl->monitorTargetCombo->setStyleSheet(STYLE_TOOLBAR_COMBO);
    applyRow->addWidget(m_impl->monitorTargetCombo, 1);

    m_impl->applyToMonitorBtn = new QPushButton("Apply", m_impl->inspectorDrawer);
    m_impl->applyToMonitorBtn->setFixedHeight(28);
    m_impl->applyToMonitorBtn->setStyleSheet(STYLE_BUTTON_PRIMARY);
    m_impl->applyToMonitorBtn->setEnabled(false);
    applyRow->addWidget(m_impl->applyToMonitorBtn, 1);
    drawerLayout->addLayout(applyRow);

    drawerLayout->addStretch();

    m_impl->inspectorScrollArea->setWidget(m_impl->inspectorDrawer);
    m_impl->mainSplitter->addWidget(m_impl->inspectorScrollArea);

    m_impl->mainSplitter->setStretchFactor(0, 60);
    m_impl->mainSplitter->setStretchFactor(1, 40);
    rootLayout->addWidget(m_impl->mainSplitter, 1);

    auto *bottomStrip = new QFrame(this);
    bottomStrip->setStyleSheet("background-color: #14171f; border-top: 1px solid #262b36;");
    auto *bottomLayout = new QHBoxLayout(bottomStrip);
    bottomLayout->setContentsMargins(10, 5, 10, 5);

    m_impl->statusTelemetryLabel = new QLabel("0 items", bottomStrip);
    m_impl->statusTelemetryLabel->setStyleSheet("font-size: 11px; color: #94a3b8;");
    bottomLayout->addWidget(m_impl->statusTelemetryLabel);
    bottomLayout->addStretch();

    rootLayout->addWidget(bottomStrip);

    m_impl->applyThemeStyles();
}

void WorkshopBrowserWidget::wireSignals()
{
    m_impl->searchDebounceTimer = new QTimer(this);
    m_impl->searchDebounceTimer->setSingleShot(true);
    m_impl->searchDebounceTimer->setInterval(100);

    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_impl->activeQuery = text;
        m_impl->searchDebounceTimer->start();
    });

    connect(m_impl->searchDebounceTimer, &QTimer::timeout, this,
            [this]() { setFilterQuery(m_impl->activeQuery); });

    connect(m_typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &WorkshopBrowserWidget::setCategoryFilter);

    connect(m_impl->tierFilterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
                m_impl->activeTierIndex = idx;
                m_impl->executeQuery();
            });

    connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &WorkshopBrowserWidget::setSortOrder);

    connect(m_refreshBtn, &QToolButton::clicked, this, &WorkshopBrowserWidget::refreshLibrary);

    m_impl->hoverDwellTimer = new QTimer(this);
    m_impl->hoverDwellTimer->setSingleShot(true);
    m_impl->hoverDwellTimer->setInterval(150);

    auto *canvas = m_gridView->findChild<GridGlCanvas *>();
    if (canvas) {
        connect(canvas, &GridGlCanvas::hoverChanged, this, [this](std::optional<size_t> opt_idx) {
            if (opt_idx.has_value()) {
                m_impl->pendingHoverIndex = static_cast<int>(*opt_idx);
                m_impl->hoverDwellTimer->start(150);
            } else {
                m_impl->hoverDwellTimer->stop();
                m_impl->pendingHoverIndex = -1;
            }
        });
    }

    connect(m_impl->hoverDwellTimer, &QTimer::timeout, this, [this]() {
        if (m_impl->pendingHoverIndex >= 0) {
            onItemHoverStarted(m_impl->pendingHoverIndex);
        }
    });

    connect(m_gridView, &VirtualizedGridWidget::wallpaperSelected, this,
            [this](const CatalogItem &item) {
                uint64_t pubId = std::strtoull(item.workshop_id.c_str(), nullptr, 10);
                auto it = m_impl->idToIndex.find(pubId);
                if (it != m_impl->idToIndex.end()) {
                    m_impl->selectedItemIndex = static_cast<int>(it->second);
                    m_impl->selectedPublishedFileId = pubId;
                    m_impl->updateInspector(m_impl->filteredRecords[it->second]);
                }
            });

    connect(m_gridView, &VirtualizedGridWidget::wallpaperDoubleClicked, this,
            [this](const CatalogItem &item) {
                uint64_t pubId = std::strtoull(item.workshop_id.c_str(), nullptr, 10);
                auto it = m_impl->idToIndex.find(pubId);
                if (it != m_impl->idToIndex.end()) {
                    onItemDoubleClicked(static_cast<int>(it->second));
                }
            });

    // Subscribe / Download / Cancel toggle
    connect(m_impl->subscribeDownloadBtn, &QPushButton::clicked, this, [this]() {
        if (m_impl->selectedItemIndex < 0 ||
            static_cast<size_t>(m_impl->selectedItemIndex) >= m_impl->filteredRecords.size()) {
            return;
        }

        auto &rec = m_impl->filteredRecords[m_impl->selectedItemIndex];
        const uint64_t pubId = rec.published_file_id;

        if (!m_downloadMgr) {
            return;
        }

        // Cancel active download instead of re-enqueuing
        if (rec.install_state & WAYWAL_UGC_STATE_DOWNLOADING) {
            m_downloadMgr->cancel(pubId);
            rec.install_state &= ~static_cast<uint32_t>(WAYWAL_UGC_STATE_DOWNLOADING);
            rec.last_download_cancelled = true;
            rec.last_download_failed = false;

            auto allIt = m_impl->allIdToIndex.find(pubId);
            if (allIt != m_impl->allIdToIndex.end()) {
                m_impl->allRecords[allIt->second].install_state = rec.install_state;
                m_impl->allRecords[allIt->second].last_download_cancelled = true;
                m_impl->allRecords[allIt->second].last_download_failed = false;
            }
            m_impl->updateInspector(rec);
            return;
        }

        rec.install_state |= WAYWAL_UGC_STATE_DOWNLOADING;
        rec.last_download_cancelled = false;
        rec.last_download_failed = false;
        rec.last_error_message.clear();

        auto allIt = m_impl->allIdToIndex.find(pubId);
        if (allIt != m_impl->allIdToIndex.end()) {
            m_impl->allRecords[allIt->second].install_state |= WAYWAL_UGC_STATE_DOWNLOADING;
            m_impl->allRecords[allIt->second].last_download_cancelled = false;
            m_impl->allRecords[allIt->second].last_download_failed = false;
        }
        m_impl->updateInspector(rec);

        QPointer<WorkshopBrowserWidget> self(this);
        m_downloadMgr->enqueue(pubId, TaskPriority::Interactive,
                               [self](const DownloadProgress &prog) {
                                   if (!self) {
                                       return;
                                   }
                                   QMetaObject::invokeMethod(
                                       self,
                                       [self, prog]() {
                                           if (self) {
                                               self->m_impl->handleDownloadProgress(prog);
                                           }
                                       },
                                       Qt::QueuedConnection);
                               });
    });

    // Real shader auto-repair pipeline via we_compat_scan_directory + we_compat_apply_auto_repairs
    connect(m_impl->runAutoRepairBtn, &QPushButton::clicked, this, [this]() {
        if (m_impl->selectedItemIndex < 0 ||
            static_cast<size_t>(m_impl->selectedItemIndex) >= m_impl->filteredRecords.size()) {
            return;
        }

        auto &rec = m_impl->filteredRecords[m_impl->selectedItemIndex];
        std::string srcDir = rec.file_path.toStdString();
        std::error_code ec;
        if (srcDir.empty() || !std::filesystem::is_directory(srcDir, ec)) {
            rec.issue_messages.push_back("[ERROR] Repair source directory is unavailable.");
            m_impl->updateInspector(rec);
            return;
        }

        CompatScannerPtr scanner(we_compat_scanner_create(true, true), &we_compat_scanner_destroy);
        CompatReportPtr preRep(we_compat_report_alloc(), &we_compat_report_free);
        CompatReportPtr postRep(we_compat_report_alloc(), &we_compat_report_free);
        if (!scanner || !preRep || !postRep) {
            return;
        }

        if (!we_compat_scan_directory(scanner.get(), srcDir.c_str(), preRep.get())) {
            rec.issue_messages.push_back("[ERROR] Compatibility scan failed on source wallpaper.");
            m_impl->updateInspector(rec);
            return;
        }

        QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
                            "/wwal/repaired";
        std::filesystem::path repairedDir =
            std::filesystem::path(cacheRoot.toStdString()) / std::to_string(rec.published_file_id);
        std::filesystem::create_directories(repairedDir, ec);

        if (!we_compat_apply_auto_repairs(preRep.get(), srcDir.c_str(),
                                          repairedDir.string().c_str())) {
            rec.issue_messages.push_back("[ERROR] Auto-repair failed to write output shaders.");
            m_impl->updateInspector(rec);
            return;
        }

        if (we_compat_scan_directory(scanner.get(), repairedDir.string().c_str(), postRep.get())) {
            rec.file_path = QString::fromStdString(repairedDir.string());
            populateIssuesFromReport(postRep.get(), rec);
            rec.compat_details_loaded = true;
        }

        auto allIt = m_impl->allIdToIndex.find(rec.published_file_id);
        if (allIt != m_impl->allIdToIndex.end()) {
            m_impl->allRecords[allIt->second] = rec;
            m_impl->enqueueAsyncUpsert(toCatalogRecord(rec));
        }

        m_impl->updateInspector(rec);
    });

    connect(m_impl->applyToMonitorBtn, &QPushButton::clicked, this, [this]() {
        if (m_impl->selectedItemIndex < 0 ||
            static_cast<size_t>(m_impl->selectedItemIndex) >= m_impl->filteredRecords.size()) {
            return;
        }

        const auto &rec = m_impl->filteredRecords[m_impl->selectedItemIndex];
        if ((rec.install_state & WAYWAL_UGC_STATE_INSTALLED) == 0 ||
            rec.compat_tier == WE_TIER_4_BLOCKED) {
            return;
        }

        QString target = m_impl->monitorTargetCombo->currentText();
        emit wallpaperSelected(rec.published_file_id, rec.file_path);
        emit wallpaperApplyRequested(rec.published_file_id, target);
    });
}

void WorkshopBrowserWidget::refreshLibrary() { m_impl->launchAsyncScan(); }

void WorkshopBrowserWidget::setFilterQuery(const QString &query)
{
    m_impl->activeQuery = query;
    m_impl->executeQuery();
}

void WorkshopBrowserWidget::setCategoryFilter(int categoryIndex)
{
    m_impl->activeCategoryIndex = categoryIndex;
    m_impl->executeQuery();
}

void WorkshopBrowserWidget::setSortOrder(int sortOrderIndex)
{
    m_impl->activeSortIndex = sortOrderIndex;
    m_impl->executeQuery();
}

void WorkshopBrowserWidget::onItemDoubleClicked(int itemIndex)
{
    if (itemIndex < 0 || static_cast<size_t>(itemIndex) >= m_impl->filteredRecords.size()) {
        return;
    }

    const auto &rec = m_impl->filteredRecords[itemIndex];
    m_impl->selectedItemIndex = itemIndex;
    m_impl->selectedPublishedFileId = rec.published_file_id;
    m_impl->updateInspector(rec);

    emit wallpaperSelected(rec.published_file_id, rec.file_path);

    if ((rec.install_state & WAYWAL_UGC_STATE_INSTALLED) != 0 &&
        rec.compat_tier != WE_TIER_4_BLOCKED) {
        emit wallpaperApplyRequested(rec.published_file_id,
                                     m_impl->monitorTargetCombo->currentText());
    }
}

void WorkshopBrowserWidget::onItemHoverStarted(int itemIndex)
{
    if (itemIndex < 0 || static_cast<size_t>(itemIndex) >= m_impl->filteredRecords.size()) {
        return;
    }

    m_impl->activeHoverIndex = itemIndex;
    const auto &rec = m_impl->filteredRecords[itemIndex];

    auto *canvas = m_gridView->findChild<GridGlCanvas *>();
    if (canvas && m_gridView) {
        m_gridView->hoverController().onCellHoverEntered(
            static_cast<uint32_t>(rec.published_file_id ^ (rec.published_file_id >> 32)),
            rec.file_path.toStdString(),
            (rec.type == QStringLiteral("video")    ? WallpaperBadge::Video
             : rec.type == QStringLiteral("shader") ? WallpaperBadge::Shader
             : rec.type == QStringLiteral("scene")  ? WallpaperBadge::Scene
                                                    : WallpaperBadge::Image),
            rec.preview_path.toStdString());
    }
}

void WorkshopBrowserWidget::onDownloadProgressUpdated(uint64_t publishedFileId, float progressPct,
                                                      float speedBps)
{
    double speedMiB = static_cast<double>(std::max(0.0f, speedBps)) / (1024.0 * 1024.0);
    float clampedPct = std::clamp(progressPct, 0.0f, 100.0f);

    auto allIt = m_impl->allIdToIndex.find(publishedFileId);
    if (allIt != m_impl->allIdToIndex.end()) {
        if (progressPct >= 100.0f) {
            m_impl->allRecords[allIt->second].install_state =
                WAYWAL_UGC_STATE_INSTALLED | WAYWAL_UGC_STATE_SUBSCRIBED;
        } else {
            m_impl->allRecords[allIt->second].install_state |= WAYWAL_UGC_STATE_DOWNLOADING;
        }
    }

    auto it = m_impl->idToIndex.find(publishedFileId);
    if (it == m_impl->idToIndex.end()) {
        if (m_impl->downloadTelemetryLabel && progressPct < 100.0f) {
            m_impl->downloadTelemetryLabel->setText(QString("Downloading: %1% (%2 MB/s)")
                                                        .arg(static_cast<int>(clampedPct))
                                                        .arg(QString::number(speedMiB, 'f', 1)));
        }
        return;
    }

    auto &rec = m_impl->filteredRecords[it->second];

    if (progressPct >= 100.0f) {
        rec.install_state = WAYWAL_UGC_STATE_INSTALLED | WAYWAL_UGC_STATE_SUBSCRIBED;
        if (m_impl->selectedItemIndex == static_cast<int>(it->second)) {
            m_impl->downloadProgressBox->setVisible(false);
            m_impl->updateInspector(rec);
        }
    } else {
        rec.install_state |= WAYWAL_UGC_STATE_DOWNLOADING;
        if (m_impl->selectedItemIndex == static_cast<int>(it->second)) {
            m_impl->downloadProgressBox->setVisible(true);
            m_impl->downloadProgressBar->setValue(static_cast<int>(clampedPct));

            uint64_t downloadedBytes = static_cast<uint64_t>(
                static_cast<double>(rec.file_size_bytes) * (clampedPct / 100.0));
            uint32_t etaSec = (speedBps > 1024.0f && rec.file_size_bytes > downloadedBytes)
                                  ? static_cast<uint32_t>((rec.file_size_bytes - downloadedBytes) /
                                                          static_cast<uint64_t>(speedBps))
                                  : 0;

            m_impl->downloadTelemetryLabel->setText(
                QString("Downloading: %1 / %2 (%3 MB/s, ETA: %4s)")
                    .arg(formatBytes(downloadedBytes))
                    .arg(formatBytes(rec.file_size_bytes))
                    .arg(QString::number(speedMiB, 'f', 1))
                    .arg(etaSec));
        }
    }
}

} // namespace wwal::studio
