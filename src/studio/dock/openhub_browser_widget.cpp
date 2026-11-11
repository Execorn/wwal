// SPDX-License-Identifier: GPL-3.0-or-later
#include "openhub_browser_widget.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
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
#include "wizard/creator_packaging_wizard.hpp"

#include <waywal/thumb_cache.h>
#include <waywal/wwpkg.h>

namespace wwal::studio {

namespace {

using WwpkgHandlePtr = std::unique_ptr<wwpkg_package_t, decltype(&wwpkg_close)>;

struct OpenHubRecord {
    QString id;
    QString slug;
    QString name;
    QString version;
    QString author_name;
    QString author_email;
    QString author_pubkey_hex;
    QString license;
    QString description;
    QStringList tags;
    wwpkg_type_t type{WWPKG_TYPE_SHADER_FRAGMENT};

    QString download_url;
    QString local_pkg_path;
    QString preview_path;
    QString cid;
    uint64_t file_size_bytes{0};
    uint64_t timestamp{0};
    uint32_t download_count{0};
    float rating{0.0f};

    bool is_verified{false};
    bool is_trusted{false};
    QString merkle_root_hex;
    QString signature_hex;

    bool cap_compute{false};
    bool cap_audio_spectrum{false};
    bool cap_audio_playback{false};
    bool cap_scripting{false};
    bool cap_foreign_toplevel{false};
    bool cap_network{false};
    bool cap_filesystem{false};

    uint32_t max_vram_mb{128};
    uint32_t max_ram_mb{256};
    uint32_t target_fps{60};

    bool is_installed{false};
    bool is_downloading{false};
    bool last_download_failed{false};
    bool last_download_cancelled{false};
    QString last_error_message;
    float download_pct{0.0f};
    float download_speed_bps{0.0f};
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

QString bytesToHex(const uint8_t *data, size_t len)
{
    QString hex;
    hex.reserve(static_cast<qsizetype>(len * 2));
    for (size_t i = 0; i < len; ++i) {
        hex += QString::asprintf("%02x", data[i]);
    }
    return hex;
}

bool parseVerifiedPackageFile(const std::filesystem::path &pkgPath, OpenHubRecord &outRec)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(pkgPath, ec)) {
        return false;
    }

    WwpkgHandlePtr pkg(wwpkg_open(pkgPath.string().c_str()), &wwpkg_close);
    if (!pkg) {
        return false;
    }

    const wwpkg_manifest_t *man = wwpkg_get_manifest(pkg.get());
    const wwpkg_signature_t *sig = wwpkg_get_signature(pkg.get());
    if (!man) {
        return false;
    }

    OpenHubRecord rec;
    rec.id = QString::fromUtf8(man->id);
    if (rec.id.isEmpty()) {
        rec.id = QString::fromStdString(pkgPath.stem().string());
    }
    rec.slug = QString::fromUtf8(man->slug);
    rec.name = QString::fromUtf8(man->name);
    if (rec.name.isEmpty()) {
        rec.name = rec.id;
    }
    rec.version = QString::fromUtf8(man->version);
    rec.author_name = QString::fromUtf8(man->author_name);
    rec.author_email = QString::fromUtf8(man->author_email);
    rec.author_pubkey_hex = bytesToHex(man->author_pubkey, WWPKG_PUBKEY_BYTES);
    rec.license = QString::fromUtf8(man->license);
    rec.description = QString::fromUtf8(man->description);
    rec.type = man->type;

    for (uint32_t t = 0; t < man->tag_count && t < WWPKG_MAX_TAGS; ++t) {
        rec.tags.push_back(QString::fromUtf8(man->tags[t]));
    }

    rec.cap_compute = man->capabilities.compute_shaders;
    rec.cap_audio_spectrum = man->capabilities.audio_spectrum;
    rec.cap_audio_playback = man->capabilities.audio_playback;
    rec.cap_scripting = man->capabilities.scripting;
    rec.cap_foreign_toplevel = man->capabilities.foreign_toplevel;
    rec.cap_network = false;
    rec.cap_filesystem = false;

    rec.max_vram_mb = man->budgets.max_vram_mb > 0 ? man->budgets.max_vram_mb : 128;
    rec.max_ram_mb = man->budgets.max_ram_mb > 0 ? man->budgets.max_ram_mb : 256;
    rec.target_fps = man->budgets.target_fps > 0 ? man->budgets.target_fps : 60;

    if (sig) {
        rec.merkle_root_hex = bytesToHex(sig->merkle_root, WWPKG_MERKLE_ROOT_BYTES);
        rec.signature_hex = bytesToHex(sig->signature, WWPKG_SIGNATURE_BYTES);
        rec.timestamp = sig->timestamp;
        rec.cid = "sha256:" + rec.merkle_root_hex;

        bool is_trusted = false;
        rec.is_verified = wwpkg_verify_signature(pkg.get(), &is_trusted);
        rec.is_trusted = is_trusted;
    } else {
        rec.cid = "sha256:" + rec.id;
        rec.is_verified = false;
        rec.is_trusted = false;
    }

    rec.local_pkg_path = QString::fromStdString(pkgPath.string());
    uint64_t sz = std::filesystem::file_size(pkgPath, ec);
    rec.file_size_bytes = ec ? 0 : sz;
    rec.is_installed = rec.is_verified;
    rec.download_count = 0;
    rec.rating = 0.0f;

    // Extract preview asset into local cache for verified packages so grid cards display thumbnails
    // before activation.
    if (rec.is_verified) {
        const char *previewCands[4] = {
            man->entrypoint_preview[0] != '\0' ? man->entrypoint_preview : nullptr, "preview.webp",
            "preview.png", "preview.jpg"};
        for (const char *cand : previewCands) {
            if (!cand || cand[0] == '\0') {
                continue;
            }
            size_t assetLen = 0;
            const uint8_t *assetBytes = wwpkg_read_asset(pkg.get(), cand, &assetLen);
            if (!assetBytes || assetLen == 0) {
                continue;
            }

            QString cacheRoot =
                QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
            if (cacheRoot.isEmpty()) {
                cacheRoot = QStringLiteral("/tmp");
            }
            std::filesystem::path cacheDir = std::filesystem::path(
                (cacheRoot + QStringLiteral("/wwal/openhub_previews")).toStdString());
            std::filesystem::create_directories(cacheDir, ec);

            std::string ext = std::filesystem::path(cand).extension().string();
            if (ext.empty()) {
                ext = ".webp";
            }
            QString safeStem = rec.id;
            for (qsizetype i = 0; i < safeStem.size(); ++i) {
                const QChar c = safeStem[i];
                if (!c.isLetterOrNumber() && c != QLatin1Char('_') && c != QLatin1Char('-')) {
                    safeStem[i] = QLatin1Char('_');
                }
            }
            std::filesystem::path outPreview = cacheDir / (safeStem.toStdString() + ext);
            const uint64_t existingSz = std::filesystem::exists(outPreview, ec)
                                            ? std::filesystem::file_size(outPreview, ec)
                                            : 0;
            if (ec || existingSz != assetLen) {
                QFile pf(QString::fromStdString(outPreview.string()));
                if (pf.open(QIODevice::WriteOnly)) {
                    pf.write(reinterpret_cast<const char *>(assetBytes),
                             static_cast<qint64>(assetLen));
                    pf.close();
                }
            }
            if (std::filesystem::is_regular_file(outPreview, ec)) {
                rec.preview_path = QString::fromStdString(outPreview.string());
            }
            break;
        }
    }

    outRec = std::move(rec);
    return true;
}

static QString resolveConfinedHubAsset(const std::filesystem::path &rootDir, const QString &rel)
{
    if (rel.isEmpty()) {
        return {};
    }
    std::filesystem::path relPath(rel.toStdString());
    if (relPath.is_absolute()) {
        return {};
    }
    std::error_code ec;
    const auto canonRoot = std::filesystem::weakly_canonical(rootDir, ec);
    if (ec) {
        return {};
    }
    const auto canonTarget = std::filesystem::weakly_canonical(rootDir / relPath, ec);
    if (ec || !std::filesystem::is_regular_file(canonTarget, ec)) {
        return {};
    }
    auto [rootEnd, _] =
        std::mismatch(canonRoot.begin(), canonRoot.end(), canonTarget.begin(), canonTarget.end());
    if (rootEnd != canonRoot.end()) {
        return {};
    }
    return QString::fromStdString(canonTarget.string());
}

static void parseHubManifestJsonFile(const std::filesystem::path &jsonPath,
                                     const std::filesystem::path &rootDir,
                                     std::unordered_set<std::string> &seenIds,
                                     std::vector<OpenHubRecord> &outRecords)
{
    QFile jf(QString::fromStdString(jsonPath.string()));
    if (!jf.open(QIODevice::ReadOnly)) {
        return;
    }
    const QByteArray raw = jf.read(8 * 1024 * 1024);
    QJsonParseError jerr{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &jerr);
    if (jerr.error != QJsonParseError::NoError) {
        return;
    }

    QJsonArray arr;
    if (doc.isArray()) {
        arr = doc.array();
    } else if (doc.isObject()) {
        const QJsonObject rootObj = doc.object();
        arr = rootObj.contains(QStringLiteral("wallpapers"))
                  ? rootObj.value(QStringLiteral("wallpapers")).toArray()
                  : rootObj.value(QStringLiteral("items")).toArray();
    }

    for (const QJsonValue &val : arr) {
        if (!val.isObject()) {
            continue;
        }
        const QJsonObject obj = val.toObject();
        OpenHubRecord rec{};
        rec.id = obj.value(QStringLiteral("id")).toString().trimmed();
        rec.slug = obj.value(QStringLiteral("slug")).toString().trimmed();
        if (rec.id.isEmpty()) {
            rec.id = rec.slug;
        }
        if (rec.id.isEmpty()) {
            continue;
        }
        std::string key = rec.id.toStdString();
        if (seenIds.contains(key)) {
            continue;
        }

        rec.name = obj.value(QStringLiteral("name")).toString().trimmed();
        if (rec.name.isEmpty()) {
            rec.name = obj.value(QStringLiteral("title")).toString().trimmed();
        }
        if (rec.name.isEmpty()) {
            rec.name = rec.id;
        }
        rec.version = obj.value(QStringLiteral("version")).toString(QStringLiteral("1.0.0"));
        rec.author_name = obj.value(QStringLiteral("author_name")).toString();
        if (rec.author_name.isEmpty()) {
            rec.author_name = obj.value(QStringLiteral("author")).toString();
        }
        rec.author_email = obj.value(QStringLiteral("author_email")).toString();
        rec.license = obj.value(QStringLiteral("license")).toString(QStringLiteral("CC-BY-4.0"));
        rec.description = obj.value(QStringLiteral("description")).toString();
        rec.download_url = obj.value(QStringLiteral("download_url")).toString();
        if (rec.download_url.isEmpty()) {
            rec.download_url = obj.value(QStringLiteral("url")).toString();
        }
        rec.cid = obj.value(QStringLiteral("cid")).toString();
        if (rec.cid.isEmpty()) {
            rec.cid = QStringLiteral("sha256:") + rec.id;
        }
        rec.file_size_bytes = static_cast<uint64_t>(
            obj.value(QStringLiteral("file_size_bytes"))
                .toVariant()
                .toULongLong());
        if (rec.file_size_bytes == 0) {
            rec.file_size_bytes =
                static_cast<uint64_t>(obj.value(QStringLiteral("size")).toVariant().toULongLong());
        }
        rec.timestamp =
            static_cast<uint64_t>(obj.value(QStringLiteral("timestamp")).toVariant().toULongLong());
        rec.download_count = static_cast<uint32_t>(
            obj.value(QStringLiteral("download_count")).toInt(
                obj.value(QStringLiteral("downloads")).toInt(0)));
        rec.rating = static_cast<float>(obj.value(QStringLiteral("rating")).toDouble(0.0));

        const QString typeStr = obj.value(QStringLiteral("type")).toString().trimmed().toLower();
        if (typeStr == QStringLiteral("video")) {
            rec.type = WWPKG_TYPE_VIDEO_STREAM;
        } else if (typeStr == QStringLiteral("compute")) {
            rec.type = WWPKG_TYPE_SHADER_COMPUTE;
        } else if (typeStr == QStringLiteral("scene")) {
            rec.type = WWPKG_TYPE_SCENE_2D;
        } else {
            rec.type = WWPKG_TYPE_SHADER_FRAGMENT;
        }

        const QJsonArray tagArr = obj.value(QStringLiteral("tags")).toArray();
        for (const QJsonValue &tv : tagArr) {
            if (tv.isString()) {
                rec.tags.push_back(tv.toString());
            }
        }

        QString prevRaw = obj.value(QStringLiteral("preview_path")).toString().trimmed();
        if (prevRaw.isEmpty()) {
            prevRaw = obj.value(QStringLiteral("preview")).toString().trimmed();
        }
        if (!prevRaw.isEmpty()) {
            rec.preview_path = resolveConfinedHubAsset(rootDir, prevRaw);
        }

        QString pkgRel = obj.value(QStringLiteral("package_path")).toString().trimmed();
        if (!pkgRel.isEmpty()) {
            rec.local_pkg_path = resolveConfinedHubAsset(rootDir, pkgRel);
            rec.is_installed = !rec.local_pkg_path.isEmpty();
        }

        seenIds.insert(std::move(key));
        outRecords.push_back(std::move(rec));
    }
}

std::vector<OpenHubRecord> scanLocalHubRootsWorker(const std::atomic<bool> &cancelFlag,
                                                   const std::string &extraDownloadDir)
{
    std::vector<std::filesystem::path> roots;
    std::unordered_set<std::string> seenRoots;
    auto addRoot = [&](const std::filesystem::path &p) {
        if (p.empty()) {
            return;
        }
        std::string norm = p.lexically_normal().string();
        if (seenRoots.insert(norm).second) {
            roots.push_back(std::filesystem::path(norm));
        }
    };

    const char *envDir = std::getenv("WWAL_OPENHUB_DIR");
    if (envDir && *envDir) {
        addRoot(std::filesystem::path(envDir));
    }
    if (!extraDownloadDir.empty()) {
        addRoot(std::filesystem::path(extraDownloadDir));
    }

    QString dataBase = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!dataBase.isEmpty()) {
        addRoot(std::filesystem::path((dataBase + "/waywal/hub").toStdString()));
        addRoot(std::filesystem::path((dataBase + "/wwal/hub").toStdString()));
        addRoot(std::filesystem::path((dataBase + "/waywal/downloads").toStdString()));
        addRoot(std::filesystem::path((dataBase + "/wwal/downloads").toStdString()));
    }

    std::vector<OpenHubRecord> records;
    std::unordered_set<std::string> seenIds;
    constexpr auto dirOpts = std::filesystem::directory_options::skip_permission_denied;

    for (const auto &root : roots) {
        if (cancelFlag.load(std::memory_order_relaxed)) {
            break;
        }
        std::error_code ec;
        if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
            continue;
        }

        std::filesystem::directory_iterator dirIt(root, dirOpts, ec);
        if (ec) {
            continue;
        }

        for (const auto &entry : dirIt) {
            if (cancelFlag.load(std::memory_order_relaxed)) {
                break;
            }
            std::error_code fEc;
            if (!entry.is_regular_file(fEc) || entry.path().extension() != ".wwpkg") {
                continue;
            }

            OpenHubRecord rec;
            if (parseVerifiedPackageFile(entry.path(), rec)) {
                std::string key = rec.id.toStdString();
                if (!seenIds.contains(key)) {
                    seenIds.insert(std::move(key));
                    records.push_back(std::move(rec));
                }
            }
        }

        for (const char *feedName : {"catalog.json", "manifest.json", "index.json"}) {
            if (cancelFlag.load(std::memory_order_relaxed)) {
                break;
            }
            std::filesystem::path feedPath = root / feedName;
            if (std::filesystem::is_regular_file(feedPath, ec)) {
                parseHubManifestJsonFile(feedPath, root, seenIds, records);
            }
        }
    }

    return records;
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

constexpr const char *STYLE_PILL_COMPUTE =
    "background-color: #064e3b; color: #34d399; border: 1px solid #059669; "
    "border-radius: 10px; padding: 2px 8px; font-size: 10px; font-weight: bold;";

constexpr const char *STYLE_PILL_AUDIO =
    "background-color: #075985; color: #38bdf8; border: 1px solid #0284c7; "
    "border-radius: 10px; padding: 2px 8px; font-size: 10px; font-weight: bold;";

constexpr const char *STYLE_PILL_SCRIPT =
    "background-color: #581c87; color: #c084fc; border: 1px solid #7e22ce; "
    "border-radius: 10px; padding: 2px 8px; font-size: 10px; font-weight: bold;";

constexpr const char *STYLE_PILL_AIRGAP =
    "background-color: #164e63; color: #22d3ee; border: 1px solid #0891b2; "
    "border-radius: 10px; padding: 2px 8px; font-size: 10px; font-weight: bold;";

} // namespace

struct OpenHubBrowserWidget::Impl {
    OpenHubBrowserWidget *owner{nullptr};

    std::vector<OpenHubRecord> allRecords;
    std::vector<OpenHubRecord> filteredRecords;
    std::unordered_map<std::string, size_t> idToIndex;
    std::unordered_map<std::string, size_t> allIdToIndex;

    std::thread scanThread;
    std::atomic<bool> scanCancel{false};
    uint64_t scanGeneration{0};

    QString activeQuery;
    int activeCategoryIndex{0};
    int activeSortIndex{0};

    QTimer *hoverDwellTimer{nullptr};
    int pendingHoverIndex{-1};
    int activeHoverIndex{-1};

    QTimer *searchDebounceTimer{nullptr};

    int selectedItemIndex{-1};
    QString selectedPackageId;

    QSplitter *mainSplitter{nullptr};
    QToolButton *wizardLaunchBtn{nullptr};
    QLabel *statusTelemetryLabel{nullptr};

    QWidget *inspectorDrawer{nullptr};
    QScrollArea *inspectorScrollArea{nullptr};
    QLabel *inspectorTitleLabel{nullptr};
    QLabel *inspectorVersionSlugLabel{nullptr};
    QLabel *inspectorAuthorLabel{nullptr};
    QLabel *inspectorCidLabel{nullptr};
    QPushButton *copyCidBtn{nullptr};

    QLabel *cryptoBadgeLabel{nullptr};
    QLabel *authorPubkeyLabel{nullptr};
    QLabel *merkleRootLabel{nullptr};

    QWidget *capabilitiesContainer{nullptr};
    QGridLayout *capabilitiesLayout{nullptr};

    QLabel *vramBudgetLabel{nullptr};
    QProgressBar *vramBudgetBar{nullptr};
    QLabel *ramBudgetLabel{nullptr};
    QProgressBar *ramBudgetBar{nullptr};
    QLabel *targetFpsBadge{nullptr};

    QLabel *installStateBadge{nullptr};
    QWidget *downloadProgressBox{nullptr};
    QProgressBar *downloadProgressBar{nullptr};
    QLabel *downloadTelemetryLabel{nullptr};

    QPushButton *installDownloadBtn{nullptr};
    QComboBox *monitorTargetCombo{nullptr};
    QPushButton *applyToMonitorBtn{nullptr};

    void stopScanThread();
    void rebuildAllIndex();
    void launchAsyncScan();
    void executeQuery();
    void applySorting();
    void syncWithGridView();
    void updateInspector(const OpenHubRecord &rec);
    void handleDownloadProgress(const QString &pkgId, const DownloadProgress &prog);
};

void OpenHubBrowserWidget::Impl::stopScanThread()
{
    scanCancel.store(true, std::memory_order_relaxed);
    if (scanThread.joinable()) {
        scanThread.join();
    }
}

void OpenHubBrowserWidget::Impl::rebuildAllIndex()
{
    allIdToIndex.clear();
    allIdToIndex.reserve(allRecords.size());
    for (size_t i = 0; i < allRecords.size(); ++i) {
        allIdToIndex[allRecords[i].id.toStdString()] = i;
    }
}

void OpenHubBrowserWidget::Impl::launchAsyncScan()
{
    stopScanThread();
    scanCancel.store(false, std::memory_order_relaxed);
    const uint64_t gen = ++scanGeneration;

    QString hubDir =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/waywal/hub";
    QDir().mkpath(hubDir);

    QPointer<OpenHubBrowserWidget> self(owner);
    std::string downloadDir =
        owner->m_downloadMgr ? owner->m_downloadMgr->downloadDirectory() : std::string{};
    scanThread = std::thread([this, self, gen, downloadDir = std::move(downloadDir)]() {
        auto discovered = scanLocalHubRootsWorker(scanCancel, downloadDir);
        if (scanCancel.load(std::memory_order_relaxed) || !self) {
            return;
        }

        QMetaObject::invokeMethod(
            self,
            [self, gen, records = std::move(discovered)]() mutable {
                if (!self || gen != self->m_impl->scanGeneration) {
                    return;
                }

                std::unordered_set<std::string> seenIds;
                seenIds.reserve(records.size());
                for (auto &incoming : records) {
                    std::string idKey = incoming.id.toStdString();
                    seenIds.insert(idKey);
                    auto prevIt = self->m_impl->allIdToIndex.find(idKey);
                    if (prevIt != self->m_impl->allIdToIndex.end()) {
                        const auto &prev = self->m_impl->allRecords[prevIt->second];
                        if (prev.is_downloading) {
                            incoming.is_downloading = true;
                        }
                        if (incoming.download_url.isEmpty() && !prev.download_url.isEmpty()) {
                            incoming.download_url = prev.download_url;
                        }
                    }
                }

                // Retain remote or in-flight entries not yet written to local storage.
                for (const auto &prev : self->m_impl->allRecords) {
                    std::string idKey = prev.id.toStdString();
                    if (!seenIds.contains(idKey) &&
                        (prev.is_downloading ||
                         (!prev.download_url.isEmpty() && prev.local_pkg_path.isEmpty()))) {
                        seenIds.insert(std::move(idKey));
                        records.push_back(prev);
                    }
                }

                self->m_impl->allRecords = std::move(records);
                self->m_impl->rebuildAllIndex();
                self->m_impl->executeQuery();
            },
            Qt::QueuedConnection);
    });
}

void OpenHubBrowserWidget::Impl::executeQuery()
{
    filteredRecords.clear();
    idToIndex.clear();

    QString queryText = activeQuery.trimmed().toLower();

    for (const auto &rec : allRecords) {
        if (activeCategoryIndex > 0) {
            wwpkg_type_t targetType = WWPKG_TYPE_UNKNOWN;
            switch (activeCategoryIndex) {
            case 1:
                targetType = WWPKG_TYPE_SHADER_FRAGMENT;
                break;
            case 2:
                targetType = WWPKG_TYPE_SHADER_COMPUTE;
                break;
            case 3:
                targetType = WWPKG_TYPE_SCENE_2D;
                break;
            case 4:
                targetType = WWPKG_TYPE_SCENE_PUPPET;
                break;
            case 5:
                targetType = WWPKG_TYPE_SCENE_PARTICLES;
                break;
            case 6:
                targetType = WWPKG_TYPE_VIDEO_STREAM;
                break;
            default:
                break;
            }
            if (targetType != WWPKG_TYPE_UNKNOWN && rec.type != targetType) {
                continue;
            }
        }

        if (!queryText.isEmpty()) {
            bool matches =
                rec.name.toLower().contains(queryText) || rec.slug.toLower().contains(queryText) ||
                rec.author_name.toLower().contains(queryText) ||
                rec.cid.toLower().contains(queryText) || rec.license.toLower().contains(queryText);
            if (!matches) {
                continue;
            }
        }

        filteredRecords.push_back(rec);
    }

    applySorting();
    syncWithGridView();
}

void OpenHubBrowserWidget::Impl::applySorting()
{
    switch (activeSortIndex) {
    case 0:
        std::sort(filteredRecords.begin(), filteredRecords.end(),
                  [](const OpenHubRecord &a, const OpenHubRecord &b) {
                      double scoreA = static_cast<double>(a.download_count) * a.rating;
                      double scoreB = static_cast<double>(b.download_count) * b.rating;
                      if (std::abs(scoreA - scoreB) > 1e-4) {
                          return scoreA > scoreB;
                      }
                      return a.timestamp > b.timestamp;
                  });
        break;
    case 1:
        std::sort(filteredRecords.begin(), filteredRecords.end(),
                  [](const OpenHubRecord &a, const OpenHubRecord &b) {
                      if (a.download_count != b.download_count) {
                          return a.download_count > b.download_count;
                      }
                      return a.timestamp > b.timestamp;
                  });
        break;
    case 2:
        std::sort(filteredRecords.begin(), filteredRecords.end(),
                  [](const OpenHubRecord &a, const OpenHubRecord &b) {
                      return a.timestamp > b.timestamp;
                  });
        break;
    case 3:
        std::sort(filteredRecords.begin(), filteredRecords.end(),
                  [](const OpenHubRecord &a, const OpenHubRecord &b) {
                      return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
                  });
        break;
    default:
        break;
    }

    idToIndex.clear();
    selectedItemIndex = -1;
    for (size_t i = 0; i < filteredRecords.size(); ++i) {
        std::string key = filteredRecords[i].id.toStdString();
        idToIndex[key] = i;
        if (!selectedPackageId.isEmpty() && filteredRecords[i].id == selectedPackageId) {
            selectedItemIndex = static_cast<int>(i);
        }
    }
}

void OpenHubBrowserWidget::Impl::syncWithGridView()
{
    if (!owner->m_gridView) {
        return;
    }

    auto &catIndex = owner->m_gridView->catalogIndex();
    catIndex.clear();

    for (size_t i = 0; i < filteredRecords.size(); ++i) {
        const auto &rec = filteredRecords[i];
        CatalogItem ci;
        ci.id = static_cast<uint32_t>(i + 1);
        ci.workshop_id = rec.id.toStdString();
        ci.title = rec.name.toStdString();
        ci.path = rec.local_pkg_path.toStdString();
        ci.preview_path = rec.preview_path.toStdString();
        ci.file_size = rec.file_size_bytes;
        ci.date_modified = static_cast<int64_t>(rec.timestamp);

        switch (rec.type) {
        case WWPKG_TYPE_VIDEO_STREAM:
            ci.badge = WallpaperBadge::Video;
            break;
        case WWPKG_TYPE_SHADER_FRAGMENT:
        case WWPKG_TYPE_SHADER_COMPUTE:
            ci.badge = WallpaperBadge::Shader;
            break;
        case WWPKG_TYPE_SCENE_2D:
        case WWPKG_TYPE_SCENE_PUPPET:
        case WWPKG_TYPE_SCENE_PARTICLES:
            ci.badge = WallpaperBadge::Scene;
            break;
        default:
            ci.badge = WallpaperBadge::Image;
            break;
        }

        catIndex.addItem(std::move(ci));
    }

    if (statusTelemetryLabel) {
        statusTelemetryLabel->setText(QString("%1 packages").arg(filteredRecords.size()));
    }

    catIndex.refilter();
    owner->m_gridView->refreshCatalogView();
}

void OpenHubBrowserWidget::Impl::updateInspector(const OpenHubRecord &rec)
{
    if (!inspectorDrawer) {
        return;
    }

    inspectorTitleLabel->setText(rec.name);
    inspectorVersionSlugLabel->setText(QString("<b>%1</b> (v%2) | %3 | %4")
                                           .arg(rec.slug.toHtmlEscaped())
                                           .arg(rec.version.toHtmlEscaped())
                                           .arg(rec.license.toHtmlEscaped())
                                           .arg(formatBytes(rec.file_size_bytes).toHtmlEscaped()));

    inspectorAuthorLabel->setText(QString("Author: %1 &lt;%2&gt;")
                                      .arg(rec.author_name.toHtmlEscaped())
                                      .arg(rec.author_email.toHtmlEscaped()));

    QString shortCid =
        rec.cid.length() > 24 ? (rec.cid.left(15) + "..." + rec.cid.right(8)) : rec.cid;
    inspectorCidLabel->setText(
        QString("<b>CID:</b> <code>%1</code>").arg(shortCid.toHtmlEscaped()));
    inspectorCidLabel->setToolTip(rec.cid);

    if (rec.is_verified) {
        cryptoBadgeLabel->setText("Verified Signature");
        cryptoBadgeLabel->setStyleSheet(
            "background-color: #065f46; color: #34d399; border: 1px solid #059669; "
            "border-radius: 4px; padding: 4px 10px; font-weight: bold; font-size: 11px;");
    } else {
        cryptoBadgeLabel->setText("Unverified Package");
        cryptoBadgeLabel->setStyleSheet(
            "background-color: #78350f; color: #fbbf24; border: 1px solid #d97706; "
            "border-radius: 4px; padding: 4px 10px; font-weight: bold; font-size: 11px;");
    }

    QString shortPubkey =
        rec.author_pubkey_hex.length() > 16
            ? (rec.author_pubkey_hex.left(8) + "..." + rec.author_pubkey_hex.right(8))
            : rec.author_pubkey_hex;
    authorPubkeyLabel->setText(
        QString("Public Key: <code>%1</code>").arg(shortPubkey.toHtmlEscaped()));

    QString shortMerkle = rec.merkle_root_hex.length() > 16
                              ? (rec.merkle_root_hex.left(8) + "..." + rec.merkle_root_hex.right(8))
                              : rec.merkle_root_hex;
    merkleRootLabel->setText(
        QString("Merkle Root: <code>%1</code>").arg(shortMerkle.toHtmlEscaped()));

    QLayoutItem *child;
    while ((child = capabilitiesLayout->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }

    int pillIdx = 0;
    auto addPill = [this, &pillIdx](const QString &text, const char *style) {
        auto *lbl = new QLabel(text, capabilitiesContainer);
        lbl->setTextFormat(Qt::PlainText);
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setStyleSheet(style);
        capabilitiesLayout->addWidget(lbl, pillIdx / 2, pillIdx % 2);
        ++pillIdx;
    };

    if (rec.cap_compute)
        addPill("Compute Shader", STYLE_PILL_COMPUTE);
    if (rec.cap_audio_spectrum)
        addPill("Audio Reactive", STYLE_PILL_AUDIO);
    if (rec.cap_scripting)
        addPill("SceneScript", STYLE_PILL_SCRIPT);
    addPill("Offline Safe", STYLE_PILL_AIRGAP);

    vramBudgetLabel->setText(QString("VRAM Limit: %1 MB").arg(rec.max_vram_mb));
    vramBudgetBar->setValue(static_cast<int>(std::min(rec.max_vram_mb, 512u)));

    ramBudgetLabel->setText(QString("RAM Limit: %1 MB").arg(rec.max_ram_mb));
    ramBudgetBar->setValue(static_cast<int>(std::min(rec.max_ram_mb, 1024u)));

    targetFpsBadge->setText(QString("Target: <b>%1 FPS</b>").arg(rec.target_fps));
    installStateBadge->setToolTip(rec.last_error_message);

    // Gate Apply strictly behind verified cryptographic signature + local installation
    const bool canApply = rec.is_installed && rec.is_verified && !rec.is_downloading;
    applyToMonitorBtn->setEnabled(canApply);

    if (rec.is_downloading) {
        installStateBadge->setText("<font color='#38bdf8'><b>Downloading...</b></font>");
        downloadProgressBox->setVisible(true);
        installDownloadBtn->setText("Cancel Download");
        installDownloadBtn->setEnabled(true);
    } else if (rec.last_download_failed) {
        installStateBadge->setText("<font color='#f87171'><b>Download Failed</b></font>");
        downloadProgressBox->setVisible(false);
        installDownloadBtn->setText("Retry Install");
        installDownloadBtn->setEnabled(!rec.download_url.isEmpty());
    } else if (rec.last_download_cancelled) {
        installStateBadge->setText("<font color='#94a3b8'>Cancelled</font>");
        downloadProgressBox->setVisible(false);
        installDownloadBtn->setText(rec.is_installed ? "Reinstall" : "Install");
        installDownloadBtn->setEnabled(!rec.download_url.isEmpty());
    } else if (rec.is_installed && rec.is_verified) {
        installStateBadge->setText("<font color='#34d399'><b>Installed</b></font>");
        downloadProgressBox->setVisible(false);
        installDownloadBtn->setText("Reinstall");
        installDownloadBtn->setEnabled(!rec.download_url.isEmpty());
    } else if (!rec.local_pkg_path.isEmpty() && !rec.is_verified) {
        installStateBadge->setText("<font color='#f87171'><b>Signature Rejected</b></font>");
        downloadProgressBox->setVisible(false);
        installDownloadBtn->setText("Blocked");
        installDownloadBtn->setEnabled(false);
    } else {
        installStateBadge->setText("<font color='#94a3b8'>Available</font>");
        downloadProgressBox->setVisible(false);
        installDownloadBtn->setText("Install");
        installDownloadBtn->setEnabled(!rec.download_url.isEmpty());
    }
}

void OpenHubBrowserWidget::Impl::handleDownloadProgress(const QString &pkgId,
                                                        const DownloadProgress &prog)
{
    std::string key = pkgId.toStdString();

    QString expectedMerkle;
    QString expectedPubkey;
    QString expectedCid;
    if (auto allIt = allIdToIndex.find(key); allIt != allIdToIndex.end()) {
        const auto &prev = allRecords[allIt->second];
        expectedMerkle = prev.merkle_root_hex.trimmed();
        expectedPubkey = prev.author_pubkey_hex.trimmed();
        expectedCid = prev.cid.trimmed();
    } else if (auto filtIt = idToIndex.find(key); filtIt != idToIndex.end()) {
        const auto &prev = filteredRecords[filtIt->second];
        expectedMerkle = prev.merkle_root_hex.trimmed();
        expectedPubkey = prev.author_pubkey_hex.trimmed();
        expectedCid = prev.cid.trimmed();
    }

    bool verifiedInstall = false;
    bool digestMismatch = false;
    OpenHubRecord verifiedMeta;
    if (prog.state == TaskState::Installed && !prog.installed_path.empty()) {
        verifiedInstall = parseVerifiedPackageFile(prog.installed_path, verifiedMeta);
        if (verifiedInstall && verifiedMeta.is_verified) {
            if (!expectedMerkle.isEmpty() &&
                expectedMerkle.compare(verifiedMeta.merkle_root_hex, Qt::CaseInsensitive) != 0) {
                digestMismatch = true;
            }
            if (!expectedPubkey.isEmpty() &&
                expectedPubkey.compare(verifiedMeta.author_pubkey_hex, Qt::CaseInsensitive) != 0) {
                digestMismatch = true;
            }
            if (expectedCid.startsWith(QStringLiteral("sha256:"), Qt::CaseInsensitive) &&
                expectedCid.compare(verifiedMeta.cid, Qt::CaseInsensitive) != 0) {
                digestMismatch = true;
            }
        }
        if (!verifiedInstall || !verifiedMeta.is_verified || digestMismatch) {
            std::error_code rmEc;
            std::filesystem::remove(prog.installed_path, rmEc);
            if (!prog.extracted_dir.empty()) {
                std::filesystem::remove_all(prog.extracted_dir, rmEc);
            }
        }
    }

    auto applyState = [&](OpenHubRecord &rec) {
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
            rec.is_downloading = true;
            rec.last_download_failed = false;
            rec.last_download_cancelled = false;
            rec.last_error_message.clear();
            rec.download_pct = prog.progress_pct;
            rec.download_speed_bps = prog.speed_bytes_per_sec;
            break;
        case TaskState::Installed:
            rec.is_downloading = false;
            if (verifiedInstall && verifiedMeta.is_verified && !digestMismatch) {
                rec.local_pkg_path = verifiedMeta.local_pkg_path;
                rec.author_pubkey_hex = verifiedMeta.author_pubkey_hex;
                rec.merkle_root_hex = verifiedMeta.merkle_root_hex;
                rec.signature_hex = verifiedMeta.signature_hex;
                rec.cid = verifiedMeta.cid;
                rec.is_verified = true;
                rec.is_trusted = verifiedMeta.is_trusted;
                rec.is_installed = true;
                rec.last_download_failed = false;
                rec.last_download_cancelled = false;
                rec.last_error_message.clear();
            } else {
                rec.local_pkg_path.clear();
                rec.is_verified = false;
                rec.is_installed = false;
                rec.last_download_failed = true;
                rec.last_download_cancelled = false;
                rec.last_error_message =
                    digestMismatch
                        ? QStringLiteral("Downloaded package digest or author key mismatch")
                        : QStringLiteral("Ed25519 package signature verification failed");
            }
            break;
        case TaskState::Failed:
            rec.is_downloading = false;
            rec.last_download_failed = true;
            rec.last_download_cancelled = false;
            rec.last_error_message = QString::fromStdString(prog.error_message);
            break;
        case TaskState::Cancelled:
            rec.is_downloading = false;
            rec.last_download_failed = false;
            rec.last_download_cancelled = true;
            rec.last_error_message.clear();
            break;
        }
    };

    auto allIt = allIdToIndex.find(key);
    if (allIt != allIdToIndex.end()) {
        applyState(allRecords[allIt->second]);
    }

    auto filtIt = idToIndex.find(key);
    if (filtIt == idToIndex.end()) {
        return;
    }

    auto &rec = filteredRecords[filtIt->second];
    applyState(rec);

    if (selectedItemIndex == static_cast<int>(filtIt->second)) {
        if (prog.state == TaskState::Downloading || prog.state == TaskState::Verifying ||
            prog.state == TaskState::Queued) {
            owner->onDownloadProgressUpdated(pkgId, std::min(prog.progress_pct, 99.9f),
                                             prog.speed_bytes_per_sec);
        } else {
            downloadProgressBox->setVisible(false);
            updateInspector(rec);
        }
    }
}

OpenHubBrowserWidget::OpenHubBrowserWidget(std::shared_ptr<DownloadManager> downloadMgr,
                                           QWidget *parent)
    : QWidget(parent), m_downloadMgr(std::move(downloadMgr)), m_impl(std::make_unique<Impl>())
{
    m_impl->owner = this;

    setupUi();
    wireSignals();

    m_impl->executeQuery();
    m_impl->launchAsyncScan();
}

OpenHubBrowserWidget::~OpenHubBrowserWidget()
{
    m_impl->stopScanThread();
    if (m_impl->hoverDwellTimer) {
        m_impl->hoverDwellTimer->stop();
    }
    if (m_impl->searchDebounceTimer) {
        m_impl->searchDebounceTimer->stop();
    }
}

QSize OpenHubBrowserWidget::sizeHint() const { return QSize(360, 600); }

QSize OpenHubBrowserWidget::minimumSizeHint() const { return QSize(200, 200); }

void OpenHubBrowserWidget::setupUi()
{
    setStyleSheet(STYLE_PANEL_BG);

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *toolbarFrame = new QFrame(this);
    toolbarFrame->setStyleSheet("background-color: #181b22; border-bottom: 1px solid #262b36;");
    auto *toolbarOuter = new QVBoxLayout(toolbarFrame);
    toolbarOuter->setContentsMargins(10, 8, 10, 8);
    toolbarOuter->setSpacing(8);

    auto *topRow = new QHBoxLayout();
    topRow->setSpacing(8);

    m_searchEdit = new QLineEdit(toolbarFrame);
    m_searchEdit->setPlaceholderText("Search Open Hub...");
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setFixedHeight(28);
    m_searchEdit->setStyleSheet(STYLE_SEARCH_BAR);
    topRow->addWidget(m_searchEdit, 1);

    m_refreshBtn = new QToolButton(toolbarFrame);
    m_refreshBtn->setText("Refresh");
    m_refreshBtn->setToolTip("Refresh Open Hub feed and local packages");
    m_refreshBtn->setFixedHeight(28);
    m_refreshBtn->setStyleSheet(
        "QToolButton { background-color: #20242e; color: #f1f5f9; border: 1px solid #2c3240; "
        "border-radius: 5px; padding: 4px 12px; font-size: 12px; }"
        "QToolButton:hover { background-color: #2a303d; color: #ffffff; }");
    topRow->addWidget(m_refreshBtn);

    m_impl->wizardLaunchBtn = new QToolButton(toolbarFrame);
    m_impl->wizardLaunchBtn->setText("Package...");
    m_impl->wizardLaunchBtn->setVisible(false);

    toolbarOuter->addLayout(topRow);

    auto *filterRow = new QHBoxLayout();
    filterRow->setSpacing(8);

    m_categoryCombo = new QComboBox(toolbarFrame);
    m_categoryCombo->addItems({"All Categories", "Fragment Shaders", "Compute Shaders", "2D Scenes",
                               "Puppet Scenes", "Particle Systems", "Video Streams"});
    m_categoryCombo->setFixedHeight(26);
    m_categoryCombo->setStyleSheet(STYLE_TOOLBAR_COMBO);
    filterRow->addWidget(m_categoryCombo, 1);

    m_sortCombo = new QComboBox(toolbarFrame);
    m_sortCombo->addItems({"Trending", "Most Downloaded", "Recently Updated", "Name"});
    m_sortCombo->setFixedHeight(26);
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
    drawerLayout->setContentsMargins(14, 14, 14, 14);
    drawerLayout->setSpacing(10);

    m_impl->inspectorTitleLabel =
        new QLabel("Select a package to view details", m_impl->inspectorDrawer);
    m_impl->inspectorTitleLabel->setTextFormat(Qt::PlainText);
    m_impl->inspectorTitleLabel->setStyleSheet(
        "font-size: 14px; font-weight: bold; color: #f1f5f9;");
    m_impl->inspectorTitleLabel->setWordWrap(true);
    m_impl->inspectorTitleLabel->setMinimumWidth(0);
    drawerLayout->addWidget(m_impl->inspectorTitleLabel);

    m_impl->inspectorVersionSlugLabel = new QLabel("Package Slug: None", m_impl->inspectorDrawer);
    m_impl->inspectorVersionSlugLabel->setStyleSheet("font-size: 11px; color: #94a3b8;");
    m_impl->inspectorVersionSlugLabel->setWordWrap(true);
    m_impl->inspectorVersionSlugLabel->setMinimumWidth(0);
    drawerLayout->addWidget(m_impl->inspectorVersionSlugLabel);

    m_impl->inspectorAuthorLabel = new QLabel("Author: Unknown", m_impl->inspectorDrawer);
    m_impl->inspectorAuthorLabel->setStyleSheet("font-size: 11px; color: #cbd5e1;");
    m_impl->inspectorAuthorLabel->setWordWrap(true);
    m_impl->inspectorAuthorLabel->setMinimumWidth(0);
    drawerLayout->addWidget(m_impl->inspectorAuthorLabel);

    auto *cidRow = new QHBoxLayout();
    cidRow->setSpacing(8);
    m_impl->inspectorCidLabel = new QLabel("CID: None", m_impl->inspectorDrawer);
    m_impl->inspectorCidLabel->setStyleSheet("font-size: 11px; color: #38bdf8;");
    m_impl->inspectorCidLabel->setWordWrap(true);
    m_impl->inspectorCidLabel->setMinimumWidth(0);
    cidRow->addWidget(m_impl->inspectorCidLabel, 1);

    m_impl->copyCidBtn = new QPushButton("Copy", m_impl->inspectorDrawer);
    m_impl->copyCidBtn->setFixedSize(60, 26);
    m_impl->copyCidBtn->setStyleSheet(STYLE_BUTTON_SECONDARY);
    cidRow->addWidget(m_impl->copyCidBtn);
    drawerLayout->addLayout(cidRow);

    m_impl->cryptoBadgeLabel = new QLabel("Signature Status", m_impl->inspectorDrawer);
    m_impl->cryptoBadgeLabel->setTextFormat(Qt::PlainText);
    m_impl->cryptoBadgeLabel->setAlignment(Qt::AlignCenter);
    drawerLayout->addWidget(m_impl->cryptoBadgeLabel);

    m_impl->authorPubkeyLabel = new QLabel("Public Key: None", m_impl->inspectorDrawer);
    m_impl->authorPubkeyLabel->setStyleSheet("font-size: 11px; color: #94a3b8;");
    m_impl->authorPubkeyLabel->setMinimumWidth(0);
    drawerLayout->addWidget(m_impl->authorPubkeyLabel);

    m_impl->merkleRootLabel = new QLabel("Merkle Root: None", m_impl->inspectorDrawer);
    m_impl->merkleRootLabel->setStyleSheet("font-size: 11px; color: #94a3b8;");
    m_impl->merkleRootLabel->setMinimumWidth(0);
    drawerLayout->addWidget(m_impl->merkleRootLabel);

    auto *capTitle = new QLabel("<b>Capabilities</b>", m_impl->inspectorDrawer);
    capTitle->setStyleSheet("font-size: 11px; color: #cbd5e1;");
    drawerLayout->addWidget(capTitle);

    m_impl->capabilitiesContainer = new QWidget(m_impl->inspectorDrawer);
    m_impl->capabilitiesLayout = new QGridLayout(m_impl->capabilitiesContainer);
    m_impl->capabilitiesLayout->setContentsMargins(0, 2, 0, 2);
    m_impl->capabilitiesLayout->setHorizontalSpacing(6);
    m_impl->capabilitiesLayout->setVerticalSpacing(6);
    drawerLayout->addWidget(m_impl->capabilitiesContainer);

    auto *budgetsFrame = new QFrame(m_impl->inspectorDrawer);
    budgetsFrame->setStyleSheet(
        "background: #14171c; border: 1px solid #2c3240; border-radius: 6px; padding: 6px;");
    auto *budgetsLayout = new QVBoxLayout(budgetsFrame);
    budgetsLayout->setContentsMargins(8, 6, 8, 6);
    budgetsLayout->setSpacing(6);

    auto *vramRow = new QHBoxLayout();
    m_impl->vramBudgetLabel = new QLabel("VRAM Limit: 128 MB", budgetsFrame);
    m_impl->vramBudgetLabel->setTextFormat(Qt::PlainText);
    m_impl->vramBudgetLabel->setStyleSheet("font-size: 11px; color: #94a3b8;");
    vramRow->addWidget(m_impl->vramBudgetLabel);
    m_impl->vramBudgetBar = new QProgressBar(budgetsFrame);
    m_impl->vramBudgetBar->setRange(0, 512);
    m_impl->vramBudgetBar->setValue(128);
    m_impl->vramBudgetBar->setFixedHeight(6);
    m_impl->vramBudgetBar->setTextVisible(false);
    m_impl->vramBudgetBar->setStyleSheet(
        "QProgressBar { background: #1e222b; border-radius: 3px; }"
        "QProgressBar::chunk { background-color: #38bdf8; border-radius: 3px; }");
    vramRow->addWidget(m_impl->vramBudgetBar, 1);
    budgetsLayout->addLayout(vramRow);

    auto *ramRow = new QHBoxLayout();
    m_impl->ramBudgetLabel = new QLabel("RAM Limit: 256 MB", budgetsFrame);
    m_impl->ramBudgetLabel->setTextFormat(Qt::PlainText);
    m_impl->ramBudgetLabel->setStyleSheet("font-size: 11px; color: #94a3b8;");
    ramRow->addWidget(m_impl->ramBudgetLabel);
    m_impl->ramBudgetBar = new QProgressBar(budgetsFrame);
    m_impl->ramBudgetBar->setRange(0, 1024);
    m_impl->ramBudgetBar->setValue(256);
    m_impl->ramBudgetBar->setFixedHeight(6);
    m_impl->ramBudgetBar->setTextVisible(false);
    m_impl->ramBudgetBar->setStyleSheet(
        "QProgressBar { background: #1e222b; border-radius: 3px; }"
        "QProgressBar::chunk { background-color: #a855f7; border-radius: 3px; }");
    ramRow->addWidget(m_impl->ramBudgetBar, 1);
    budgetsLayout->addLayout(ramRow);

    m_impl->targetFpsBadge = new QLabel("Target: <b>60 FPS</b>", budgetsFrame);
    m_impl->targetFpsBadge->setStyleSheet("font-size: 11px; color: #34d399;");
    budgetsLayout->addWidget(m_impl->targetFpsBadge);

    drawerLayout->addWidget(budgetsFrame);

    auto *statusRow = new QHBoxLayout();
    auto *stLbl = new QLabel("Status:", m_impl->inspectorDrawer);
    stLbl->setStyleSheet("font-size: 11px; color: #94a3b8;");
    statusRow->addWidget(stLbl);
    m_impl->installStateBadge = new QLabel("Available", m_impl->inspectorDrawer);
    m_impl->installStateBadge->setStyleSheet("font-size: 11px;");
    statusRow->addWidget(m_impl->installStateBadge, 1);
    drawerLayout->addLayout(statusRow);

    m_impl->downloadProgressBox = new QWidget(m_impl->inspectorDrawer);
    auto *dlLayout = new QVBoxLayout(m_impl->downloadProgressBox);
    dlLayout->setContentsMargins(0, 0, 0, 0);
    dlLayout->setSpacing(4);

    m_impl->downloadProgressBar = new QProgressBar(m_impl->downloadProgressBox);
    m_impl->downloadProgressBar->setRange(0, 100);
    m_impl->downloadProgressBar->setValue(0);
    m_impl->downloadProgressBar->setFixedHeight(12);
    m_impl->downloadProgressBar->setStyleSheet(
        "QProgressBar { background: #1e222b; border: 1px solid #334155; border-radius: 4px; "
        "text-align: center; font-size: 9px; color: white; }"
        "QProgressBar::chunk { background-color: #0284c7; border-radius: 3px; }");
    dlLayout->addWidget(m_impl->downloadProgressBar);

    m_impl->downloadTelemetryLabel =
        new QLabel("Downloading: 0% (0.0 MB/s)", m_impl->downloadProgressBox);
    m_impl->downloadTelemetryLabel->setTextFormat(Qt::PlainText);
    m_impl->downloadTelemetryLabel->setStyleSheet("font-size: 10px; color: #38bdf8;");
    dlLayout->addWidget(m_impl->downloadTelemetryLabel);

    m_impl->downloadProgressBox->setVisible(false);
    drawerLayout->addWidget(m_impl->downloadProgressBox);

    m_impl->installDownloadBtn = new QPushButton("Install", m_impl->inspectorDrawer);
    m_impl->installDownloadBtn->setFixedHeight(30);
    m_impl->installDownloadBtn->setStyleSheet(STYLE_BUTTON_PRIMARY);
    m_impl->installDownloadBtn->setEnabled(false);
    drawerLayout->addWidget(m_impl->installDownloadBtn);

    auto *applyRow = new QHBoxLayout();
    applyRow->setSpacing(8);
    m_impl->monitorTargetCombo = new QComboBox(m_impl->inspectorDrawer);
    m_impl->monitorTargetCombo->addItems({"All Displays", "DP-1", "HDMI-A-1"});
    m_impl->monitorTargetCombo->setFixedHeight(28);
    m_impl->monitorTargetCombo->setStyleSheet(STYLE_TOOLBAR_COMBO);
    applyRow->addWidget(m_impl->monitorTargetCombo, 1);

    m_impl->applyToMonitorBtn = new QPushButton("Apply", m_impl->inspectorDrawer);
    m_impl->applyToMonitorBtn->setFixedHeight(28);
    m_impl->applyToMonitorBtn->setStyleSheet(STYLE_BUTTON_SECONDARY);
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
    bottomStrip->setStyleSheet(
        "background-color: #14171c; border-top: 1px solid #262b36; padding: 2px;");
    auto *bottomLayout = new QHBoxLayout(bottomStrip);
    bottomLayout->setContentsMargins(10, 4, 10, 4);

    m_impl->statusTelemetryLabel = new QLabel("Open Hub Ready", bottomStrip);
    m_impl->statusTelemetryLabel->setTextFormat(Qt::PlainText);
    m_impl->statusTelemetryLabel->setStyleSheet("font-size: 11px; color: #94a3b8;");
    bottomLayout->addWidget(m_impl->statusTelemetryLabel);
    bottomLayout->addStretch();

    rootLayout->addWidget(bottomStrip);
}

void OpenHubBrowserWidget::wireSignals()
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

    connect(m_categoryCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &OpenHubBrowserWidget::setCategoryFilter);

    connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &OpenHubBrowserWidget::setSortOrder);

    connect(m_refreshBtn, &QToolButton::clicked, this, &OpenHubBrowserWidget::refreshFeed);

    connect(m_impl->wizardLaunchBtn, &QToolButton::clicked, this, [this]() {
        auto *wizard = new CreatorPackagingWizard(this);
        wizard->setAttribute(Qt::WA_DeleteOnClose);
        wizard->show();
    });

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
                auto it = m_impl->idToIndex.find(item.workshop_id);
                if (it != m_impl->idToIndex.end()) {
                    m_impl->selectedItemIndex = static_cast<int>(it->second);
                    m_impl->selectedPackageId = m_impl->filteredRecords[it->second].id;
                    m_impl->updateInspector(m_impl->filteredRecords[it->second]);
                }
            });

    connect(m_gridView, &VirtualizedGridWidget::wallpaperDoubleClicked, this,
            [this](const CatalogItem &item) {
                auto it = m_impl->idToIndex.find(item.workshop_id);
                if (it != m_impl->idToIndex.end()) {
                    onItemDoubleClicked(static_cast<int>(it->second));
                }
            });

    connect(m_impl->copyCidBtn, &QPushButton::clicked, this, [this]() {
        if (m_impl->selectedItemIndex >= 0 &&
            static_cast<size_t>(m_impl->selectedItemIndex) < m_impl->filteredRecords.size()) {
            QApplication::clipboard()->setText(
                m_impl->filteredRecords[m_impl->selectedItemIndex].cid);
            m_impl->copyCidBtn->setText("Copied!");
            QPointer<OpenHubBrowserWidget> self(this);
            QTimer::singleShot(1500, this, [self]() {
                if (self && self->m_impl->copyCidBtn) {
                    self->m_impl->copyCidBtn->setText("Copy");
                }
            });
        }
    });

    // Install / Cancel Download toggle
    connect(m_impl->installDownloadBtn, &QPushButton::clicked, this, [this]() {
        if (m_impl->selectedItemIndex < 0 ||
            static_cast<size_t>(m_impl->selectedItemIndex) >= m_impl->filteredRecords.size()) {
            return;
        }

        auto &rec = m_impl->filteredRecords[m_impl->selectedItemIndex];
        if (!m_downloadMgr) {
            return;
        }

        // Cancel active download via cancelOpenHub rather than re-enqueuing
        if (rec.is_downloading) {
            m_downloadMgr->cancelOpenHub(rec.id.toStdString());
            rec.is_downloading = false;
            rec.last_download_cancelled = true;
            rec.last_download_failed = false;

            auto allIt = m_impl->allIdToIndex.find(rec.id.toStdString());
            if (allIt != m_impl->allIdToIndex.end()) {
                m_impl->allRecords[allIt->second].is_downloading = false;
                m_impl->allRecords[allIt->second].last_download_cancelled = true;
                m_impl->allRecords[allIt->second].last_download_failed = false;
            }
            m_impl->updateInspector(rec);
            return;
        }

        if (rec.download_url.isEmpty()) {
            return;
        }

        rec.is_downloading = true;
        rec.last_download_cancelled = false;
        rec.last_download_failed = false;
        rec.last_error_message.clear();

        auto allIt = m_impl->allIdToIndex.find(rec.id.toStdString());
        if (allIt != m_impl->allIdToIndex.end()) {
            m_impl->allRecords[allIt->second].is_downloading = true;
            m_impl->allRecords[allIt->second].last_download_cancelled = false;
            m_impl->allRecords[allIt->second].last_download_failed = false;
        }
        m_impl->updateInspector(rec);

        QPointer<OpenHubBrowserWidget> self(this);
        QString pkgId = rec.id;
        m_downloadMgr->enqueue(pkgId.toStdString(), rec.download_url.toStdString(),
                               TaskPriority::Interactive,
                               [self, pkgId](const DownloadProgress &prog) {
                                   if (!self) {
                                       return;
                                   }
                                   QMetaObject::invokeMethod(
                                       self,
                                       [self, pkgId, prog]() {
                                           if (self) {
                                               self->m_impl->handleDownloadProgress(pkgId, prog);
                                           }
                                       },
                                       Qt::QueuedConnection);
                               });
    });

    // Apply strictly gated behind verified cryptographic signature + local install
    connect(m_impl->applyToMonitorBtn, &QPushButton::clicked, this, [this]() {
        if (m_impl->selectedItemIndex < 0 ||
            static_cast<size_t>(m_impl->selectedItemIndex) >= m_impl->filteredRecords.size()) {
            return;
        }

        const auto &rec = m_impl->filteredRecords[m_impl->selectedItemIndex];
        if (!rec.is_installed || !rec.is_verified || rec.is_downloading) {
            return;
        }

        QString target = m_impl->monitorTargetCombo->currentText();
        emit packageSelected(rec.id, rec.cid);
        emit packageInstallRequested(rec.id, target);
    });
}

void OpenHubBrowserWidget::refreshFeed() { m_impl->launchAsyncScan(); }

void OpenHubBrowserWidget::setFilterQuery(const QString &query)
{
    m_impl->activeQuery = query;
    m_impl->executeQuery();
}

void OpenHubBrowserWidget::setCategoryFilter(int categoryIndex)
{
    m_impl->activeCategoryIndex = categoryIndex;
    m_impl->executeQuery();
}

void OpenHubBrowserWidget::setSortOrder(int sortOrderIndex)
{
    m_impl->activeSortIndex = sortOrderIndex;
    m_impl->executeQuery();
}

void OpenHubBrowserWidget::onItemDoubleClicked(int itemIndex)
{
    if (itemIndex < 0 || static_cast<size_t>(itemIndex) >= m_impl->filteredRecords.size()) {
        return;
    }

    const auto &rec = m_impl->filteredRecords[itemIndex];
    m_impl->selectedItemIndex = itemIndex;
    m_impl->selectedPackageId = rec.id;
    m_impl->updateInspector(rec);

    emit packageSelected(rec.id, rec.cid);

    if (rec.is_installed && rec.is_verified && !rec.is_downloading) {
        emit packageInstallRequested(rec.id, m_impl->monitorTargetCombo->currentText());
    }
}

void OpenHubBrowserWidget::onItemHoverStarted(int itemIndex)
{
    if (itemIndex < 0 || static_cast<size_t>(itemIndex) >= m_impl->filteredRecords.size()) {
        return;
    }

    m_impl->activeHoverIndex = itemIndex;
    const auto &rec = m_impl->filteredRecords[itemIndex];

    if (m_gridView) {
        WallpaperBadge badge = WallpaperBadge::Image;
        if (rec.type == WWPKG_TYPE_VIDEO_STREAM)
            badge = WallpaperBadge::Video;
        else if (rec.type == WWPKG_TYPE_SHADER_FRAGMENT || rec.type == WWPKG_TYPE_SHADER_COMPUTE)
            badge = WallpaperBadge::Shader;
        else if (rec.type >= WWPKG_TYPE_SCENE_2D && rec.type <= WWPKG_TYPE_SCENE_PARTICLES)
            badge = WallpaperBadge::Scene;

        m_gridView->hoverController().onCellHoverEntered(static_cast<uint32_t>(itemIndex + 1),
                                                         rec.local_pkg_path.toStdString(), badge,
                                                         rec.preview_path.toStdString());
    }
}

void OpenHubBrowserWidget::onDownloadProgressUpdated(const QString &packageId, float progressPct,
                                                     float speedBps)
{
    double speedMiB = static_cast<double>(std::max(0.0f, speedBps)) / (1024.0 * 1024.0);
    float clampedPct = std::clamp(progressPct, 0.0f, 100.0f);

    std::string key = packageId.toStdString();
    auto allIt = m_impl->allIdToIndex.find(key);
    if (allIt != m_impl->allIdToIndex.end()) {
        auto &arec = m_impl->allRecords[allIt->second];
        if (progressPct >= 100.0f) {
            arec.is_downloading = false;
            if (!arec.local_pkg_path.isEmpty()) {
                OpenHubRecord verifiedMeta;
                if (parseVerifiedPackageFile(arec.local_pkg_path.toStdString(), verifiedMeta) &&
                    verifiedMeta.is_verified) {
                    arec.is_verified = true;
                    arec.is_installed = true;
                }
            }
        } else {
            arec.is_downloading = true;
            arec.download_pct = clampedPct;
            arec.download_speed_bps = speedBps;
        }
    }

    auto it = m_impl->idToIndex.find(key);
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
        rec.is_downloading = false;
        if (!rec.local_pkg_path.isEmpty()) {
            OpenHubRecord verifiedMeta;
            if (parseVerifiedPackageFile(rec.local_pkg_path.toStdString(), verifiedMeta) &&
                verifiedMeta.is_verified) {
                rec.is_verified = true;
                rec.is_installed = true;
            }
        }
        if (m_impl->selectedItemIndex == static_cast<int>(it->second)) {
            m_impl->downloadProgressBox->setVisible(false);
            m_impl->updateInspector(rec);
        }
    } else {
        rec.is_downloading = true;
        rec.download_pct = clampedPct;
        rec.download_speed_bps = speedBps;

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
                QString("Downloading .wwpkg: %1 / %2 (%3 MB/s, ETA: %4s)")
                    .arg(formatBytes(downloadedBytes))
                    .arg(formatBytes(rec.file_size_bytes))
                    .arg(QString::number(speedMiB, 'f', 1))
                    .arg(etaSec));
        }
    }
}

} // namespace wwal::studio
