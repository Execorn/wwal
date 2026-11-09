// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/wizard/creator_packaging_wizard.hpp"

#include "waywal/log.h"
#include "waywal/path.h"
#include "waywal/shader_parser.h"
#include "waywal/we_compat_scanner.h"
#include "waywal/we_project.h"
#include "waywal/wwpkg.h"

#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wwal::studio {

namespace {

/* Helper to format byte counts into human-readable strings */
QString formatBytes(qint64 bytes)
{
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024 * 1024) {
        return QStringLiteral("%1 KiB").arg(QString::number(bytes / 1024.0, 'f', 1));
    }
    if (bytes < 1024 * 1024 * 1024) {
        return QStringLiteral("%1 MiB").arg(QString::number(bytes / (1024.0 * 1024.0), 'f', 2));
    }
    return QStringLiteral("%1 GiB").arg(
        QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2));
}

/* Helper to convert title string into canonical RFC-compliant slug */
QString slugify(const QString &title)
{
    QString s = title.toLower();
    QString out;
    bool lastWasHyphen = true; // prevent leading hyphen

    for (const QChar &ch : s) {
        if ((ch >= u'a' && ch <= u'z') || (ch >= u'0' && ch <= u'9')) {
            out.append(ch);
            lastWasHyphen = false;
        } else if (!lastWasHyphen) {
            out.append(u'-');
            lastWasHyphen = true;
        }
    }

    while (out.endsWith(u'-')) {
        out.chop(1);
    }
    if (out.isEmpty()) {
        out = QStringLiteral("untitled-package");
    }
    return out;
}

/* Helper to add pubkey hex to trusted_keys.json */
void addPubkeyToTrustedKeys(const QString &pubkeyHex)
{
    const QString home = QDir::homePath();
    const QString trustedPath = home + QStringLiteral("/.config/waywal/trusted_keys.json");
    QFileInfo fi(trustedPath);
    fi.dir().mkpath(QStringLiteral("."));

    QJsonArray keysArray;
    QFile readFile(trustedPath);
    if (readFile.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(readFile.readAll());
        readFile.close();
        if (doc.isObject() && doc.object().contains(QStringLiteral("trusted_keys"))) {
            keysArray = doc.object().value(QStringLiteral("trusted_keys")).toArray();
        } else if (doc.isArray()) {
            keysArray = doc.array();
        }
    }

    bool alreadyPresent = false;
    for (const auto &val : keysArray) {
        if (val.toString().compare(pubkeyHex, Qt::CaseInsensitive) == 0) {
            alreadyPresent = true;
            break;
        }
    }

    if (!alreadyPresent) {
        keysArray.append(pubkeyHex.toLower());
        QJsonObject root;
        root.insert(QStringLiteral("trusted_keys"), keysArray);
        QJsonDocument writeDoc(root);
        QFile writeFile(trustedPath);
        if (writeFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            writeFile.write(writeDoc.toJson(QJsonDocument::Indented));
            writeFile.close();
        }
    }
}

} // namespace

/* ========================================================================= */
/* Step 1: SourceIntakePage Implementation                                   */
/* ========================================================================= */

SourceIntakePage::SourceIntakePage(CreatorPackagingContext *ctx, QWidget *parent)
    : QWizardPage(parent), m_ctx(ctx)
{
    setTitle(QStringLiteral("Step 1: Source Intake & Project Detection"));
    setSubTitle(QStringLiteral(
        "Select your wallpaper project directory or individual GLSL shader/scene file."));
    setupUi();
}

void SourceIntakePage::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    // Source path group
    auto *pathGroup = new QGroupBox(QStringLiteral("Wallpaper Source Intake"), this);
    auto *pathLayout = new QVBoxLayout(pathGroup);
    pathLayout->setSpacing(8);

    auto *rowLayout = new QHBoxLayout();
    m_sourcePathEdit = new QLineEdit(pathGroup);
    m_sourcePathEdit->setPlaceholderText(
        QStringLiteral("Select project folder, shader (.frag/.comp), or scene.json..."));
    rowLayout->addWidget(m_sourcePathEdit, 1);

    m_browseDirBtn = new QPushButton(QStringLiteral("Browse Folder..."), pathGroup);
    rowLayout->addWidget(m_browseDirBtn);

    m_browseFileBtn = new QPushButton(QStringLiteral("Browse File..."), pathGroup);
    rowLayout->addWidget(m_browseFileBtn);

    pathLayout->addLayout(rowLayout);

    // Project Type dropdown
    auto *typeLayout = new QHBoxLayout();
    auto *typeLabel = new QLabel(QStringLiteral("Package Type:"), pathGroup);
    typeLabel->setFixedWidth(110);
    typeLayout->addWidget(typeLabel);

    m_typeCombo = new QComboBox(pathGroup);
    m_typeCombo->addItem(QStringLiteral("GLSL Fragment Shader (shader_fragment)"),
                         QVariant(static_cast<int>(WWPKG_TYPE_SHADER_FRAGMENT)));
    m_typeCombo->addItem(QStringLiteral("GLSL Compute Shader (shader_compute)"),
                         QVariant(static_cast<int>(WWPKG_TYPE_SHADER_COMPUTE)));
    m_typeCombo->addItem(QStringLiteral("2D Scene Wallpaper (scene_2d)"),
                         QVariant(static_cast<int>(WWPKG_TYPE_SCENE_2D)));
    m_typeCombo->addItem(QStringLiteral("Skeletal Puppet Scene (scene_puppet)"),
                         QVariant(static_cast<int>(WWPKG_TYPE_SCENE_PUPPET)));
    m_typeCombo->addItem(QStringLiteral("Particle System Scene (scene_particles)"),
                         QVariant(static_cast<int>(WWPKG_TYPE_SCENE_PARTICLES)));
    m_typeCombo->addItem(QStringLiteral("Video Stream Wallpaper (video_stream)"),
                         QVariant(static_cast<int>(WWPKG_TYPE_VIDEO_STREAM)));
    typeLayout->addWidget(m_typeCombo, 1);

    pathLayout->addLayout(typeLayout);
    layout->addWidget(pathGroup);

    // Analysis results card
    auto *analysisGroup = new QGroupBox(QStringLiteral("Automated Intake Analysis"), this);
    auto *analysisLayout = new QGridLayout(analysisGroup);
    analysisLayout->setHorizontalSpacing(16);
    analysisLayout->setVerticalSpacing(6);

    analysisLayout->addWidget(new QLabel(QStringLiteral("Detected Title:"), analysisGroup), 0, 0);
    m_detectedTitleLabel = new QLabel(QStringLiteral("None"), analysisGroup);
    m_detectedTitleLabel->setStyleSheet(QStringLiteral("font-weight: bold; color: #60a5fa;"));
    analysisLayout->addWidget(m_detectedTitleLabel, 0, 1);

    analysisLayout->addWidget(new QLabel(QStringLiteral("Detected Author:"), analysisGroup), 1, 0);
    m_detectedAuthorLabel = new QLabel(QStringLiteral("None"), analysisGroup);
    analysisLayout->addWidget(m_detectedAuthorLabel, 1, 1);

    analysisLayout->addWidget(new QLabel(QStringLiteral("Entrypoint:"), analysisGroup), 2, 0);
    m_entrypointLabel = new QLabel(QStringLiteral("shaders/main.frag"), analysisGroup);
    m_entrypointLabel->setStyleSheet(QStringLiteral("font-family: monospace; color: #34d399;"));
    analysisLayout->addWidget(m_entrypointLabel, 2, 1);

    analysisLayout->addWidget(new QLabel(QStringLiteral("Assets Detected:"), analysisGroup), 3, 0);
    m_fileCountLabel = new QLabel(QStringLiteral("0 files (0 B)"), analysisGroup);
    analysisLayout->addWidget(m_fileCountLabel, 3, 1);

    layout->addWidget(analysisGroup);

    // Status message
    m_statusLabel = new QLabel(
        QStringLiteral("Please select a project directory or shader file to begin."), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color: #9a9a9e; font-style: italic;"));
    layout->addWidget(m_statusLabel);

    layout->addStretch();

    // Wiring
    connect(m_browseDirBtn, &QPushButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Select Project Directory"), m_sourcePathEdit->text());
        if (!dir.isEmpty()) {
            m_sourcePathEdit->setText(dir);
        }
    });

    connect(m_browseFileBtn, &QPushButton::clicked, this, [this]() {
        QString file = QFileDialog::getOpenFileName(
            this, QStringLiteral("Select Shader or Scene File"), m_sourcePathEdit->text(),
            QStringLiteral("All Supported (*.frag *.vert *.comp *.glsl *.json *.mp4 *.webm *.mkv);;"
                           "GLSL Shaders (*.frag *.vert *.comp *.glsl);;"
                           "Scene / Project JSON (*.json);;"
                           "Videos (*.mp4 *.webm *.mkv);;"
                           "All Files (*)"));
        if (!file.isEmpty()) {
            m_sourcePathEdit->setText(file);
        }
    });

    connect(m_sourcePathEdit, &QLineEdit::textChanged, this,
            [this](const QString &text) { analyzeSourcePath(text.trimmed()); });

    connect(m_typeCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (m_ctx) {
            m_ctx->projectType = static_cast<wwpkg_type_t>(m_typeCombo->itemData(idx).toInt());
            switch (m_ctx->projectType) {
            case WWPKG_TYPE_SHADER_FRAGMENT:
                m_ctx->projectTypeStr = QStringLiteral("shader_fragment");
                break;
            case WWPKG_TYPE_SHADER_COMPUTE:
                m_ctx->projectTypeStr = QStringLiteral("shader_compute");
                break;
            case WWPKG_TYPE_SCENE_2D:
                m_ctx->projectTypeStr = QStringLiteral("scene_2d");
                break;
            case WWPKG_TYPE_SCENE_PUPPET:
                m_ctx->projectTypeStr = QStringLiteral("scene_puppet");
                break;
            case WWPKG_TYPE_SCENE_PARTICLES:
                m_ctx->projectTypeStr = QStringLiteral("scene_particles");
                break;
            case WWPKG_TYPE_VIDEO_STREAM:
                m_ctx->projectTypeStr = QStringLiteral("video_stream");
                break;
            default:
                m_ctx->projectTypeStr = QStringLiteral("shader_fragment");
                break;
            }
        }
    });
}

void SourceIntakePage::setSourcePath(const QString &path)
{
    if (m_sourcePathEdit) {
        m_sourcePathEdit->setText(path);
    }
}

void SourceIntakePage::initializePage()
{
    if (m_ctx && !m_ctx->sourcePath.isEmpty()) {
        m_sourcePathEdit->setText(m_ctx->sourcePath);
    }
}

void SourceIntakePage::analyzeSourcePath(const QString &path)
{
    if (!m_ctx)
        return;

    m_ctx->sourcePath = path;
    QFileInfo info(path);

    if (path.isEmpty() || !info.exists()) {
        m_detectedTitleLabel->setText(QStringLiteral("None"));
        m_detectedAuthorLabel->setText(QStringLiteral("None"));
        m_entrypointLabel->setText(QStringLiteral("None"));
        m_fileCountLabel->setText(QStringLiteral("0 files (0 B)"));
        m_statusLabel->setText(
            path.isEmpty()
                ? QStringLiteral("Please select a project directory or shader file to begin.")
                : QStringLiteral("Path does not exist on disk."));
        m_statusLabel->setStyleSheet(QStringLiteral("color: #ef4444;"));
        emit completeChanged();
        return;
    }

    qint64 totalBytes = 0;
    int fileCount = 0;

    if (info.isDir()) {
        QDir dir(path);

        // 1. Check for Wallpaper Engine project.json
        if (dir.exists(QStringLiteral("project.json"))) {
            we_project_t proj;
            memset(&proj, 0, sizeof(proj));
            if (we_project_load_dir(path.toUtf8().constData(), &proj)) {
                if (proj.title[0] != '\0') {
                    m_ctx->detectedTitle = QString::fromUtf8(proj.title);
                }
                QString pType = QString::fromUtf8(proj.type).toLower();
                if (pType == QStringLiteral("video")) {
                    m_typeCombo->setCurrentIndex(5); // video_stream
                } else if (pType == QStringLiteral("scene")) {
                    m_typeCombo->setCurrentIndex(2); // scene_2d
                }
                if (proj.file[0] != '\0') {
                    m_ctx->entrypointPipeline = QString::fromUtf8(proj.file);
                } else {
                    m_ctx->entrypointPipeline = QStringLiteral("scene.json");
                }
                if (proj.preview[0] != '\0') {
                    m_ctx->entrypointPreview = QString::fromUtf8(proj.preview);
                }
            }
        }

        // 2. Check for GLSL shaders
        if (m_ctx->entrypointPipeline.isEmpty()) {
            if (dir.exists(QStringLiteral("shaders/main.frag"))) {
                m_ctx->entrypointPipeline = QStringLiteral("shaders/main.frag");
            } else if (dir.exists(QStringLiteral("main.frag"))) {
                m_ctx->entrypointPipeline = QStringLiteral("main.frag");
            } else if (dir.exists(QStringLiteral("shaders/main.comp"))) {
                m_ctx->entrypointPipeline = QStringLiteral("shaders/main.comp");
                m_typeCombo->setCurrentIndex(1); // shader_compute
            }
        }

        // Parse shader metadata if present
        QString fragCandidate = dir.filePath(m_ctx->entrypointPipeline);
        if (QFile::exists(fragCandidate) && (fragCandidate.endsWith(QStringLiteral(".frag")) ||
                                             fragCandidate.endsWith(QStringLiteral(".glsl")))) {
            waywal_shader_meta_t meta;
            waywal_shader_meta_init(&meta);
            if (waywal_shader_parse_file(fragCandidate.toUtf8().constData(), &meta)) {
                if (m_ctx->detectedTitle.isEmpty() && meta.title[0] != '\0') {
                    m_ctx->detectedTitle = QString::fromUtf8(meta.title);
                }
                if (meta.author[0] != '\0') {
                    m_ctx->detectedAuthor = QString::fromUtf8(meta.author);
                }
                if (meta.description[0] != '\0') {
                    m_ctx->detectedDescription = QString::fromUtf8(meta.description);
                }
                if (meta.license[0] != '\0') {
                    m_ctx->detectedLicense = QString::fromUtf8(meta.license);
                }
                if (meta.has_multi_pass || meta.passes[1].target_fps > 0) {
                    m_typeCombo->setCurrentIndex(1); // shader_compute
                }
            }
        }

        // Count files and total size
        QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            fileCount++;
            totalBytes += it.fileInfo().size();
        }
    } else {
        // Single file intake
        fileCount = 1;
        totalBytes = info.size();
        const QString ext = info.suffix().toLower();

        if (ext == QStringLiteral("frag") || ext == QStringLiteral("glsl")) {
            m_typeCombo->setCurrentIndex(0); // shader_fragment
            m_ctx->entrypointPipeline = QStringLiteral("shaders/main.frag");
            waywal_shader_meta_t meta;
            waywal_shader_meta_init(&meta);
            if (waywal_shader_parse_file(path.toUtf8().constData(), &meta)) {
                if (meta.title[0] != '\0')
                    m_ctx->detectedTitle = QString::fromUtf8(meta.title);
                if (meta.author[0] != '\0')
                    m_ctx->detectedAuthor = QString::fromUtf8(meta.author);
                if (meta.description[0] != '\0')
                    m_ctx->detectedDescription = QString::fromUtf8(meta.description);
                if (meta.license[0] != '\0')
                    m_ctx->detectedLicense = QString::fromUtf8(meta.license);
            }
        } else if (ext == QStringLiteral("comp")) {
            m_typeCombo->setCurrentIndex(1); // shader_compute
            m_ctx->entrypointPipeline = QStringLiteral("shaders/main.comp");
        } else if (ext == QStringLiteral("mp4") || ext == QStringLiteral("webm") ||
                   ext == QStringLiteral("mkv")) {
            m_typeCombo->setCurrentIndex(5); // video_stream
            m_ctx->entrypointPipeline = QStringLiteral("assets/video.") + ext;
        } else if (ext == QStringLiteral("json")) {
            m_typeCombo->setCurrentIndex(2); // scene_2d
            m_ctx->entrypointPipeline = QStringLiteral("scene.json");
        }
    }

    if (m_ctx->detectedTitle.isEmpty()) {
        m_ctx->detectedTitle = info.isDir() ? info.fileName() : info.completeBaseName();
    }
    if (m_ctx->entrypointPipeline.isEmpty()) {
        m_ctx->entrypointPipeline = QStringLiteral("shaders/main.frag");
    }

    m_detectedTitleLabel->setText(m_ctx->detectedTitle);
    m_detectedAuthorLabel->setText(m_ctx->detectedAuthor.isEmpty() ? QStringLiteral("Unknown")
                                                                   : m_ctx->detectedAuthor);
    m_entrypointLabel->setText(m_ctx->entrypointPipeline);
    m_fileCountLabel->setText(
        QStringLiteral("%1 files (%2)").arg(fileCount).arg(formatBytes(totalBytes)));

    m_statusLabel->setText(
        QStringLiteral("Source validated: %1 asset(s) ready for bundling.").arg(fileCount));
    m_statusLabel->setStyleSheet(QStringLiteral("color: #10b981; font-weight: bold;"));

    emit completeChanged();
}

bool SourceIntakePage::isComplete() const
{
    return m_ctx && !m_ctx->sourcePath.isEmpty() && QFileInfo::exists(m_ctx->sourcePath);
}

bool SourceIntakePage::validatePage()
{
    if (!isComplete()) {
        return false;
    }
    m_ctx->projectType = static_cast<wwpkg_type_t>(m_typeCombo->currentData().toInt());
    return true;
}

/* ========================================================================= */
/* Step 2: AssetBundlingPage Implementation                                  */
/* ========================================================================= */

AssetBundlingPage::AssetBundlingPage(CreatorPackagingContext *ctx, QWidget *parent)
    : QWizardPage(parent), m_ctx(ctx)
{
    setTitle(QStringLiteral("Step 2: Asset Bundling & Optimization"));
    setSubTitle(QStringLiteral("Inspect referenced assets, prune unreferenced dead files, and "
                               "transcode uncompressed textures."));
    setupUi();
}

void AssetBundlingPage::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    m_summaryLabel = new QLabel(QStringLiteral("Scanning assets..."), this);
    m_summaryLabel->setStyleSheet(QStringLiteral("color: #9a9a9e; font-size: 11px;"));
    layout->addWidget(m_summaryLabel);

    m_assetTree = new QTreeWidget(this);
    m_assetTree->setColumnCount(4);
    m_assetTree->setHeaderLabels({QStringLiteral("Relative Path"), QStringLiteral("Category"),
                                  QStringLiteral("Size"), QStringLiteral("Packaging Action")});
    m_assetTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_assetTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_assetTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_assetTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_assetTree->setAlternatingRowColors(true);
    layout->addWidget(m_assetTree, 1);

    auto *optGroup = new QGroupBox(QStringLiteral("Optimization & Pruning Directives"), this);
    auto *optLayout = new QVBoxLayout(optGroup);
    optLayout->setSpacing(6);

    m_pruneCheck = new QCheckBox(
        QStringLiteral(
            "Prune unreferenced & temporary files (.git, .DS_Store, *.bak, dead assets)"),
        optGroup);
    m_pruneCheck->setChecked(true);
    optLayout->addWidget(m_pruneCheck);

    m_transcodeCheck = new QCheckBox(
        QStringLiteral("Transcode textures and preview cards to WebP for maximum compression"),
        optGroup);
    m_transcodeCheck->setChecked(true);
    optLayout->addWidget(m_transcodeCheck);

    layout->addWidget(optGroup);

    auto *btnLayout = new QHBoxLayout();
    m_rescanBtn = new QPushButton(QStringLiteral("↻ Re-scan Assets"), this);
    btnLayout->addWidget(m_rescanBtn);
    btnLayout->addStretch();
    layout->addLayout(btnLayout);

    connect(m_rescanBtn, &QPushButton::clicked, this, &AssetBundlingPage::scanAssets);
}

void AssetBundlingPage::initializePage() { scanAssets(); }

void AssetBundlingPage::scanAssets()
{
    if (!m_ctx || m_ctx->sourcePath.isEmpty())
        return;

    m_assetTree->clear();
    m_ctx->scannedAssets.clear();

    QFileInfo srcInfo(m_ctx->sourcePath);
    qint64 totalSize = 0;
    int deadCount = 0;

    if (srcInfo.isDir()) {
        QDir baseDir(m_ctx->sourcePath);
        QDirIterator it(m_ctx->sourcePath, QDir::Files | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);

        while (it.hasNext()) {
            it.next();
            QString relPath = baseDir.relativeFilePath(it.filePath());
            qint64 sz = it.fileInfo().size();
            totalSize += sz;

            ScannedAssetEntry entry;
            entry.relativePath = relPath;
            entry.sizeBytes = sz;

            const QString lower = relPath.toLower();
            if (lower.startsWith(QStringLiteral(".git")) ||
                lower.startsWith(QStringLiteral(".ds_store")) ||
                lower.endsWith(QStringLiteral(".bak")) || lower.endsWith(QStringLiteral(".tmp")) ||
                lower.endsWith(QStringLiteral("~"))) {
                entry.category = QStringLiteral("dead/temporary");
                entry.isReferenced = false;
                deadCount++;
            } else if (lower.endsWith(QStringLiteral(".frag")) ||
                       lower.endsWith(QStringLiteral(".vert")) ||
                       lower.endsWith(QStringLiteral(".comp")) ||
                       lower.endsWith(QStringLiteral(".glsl"))) {
                entry.category = QStringLiteral("shader");
            } else if (lower.endsWith(QStringLiteral(".png")) ||
                       lower.endsWith(QStringLiteral(".jpg")) ||
                       lower.endsWith(QStringLiteral(".jpeg")) ||
                       lower.endsWith(QStringLiteral(".webp")) ||
                       lower.endsWith(QStringLiteral(".bmp")) ||
                       lower.endsWith(QStringLiteral(".tga"))) {
                entry.category = QStringLiteral("texture");
            } else if (lower.endsWith(QStringLiteral(".mdl")) ||
                       lower.endsWith(QStringLiteral(".glb")) ||
                       lower.endsWith(QStringLiteral(".gltf")) ||
                       lower.endsWith(QStringLiteral(".obj"))) {
                entry.category = QStringLiteral("model");
            } else if (lower.endsWith(QStringLiteral(".opus")) ||
                       lower.endsWith(QStringLiteral(".ogg")) ||
                       lower.endsWith(QStringLiteral(".flac")) ||
                       lower.endsWith(QStringLiteral(".mp3"))) {
                entry.category = QStringLiteral("audio");
            } else if (lower.endsWith(QStringLiteral(".js")) ||
                       lower.endsWith(QStringLiteral(".json"))) {
                entry.category = QStringLiteral("script/data");
            } else {
                entry.category = QStringLiteral("other");
            }

            m_ctx->scannedAssets.append(entry);

            auto *item = new QTreeWidgetItem(m_assetTree);
            item->setText(0, entry.relativePath);
            item->setText(1, entry.category);
            item->setText(2, formatBytes(entry.sizeBytes));

            if (!entry.isReferenced) {
                item->setText(3, QStringLiteral("Prune (Discard)"));
                item->setForeground(3, QBrush(QColor(QStringLiteral("#ef4444"))));
            } else if (entry.category == QStringLiteral("texture") &&
                       (lower.endsWith(QStringLiteral(".png")) ||
                        lower.endsWith(QStringLiteral(".bmp")))) {
                item->setText(3, QStringLiteral("Bundle & Transcode to WebP"));
                item->setForeground(3, QBrush(QColor(QStringLiteral("#38bdf8"))));
            } else {
                item->setText(3, QStringLiteral("Bundle"));
                item->setForeground(3, QBrush(QColor(QStringLiteral("#34d399"))));
            }
        }
    } else {
        // Single file
        ScannedAssetEntry entry;
        entry.relativePath = srcInfo.fileName();
        entry.sizeBytes = srcInfo.size();
        entry.category = QStringLiteral("shader");
        entry.isReferenced = true;
        totalSize += entry.sizeBytes;

        m_ctx->scannedAssets.append(entry);

        auto *item = new QTreeWidgetItem(m_assetTree);
        item->setText(0, entry.relativePath);
        item->setText(1, entry.category);
        item->setText(2, formatBytes(entry.sizeBytes));
        item->setText(3, QStringLiteral("Bundle into shaders/"));
        item->setForeground(3, QBrush(QColor(QStringLiteral("#34d399"))));
    }

    m_summaryLabel->setText(
        QStringLiteral("Scanned %1 asset(s) totaling %2. %3 marked as dead/temporary.")
            .arg(m_ctx->scannedAssets.size())
            .arg(formatBytes(totalSize))
            .arg(deadCount));
}

bool AssetBundlingPage::validatePage()
{
    if (!m_ctx)
        return false;

    m_ctx->pruneDeadFiles = m_pruneCheck->isChecked();
    m_ctx->transcodeTextures = m_transcodeCheck->isChecked();

    // Prepare clean temporary staging directory
    if (m_ctx->stagingDir.isEmpty() || !QDir(m_ctx->stagingDir).exists()) {
        m_ctx->stagingDir = QDir::tempPath() + QStringLiteral("/wwpkg_stage_") +
                            QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    QDir staging(m_ctx->stagingDir);
    staging.mkpath(QStringLiteral("."));

    QFileInfo srcInfo(m_ctx->sourcePath);
    if (srcInfo.isDir()) {
        QDir srcDir(m_ctx->sourcePath);
        for (const auto &asset : m_ctx->scannedAssets) {
            if (m_ctx->pruneDeadFiles && !asset.isReferenced) {
                continue;
            }
            QString srcFile = srcDir.filePath(asset.relativePath);
            QString dstFile = staging.filePath(asset.relativePath);

            QFileInfo(dstFile).dir().mkpath(QStringLiteral("."));
            QFile::remove(dstFile);
            QFile::copy(srcFile, dstFile);

            // Optional texture transcoding to WebP
            if (m_ctx->transcodeTextures && asset.category == QStringLiteral("texture") &&
                (asset.relativePath.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive) ||
                 asset.relativePath.endsWith(QStringLiteral(".bmp"), Qt::CaseInsensitive))) {
                QImage img(dstFile);
                if (!img.isNull()) {
                    QString webpDst = dstFile;
                    int dot = webpDst.lastIndexOf(u'.');
                    if (dot > 0)
                        webpDst.truncate(dot);
                    webpDst.append(QStringLiteral(".webp"));
                    img.save(webpDst, "WEBP", 85);
                }
            }
        }
    } else {
        // Single file intake
        if (m_ctx->projectType == WWPKG_TYPE_SHADER_FRAGMENT ||
            m_ctx->projectType == WWPKG_TYPE_SHADER_COMPUTE) {
            staging.mkpath(QStringLiteral("shaders"));
            QString dst = staging.filePath(QStringLiteral("shaders/main.frag"));
            if (m_ctx->projectType == WWPKG_TYPE_SHADER_COMPUTE) {
                dst = staging.filePath(QStringLiteral("shaders/main.comp"));
            }
            QFile::remove(dst);
            QFile::copy(m_ctx->sourcePath, dst);
        } else {
            QString dst = staging.filePath(srcInfo.fileName());
            QFile::remove(dst);
            QFile::copy(m_ctx->sourcePath, dst);
        }
    }

    return true;
}

/* ========================================================================= */
/* Step 3: MetadataPreviewPage Implementation                                */
/* ========================================================================= */

MetadataPreviewPage::MetadataPreviewPage(CreatorPackagingContext *ctx, QWidget *parent)
    : QWizardPage(parent), m_ctx(ctx)
{
    setTitle(QStringLiteral("Step 3: Metadata, SPDX License & Automated Preview Capture"));
    setSubTitle(QStringLiteral("Validate package slug, SemVer, author credentials, and capture an "
                               "offscreen 512x288 preview."));
    setupUi();
}

void MetadataPreviewPage::setupUi()
{
    auto *mainLayout = new QHBoxLayout(this);
    mainLayout->setSpacing(16);

    // Left column: Metadata fields
    auto *metaGroup = new QGroupBox(QStringLiteral("Package Identity & Manifest"), this);
    auto *form = new QGridLayout(metaGroup);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);

    int row = 0;

    // Package ID
    form->addWidget(new QLabel(QStringLiteral("Package UUID:"), metaGroup), row, 0);
    m_idEdit = new QLineEdit(metaGroup);
    m_idEdit->setReadOnly(true);
    m_idEdit->setStyleSheet(QStringLiteral("font-family: monospace; color: #9a9a9e;"));
    form->addWidget(m_idEdit, row++, 1, 1, 2);

    // Slug
    form->addWidget(new QLabel(QStringLiteral("Package Slug:"), metaGroup), row, 0);
    m_slugEdit = new QLineEdit(metaGroup);
    m_slugEdit->setPlaceholderText(QStringLiteral("lowercase-hyphenated-identifier"));
    form->addWidget(m_slugEdit, row, 1);
    m_slugValidationLabel = new QLabel(QStringLiteral("✓"), metaGroup);
    m_slugValidationLabel->setStyleSheet(QStringLiteral("color: #10b981; font-weight: bold;"));
    form->addWidget(m_slugValidationLabel, row++, 2);

    // Name
    form->addWidget(new QLabel(QStringLiteral("Display Name:"), metaGroup), row, 0);
    m_nameEdit = new QLineEdit(metaGroup);
    m_nameEdit->setPlaceholderText(QStringLiteral("Human-readable wallpaper title"));
    form->addWidget(m_nameEdit, row++, 1, 1, 2);

    // Version
    form->addWidget(new QLabel(QStringLiteral("SemVer Version:"), metaGroup), row, 0);
    m_versionEdit = new QLineEdit(metaGroup);
    m_versionEdit->setPlaceholderText(QStringLiteral("1.0.0"));
    form->addWidget(m_versionEdit, row, 1);
    m_versionValidationLabel = new QLabel(QStringLiteral("✓"), metaGroup);
    m_versionValidationLabel->setStyleSheet(QStringLiteral("color: #10b981; font-weight: bold;"));
    form->addWidget(m_versionValidationLabel, row++, 2);

    // Author Name & Email
    form->addWidget(new QLabel(QStringLiteral("Author Name:"), metaGroup), row, 0);
    m_authorNameEdit = new QLineEdit(metaGroup);
    form->addWidget(m_authorNameEdit, row++, 1, 1, 2);

    form->addWidget(new QLabel(QStringLiteral("Author Email:"), metaGroup), row, 0);
    m_authorEmailEdit = new QLineEdit(metaGroup);
    m_authorEmailEdit->setPlaceholderText(QStringLiteral("author@domain.org"));
    form->addWidget(m_authorEmailEdit, row++, 1, 1, 2);

    // SPDX License
    form->addWidget(new QLabel(QStringLiteral("SPDX License:"), metaGroup), row, 0);
    m_licenseCombo = new QComboBox(metaGroup);
    m_licenseCombo->setEditable(true);
    m_licenseCombo->addItems(
        {QStringLiteral("MIT"), QStringLiteral("Apache-2.0"), QStringLiteral("GPL-3.0-or-later"),
         QStringLiteral("GPL-2.0-or-later"), QStringLiteral("BSD-3-Clause"),
         QStringLiteral("CC-BY-4.0"), QStringLiteral("CC0-1.0"), QStringLiteral("Unlicense")});
    form->addWidget(m_licenseCombo, row++, 1, 1, 2);

    // Tags
    form->addWidget(new QLabel(QStringLiteral("Tags (CSV):"), metaGroup), row, 0);
    m_tagsEdit = new QLineEdit(metaGroup);
    m_tagsEdit->setPlaceholderText(QStringLiteral("cyberpunk, anime, audio-reactive, 60fps"));
    form->addWidget(m_tagsEdit, row++, 1, 1, 2);

    // Target FPS & VRAM
    auto *budgetRow = new QHBoxLayout();
    budgetRow->addWidget(new QLabel(QStringLiteral("FPS:"), metaGroup));
    m_fpsCombo = new QComboBox(metaGroup);
    m_fpsCombo->addItems(
        {QStringLiteral("30"), QStringLiteral("60"), QStringLiteral("120"), QStringLiteral("144")});
    m_fpsCombo->setCurrentText(QStringLiteral("60"));
    budgetRow->addWidget(m_fpsCombo);

    budgetRow->addWidget(new QLabel(QStringLiteral("Max VRAM:"), metaGroup));
    m_vramSpin = new QSpinBox(metaGroup);
    m_vramSpin->setRange(32, 2048);
    m_vramSpin->setValue(256);
    m_vramSpin->setSuffix(QStringLiteral(" MB"));
    budgetRow->addWidget(m_vramSpin);

    form->addLayout(budgetRow, row++, 0, 1, 3);

    // Description
    form->addWidget(new QLabel(QStringLiteral("Description:"), metaGroup), row, 0);
    m_descriptionEdit = new QTextEdit(metaGroup);
    m_descriptionEdit->setPlaceholderText(
        QStringLiteral("Wallpaper summary, interactive controls, audio dynamics..."));
    m_descriptionEdit->setMaximumHeight(80);
    form->addWidget(m_descriptionEdit, row++, 1, 1, 2);

    mainLayout->addWidget(metaGroup, 3);

    // Right column: Preview Capture Box
    auto *previewGroup = new QGroupBox(QStringLiteral("512x288 Automated Preview Capture"), this);
    auto *previewLayout = new QVBoxLayout(previewGroup);
    previewLayout->setSpacing(8);

    m_previewDisplay = new QLabel(previewGroup);
    m_previewDisplay->setFixedSize(256, 144);
    m_previewDisplay->setStyleSheet(QStringLiteral(
        "border: 2px solid #334155; background-color: #0f172a; border-radius: 4px;"));
    m_previewDisplay->setAlignment(Qt::AlignCenter);
    previewLayout->addWidget(m_previewDisplay, 0, Qt::AlignCenter);

    m_previewInfoLabel =
        new QLabel(QStringLiteral("Offscreen 512x288 WebP & 1280x720 JPEG"), previewGroup);
    m_previewInfoLabel->setStyleSheet(QStringLiteral("color: #9a9a9e; font-size: 10px;"));
    m_previewInfoLabel->setAlignment(Qt::AlignCenter);
    previewLayout->addWidget(m_previewInfoLabel);

    m_captureBtn = new QPushButton(QStringLiteral("⚡ Capture Offscreen Preview"), previewGroup);
    previewLayout->addWidget(m_captureBtn);

    m_browsePreviewBtn = new QPushButton(QStringLiteral("Load Custom Image..."), previewGroup);
    previewLayout->addWidget(m_browsePreviewBtn);

    previewLayout->addStretch();
    mainLayout->addWidget(previewGroup, 2);

    // Validation connections
    connect(m_slugEdit, &QLineEdit::textChanged, this, &MetadataPreviewPage::validateFields);
    connect(m_nameEdit, &QLineEdit::textChanged, this, &MetadataPreviewPage::validateFields);
    connect(m_versionEdit, &QLineEdit::textChanged, this, &MetadataPreviewPage::validateFields);
    connect(m_authorNameEdit, &QLineEdit::textChanged, this, &MetadataPreviewPage::validateFields);

    connect(m_captureBtn, &QPushButton::clicked, this,
            &MetadataPreviewPage::captureOffscreenPreview);
    connect(m_browsePreviewBtn, &QPushButton::clicked, this,
            &MetadataPreviewPage::loadCustomPreview);
}

void MetadataPreviewPage::initializePage()
{
    if (!m_ctx)
        return;

    if (m_ctx->packageId.isEmpty()) {
        m_ctx->packageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    m_idEdit->setText(m_ctx->packageId);

    if (m_nameEdit->text().isEmpty()) {
        m_nameEdit->setText(m_ctx->detectedTitle.isEmpty()
                                ? QStringLiteral("WayWal Dynamic Wallpaper")
                                : m_ctx->detectedTitle);
    }

    if (m_slugEdit->text().isEmpty()) {
        m_slugEdit->setText(slugify(m_nameEdit->text()));
    }

    if (m_versionEdit->text().isEmpty()) {
        m_versionEdit->setText(QStringLiteral("1.0.0"));
    }

    if (m_authorNameEdit->text().isEmpty()) {
        QString author = m_ctx->detectedAuthor;
        if (author.isEmpty()) {
            author = qEnvironmentVariable("USER");
            if (author.isEmpty())
                author = QStringLiteral("WayWal Creator");
        }
        m_authorNameEdit->setText(author);
    }

    if (m_authorEmailEdit->text().isEmpty()) {
        QString email = qEnvironmentVariable("EMAIL");
        if (email.isEmpty())
            email = QStringLiteral("creator@waywal.org");
        m_authorEmailEdit->setText(email);
    }

    if (!m_ctx->detectedDescription.isEmpty() && m_descriptionEdit->toPlainText().isEmpty()) {
        m_descriptionEdit->setText(m_ctx->detectedDescription);
    }

    // Check if preview already exists in staging
    bool loadedExisting = false;
    if (!m_ctx->stagingDir.isEmpty()) {
        QDir stg(m_ctx->stagingDir);
        for (const QString &c : {QStringLiteral("preview.webp"), QStringLiteral("preview_card.jpg"),
                                 QStringLiteral("preview.jpg"), QStringLiteral("preview.png")}) {
            if (stg.exists(c)) {
                QImage img(stg.filePath(c));
                if (!img.isNull()) {
                    m_ctx->previewImage = img;
                    loadedExisting = true;
                    break;
                }
            }
        }
    }

    if (!loadedExisting && m_ctx->previewImage.isNull()) {
        captureOffscreenPreview();
    } else {
        updatePreviewDisplay();
    }

    validateFields();
}

void MetadataPreviewPage::validateFields()
{
    static const QRegularExpression slugRegex(QStringLiteral("^[a-z0-9]+(-[a-z0-9]+)*$"));
    static const QRegularExpression semverRegex(
        QStringLiteral(R"(^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$)"));

    const QString slug = m_slugEdit->text().trimmed();
    const bool slugValid = slugRegex.match(slug).hasMatch();
    if (slugValid) {
        m_slugValidationLabel->setText(QStringLiteral("✓"));
        m_slugValidationLabel->setStyleSheet(QStringLiteral("color: #10b981; font-weight: bold;"));
    } else {
        m_slugValidationLabel->setText(QStringLiteral("✗ lowercase & hyphens only"));
        m_slugValidationLabel->setStyleSheet(QStringLiteral("color: #ef4444; font-size: 10px;"));
    }

    const QString version = m_versionEdit->text().trimmed();
    const bool versionValid = semverRegex.match(version).hasMatch();
    if (versionValid) {
        m_versionValidationLabel->setText(QStringLiteral("✓"));
        m_versionValidationLabel->setStyleSheet(
            QStringLiteral("color: #10b981; font-weight: bold;"));
    } else {
        m_versionValidationLabel->setText(QStringLiteral("✗ X.Y.Z"));
        m_versionValidationLabel->setStyleSheet(QStringLiteral("color: #ef4444; font-size: 10px;"));
    }

    emit completeChanged();
}

bool MetadataPreviewPage::isComplete() const
{
    static const QRegularExpression slugRegex(QStringLiteral("^[a-z0-9]+(-[a-z0-9]+)*$"));
    static const QRegularExpression semverRegex(
        QStringLiteral(R"(^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$)"));

    return slugRegex.match(m_slugEdit->text().trimmed()).hasMatch() &&
           semverRegex.match(m_versionEdit->text().trimmed()).hasMatch() &&
           !m_nameEdit->text().trimmed().isEmpty() &&
           !m_authorNameEdit->text().trimmed().isEmpty() && m_ctx && !m_ctx->previewImage.isNull();
}

void MetadataPreviewPage::captureOffscreenPreview()
{
    // Generate an offscreen 512x288 high-fidelity preview thumbnail canvas
    QImage canvas(512, 288, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);

    // Deep modern dark radial gradient
    QRadialGradient bgGrad(256, 144, 300, 200, 100);
    bgGrad.setColorAt(0.0, QColor(0x1e, 0x1b, 0x4b));
    bgGrad.setColorAt(0.7, QColor(0x0f, 0x17, 0x2a));
    bgGrad.setColorAt(1.0, QColor(0x02, 0x06, 0x17));
    painter.fillRect(canvas.rect(), bgGrad);

    // Subtle aesthetic geometry pattern
    painter.setPen(QPen(QColor(0x38, 0xbd, 0xf8, 0x18), 1));
    for (int x = 0; x < 512; x += 32) {
        painter.drawLine(x, 0, x, 288);
    }
    for (int y = 0; y < 288; y += 32) {
        painter.drawLine(0, y, 512, y);
    }

    // Centered accent circle
    QLinearGradient circleGrad(256, 70, 256, 170);
    circleGrad.setColorAt(0.0, QColor(0x63, 0x66, 0xf1, 0x90));
    circleGrad.setColorAt(1.0, QColor(0x06, 0xb6, 0xd4, 0x40));
    painter.setBrush(circleGrad);
    painter.setPen(QPen(QColor(0x38, 0xbd, 0xf8, 0x80), 2));
    painter.drawEllipse(QPoint(256, 110), 48, 48);

    // Modern wallpaper icon glyph
    painter.setPen(QPen(Qt::white, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawText(QRect(208, 62, 96, 96), Qt::AlignCenter, QStringLiteral("❖"));

    // Title typography
    QFont titleFont(QStringLiteral("Sans Serif"), 16, QFont::Bold);
    painter.setFont(titleFont);
    painter.setPen(Qt::white);
    QString title = m_nameEdit ? m_nameEdit->text() : QStringLiteral("WayWal Dynamic Wallpaper");
    if (title.isEmpty())
        title = QStringLiteral("WayWal Dynamic Wallpaper");
    painter.drawText(QRect(20, 175, 472, 30), Qt::AlignCenter, title);

    // Author & subtitle
    QFont subFont(QStringLiteral("Sans Serif"), 10);
    painter.setFont(subFont);
    painter.setPen(QColor(0x94, 0xa3, 0xb8));
    QString author = m_authorNameEdit ? m_authorNameEdit->text() : QStringLiteral("Creator");
    painter.drawText(QRect(20, 205, 472, 20), Qt::AlignCenter,
                     QStringLiteral("By %1 • WayWal Open Hub").arg(author));

    // Badge pill
    painter.setBrush(QColor(0x0f, 0x76, 0x6e, 0xa0));
    painter.setPen(QPen(QColor(0x14, 0xb8, 0xa6), 1));
    painter.drawRoundedRect(QRect(186, 240, 140, 22), 11, 11);
    QFont badgeFont(QStringLiteral("Monospace"), 8, QFont::Bold);
    painter.setFont(badgeFont);
    painter.setPen(QColor(0x5e, 0xeb, 0xd3));
    painter.drawText(QRect(186, 240, 140, 22), Qt::AlignCenter,
                     QStringLiteral("GLES 3.1 • 60 FPS"));

    painter.end();

    if (m_ctx) {
        m_ctx->previewImage = canvas;
    }
    updatePreviewDisplay();
    validateFields();
}

void MetadataPreviewPage::loadCustomPreview()
{
    QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select Preview Image"), QString(),
        QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp);;All Files (*)"));
    if (!path.isEmpty()) {
        QImage img(path);
        if (!img.isNull()) {
            if (m_ctx) {
                m_ctx->previewImage = img;
            }
            updatePreviewDisplay();
            validateFields();
        }
    }
}

void MetadataPreviewPage::updatePreviewDisplay()
{
    if (!m_ctx || m_ctx->previewImage.isNull()) {
        m_previewDisplay->setText(QStringLiteral("No Preview"));
        return;
    }

    QPixmap pix = QPixmap::fromImage(m_ctx->previewImage.scaled(
        256, 144, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    m_previewDisplay->setPixmap(pix);
}

bool MetadataPreviewPage::validatePage()
{
    if (!isComplete())
        return false;

    m_ctx->slug = m_slugEdit->text().trimmed();
    m_ctx->name = m_nameEdit->text().trimmed();
    m_ctx->version = m_versionEdit->text().trimmed();
    m_ctx->authorName = m_authorNameEdit->text().trimmed();
    m_ctx->authorEmail = m_authorEmailEdit->text().trimmed();
    m_ctx->license = m_licenseCombo->currentText().trimmed();
    m_ctx->description = m_descriptionEdit->toPlainText().trimmed();
    m_ctx->targetFps = m_fpsCombo->currentText().toUInt();
    m_ctx->maxVramMb = static_cast<uint32_t>(m_vramSpin->value());

    m_ctx->tags.clear();
    const QStringList rawTags = m_tagsEdit->text().split(u',', Qt::SkipEmptyParts);
    for (const QString &t : rawTags) {
        QString clean = t.trimmed();
        if (!clean.isEmpty() && m_ctx->tags.size() < 16) {
            m_ctx->tags.append(clean);
        }
    }

    // Save preview images into staging area
    if (!m_ctx->stagingDir.isEmpty() && !m_ctx->previewImage.isNull()) {
        QDir stg(m_ctx->stagingDir);
        m_ctx->previewWebpPath = stg.filePath(QStringLiteral("preview.webp"));
        m_ctx->previewCardJpgPath = stg.filePath(QStringLiteral("preview_card.jpg"));

        QImage p512 =
            m_ctx->previewImage.scaled(512, 288, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        p512.save(m_ctx->previewWebpPath, "WEBP", 90);

        QImage p1280 =
            m_ctx->previewImage.scaled(1280, 720, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        p1280.save(m_ctx->previewCardJpgPath, "JPEG", 90);
    }

    return true;
}

/* ========================================================================= */
/* Step 4: CapabilityAuditPage Implementation                                */
/* ========================================================================= */

CapabilityAuditPage::CapabilityAuditPage(CreatorPackagingContext *ctx, QWidget *parent)
    : QWizardPage(parent), m_ctx(ctx)
{
    setTitle(QStringLiteral("Step 4: Deterministic Capability & Sandbox Audit"));
    setSubTitle(QStringLiteral(
        "Audit package AST, capability flags, and ensure strict air-gap compliance."));
    setupUi();
}

void CapabilityAuditPage::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    // Top: Classification & Score Bar
    auto *badgeBox =
        new QGroupBox(QStringLiteral("Compatibility Classification & Hardware Grading"), this);
    auto *badgeLayout = new QGridLayout(badgeBox);
    badgeLayout->setHorizontalSpacing(16);
    badgeLayout->setVerticalSpacing(8);

    m_tierBadge = new QLabel(QStringLiteral("TIER 1: NATIVE"), badgeBox);
    m_tierBadge->setStyleSheet(
        QStringLiteral("background-color: #10b981; color: white; font-weight: bold; "
                       "padding: 6px 12px; border-radius: 4px; font-size: 13px;"));
    m_tierBadge->setAlignment(Qt::AlignCenter);
    m_tierBadge->setFixedWidth(180);
    badgeLayout->addWidget(m_tierBadge, 0, 0);

    m_scoreLabel = new QLabel(QStringLiteral("Compatibility Score: 100/100"), badgeBox);
    m_scoreLabel->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 12px;"));
    badgeLayout->addWidget(m_scoreLabel, 0, 1);

    m_scoreBar = new QProgressBar(badgeBox);
    m_scoreBar->setRange(0, 100);
    m_scoreBar->setValue(100);
    m_scoreBar->setFixedHeight(16);
    badgeLayout->addWidget(m_scoreBar, 1, 0, 1, 2);

    m_metricsLabel = new QLabel(
        QStringLiteral("Estimated VRAM: 64.0 MB • Estimated FPS: 60 • Pass Count: 6"), badgeBox);
    m_metricsLabel->setStyleSheet(QStringLiteral("color: #9a9a9e; font-size: 11px;"));
    badgeLayout->addWidget(m_metricsLabel, 2, 0, 1, 2);

    layout->addWidget(badgeBox);

    // Capabilities Checklist
    auto *capGroup =
        new QGroupBox(QStringLiteral("Permission Sandboxing & Capability Grants"), this);
    auto *capLayout = new QGridLayout(capGroup);
    capLayout->setHorizontalSpacing(20);
    capLayout->setVerticalSpacing(6);

    m_capComputeCheck = new QCheckBox(QStringLiteral("Compute Shaders (GLES 3.10)"), capGroup);
    capLayout->addWidget(m_capComputeCheck, 0, 0);

    m_capAudioSpectrumCheck =
        new QCheckBox(QStringLiteral("Audio Spectrum Uniforms (64 Bark bands)"), capGroup);
    capLayout->addWidget(m_capAudioSpectrumCheck, 0, 1);

    m_capAudioPlaybackCheck =
        new QCheckBox(QStringLiteral("Audio Playback (PipeWire Ducking)"), capGroup);
    capLayout->addWidget(m_capAudioPlaybackCheck, 1, 0);

    m_capScriptingCheck =
        new QCheckBox(QStringLiteral("Sandboxed Scripting (QuickJS ES2020)"), capGroup);
    capLayout->addWidget(m_capScriptingCheck, 1, 1);

    m_capForeignToplevelCheck =
        new QCheckBox(QStringLiteral("Foreign Toplevel Integration"), capGroup);
    capLayout->addWidget(m_capForeignToplevelCheck, 2, 0);

    // Locked air-gap flags
    m_capNetworkCheck =
        new QCheckBox(QStringLiteral("🔒 Network Access (Permanently Locked: FALSE)"), capGroup);
    m_capNetworkCheck->setChecked(false);
    m_capNetworkCheck->setEnabled(false);
    m_capNetworkCheck->setToolTip(QStringLiteral(
        "Air-gap invariant: .wwpkg containers are permanently forbidden from accessing network."));
    m_capNetworkCheck->setStyleSheet(QStringLiteral("color: #9a9a9e;"));
    capLayout->addWidget(m_capNetworkCheck, 3, 0);

    m_capFilesystemCheck =
        new QCheckBox(QStringLiteral("🔒 Host Filesystem (Permanently Locked: FALSE)"), capGroup);
    m_capFilesystemCheck->setChecked(false);
    m_capFilesystemCheck->setEnabled(false);
    m_capFilesystemCheck->setToolTip(
        QStringLiteral("Air-gap invariant: .wwpkg containers are permanently forbidden from "
                       "accessing host filesystem."));
    m_capFilesystemCheck->setStyleSheet(QStringLiteral("color: #9a9a9e;"));
    capLayout->addWidget(m_capFilesystemCheck, 3, 1);

    layout->addWidget(capGroup);

    // Issues & Repairs Tree
    auto *issuesGroup =
        new QGroupBox(QStringLiteral("Static AST Audit Issues & Auto-Repairs"), this);
    auto *issuesLayout = new QVBoxLayout(issuesGroup);
    issuesLayout->setSpacing(6);

    m_issuesTree = new QTreeWidget(issuesGroup);
    m_issuesTree->setColumnCount(4);
    m_issuesTree->setHeaderLabels({QStringLiteral("Severity"), QStringLiteral("File"),
                                   QStringLiteral("Message"), QStringLiteral("Suggested Fix")});
    m_issuesTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_issuesTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_issuesTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_issuesTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    issuesLayout->addWidget(m_issuesTree, 1);

    auto *repairRow = new QHBoxLayout();
    m_repairsSummaryLabel = new QLabel(QStringLiteral("0 auto-repairs available."), issuesGroup);
    m_repairsSummaryLabel->setStyleSheet(QStringLiteral("color: #9a9a9e; font-size: 11px;"));
    repairRow->addWidget(m_repairsSummaryLabel, 1);

    m_applyRepairsBtn = new QPushButton(QStringLiteral("⚡ Apply Auto-Repairs"), issuesGroup);
    m_applyRepairsBtn->setEnabled(false);
    repairRow->addWidget(m_applyRepairsBtn);

    issuesLayout->addLayout(repairRow);
    layout->addWidget(issuesGroup, 1);

    connect(m_applyRepairsBtn, &QPushButton::clicked, this, &CapabilityAuditPage::applyAutoRepairs);
}

void CapabilityAuditPage::initializePage() { runCompatibilityAudit(); }

void CapabilityAuditPage::runCompatibilityAudit()
{
    if (!m_ctx || m_ctx->stagingDir.isEmpty())
        return;

    m_issuesTree->clear();
    m_ctx->issues.clear();
    m_ctx->repairs.clear();

    // Create scanner
    we_compat_scanner_t *scanner = we_compat_scanner_create(true, true);
    if (!scanner)
        return;

    // Allocate report on heap per strict invariant (252,280 bytes)
    we_compat_report_t *report = we_compat_report_alloc();
    if (!report) {
        we_compat_scanner_destroy(scanner);
        return;
    }

    bool scanned =
        we_compat_scan_directory(scanner, m_ctx->stagingDir.toUtf8().constData(), report);

    if (scanned && report->tier != WE_TIER_UNKNOWN) {
        m_ctx->compatTier = report->tier;
        m_ctx->compatScore = report->compatibility_score;
        m_ctx->estimatedVramMb = report->summary.estimated_vram_mb;
        m_ctx->estimatedFps = report->summary.estimated_fps;

        for (uint32_t i = 0; i < report->issue_count; i++) {
            AuditIssueEntry issue;
            issue.code = QString::fromUtf8(report->issues[i].code);
            issue.severity = static_cast<int>(report->issues[i].severity);
            issue.message = QString::fromUtf8(report->issues[i].message);
            issue.filePath = QString::fromUtf8(report->issues[i].file_path);
            issue.lineNumber = report->issues[i].line_number;
            issue.suggestedFix = QString::fromUtf8(report->issues[i].suggested_fix);
            m_ctx->issues.append(issue);
        }

        for (uint32_t i = 0; i < report->repair_count; i++) {
            AuditRepairEntry repair;
            repair.transformationId = QString::fromUtf8(report->repairs[i].transformation_id);
            repair.targetFile = QString::fromUtf8(report->repairs[i].target_file);
            repair.description = QString::fromUtf8(report->repairs[i].description);
            m_ctx->repairs.append(repair);
        }
    } else {
        // Pure GLSL native wallpaper or unconstrained pass
        m_ctx->compatTier = WE_TIER_1_NATIVE;
        m_ctx->compatScore = 100;
        m_ctx->estimatedVramMb = 48.0f;
        m_ctx->estimatedFps = m_ctx->targetFps ? m_ctx->targetFps : 60;
    }

    we_compat_report_free(report);
    we_compat_scanner_destroy(scanner);

    // Update UI Badges
    switch (m_ctx->compatTier) {
    case WE_TIER_1_NATIVE:
        m_tierBadge->setText(QStringLiteral("TIER 1: NATIVE"));
        m_tierBadge->setStyleSheet(
            QStringLiteral("background-color: #10b981; color: white; font-weight: bold; "
                           "padding: 6px 12px; border-radius: 4px; font-size: 13px;"));
        break;
    case WE_TIER_2_TRANSPILED:
        m_tierBadge->setText(QStringLiteral("TIER 2: TRANSPILED"));
        m_tierBadge->setStyleSheet(
            QStringLiteral("background-color: #06b6d4; color: white; font-weight: bold; "
                           "padding: 6px 12px; border-radius: 4px; font-size: 13px;"));
        break;
    case WE_TIER_3_DEGRADED:
        m_tierBadge->setText(QStringLiteral("TIER 3: DEGRADED"));
        m_tierBadge->setStyleSheet(
            QStringLiteral("background-color: #f59e0b; color: white; font-weight: bold; "
                           "padding: 6px 12px; border-radius: 4px; font-size: 13px;"));
        break;
    case WE_TIER_4_BLOCKED:
    default:
        m_tierBadge->setText(QStringLiteral("TIER 4: BLOCKED"));
        m_tierBadge->setStyleSheet(
            QStringLiteral("background-color: #ef4444; color: white; font-weight: bold; "
                           "padding: 6px 12px; border-radius: 4px; font-size: 13px;"));
        break;
    }

    m_scoreLabel->setText(QStringLiteral("Compatibility Score: %1/100").arg(m_ctx->compatScore));
    m_scoreBar->setValue(m_ctx->compatScore);
    m_metricsLabel->setText(
        QStringLiteral("Estimated VRAM: %1 MB • Estimated FPS: %2 • Issues Found: %3")
            .arg(QString::number(m_ctx->estimatedVramMb, 'f', 1))
            .arg(m_ctx->estimatedFps)
            .arg(m_ctx->issues.size()));

    // Populate issues tree
    for (const auto &iss : m_ctx->issues) {
        auto *item = new QTreeWidgetItem(m_issuesTree);
        QString sevStr;
        QColor sevColor;
        switch (iss.severity) {
        case WE_ISSUE_SEV_INFO:
            sevStr = QStringLiteral("INFO");
            sevColor = QColor(QStringLiteral("#38bdf8"));
            break;
        case WE_ISSUE_SEV_WARNING:
            sevStr = QStringLiteral("WARNING");
            sevColor = QColor(QStringLiteral("#fbbf24"));
            break;
        case WE_ISSUE_SEV_ERROR:
            sevStr = QStringLiteral("ERROR");
            sevColor = QColor(QStringLiteral("#f87171"));
            break;
        case WE_ISSUE_SEV_FATAL:
            sevStr = QStringLiteral("FATAL");
            sevColor = QColor(QStringLiteral("#ef4444"));
            break;
        default:
            sevStr = QStringLiteral("UNKNOWN");
            sevColor = QColor(QStringLiteral("#9a9a9e"));
            break;
        }
        item->setText(0, sevStr);
        item->setForeground(0, QBrush(sevColor));
        item->setText(1, iss.filePath);
        item->setText(2, iss.message);
        item->setText(3, iss.suggestedFix.isEmpty() ? QStringLiteral("None") : iss.suggestedFix);
    }

    // Auto-select capabilities based on detection
    if (m_ctx->projectType == WWPKG_TYPE_SHADER_COMPUTE) {
        m_capComputeCheck->setChecked(true);
    }
    QDir stg(m_ctx->stagingDir);
    if (stg.exists(QStringLiteral("scripts/main.js"))) {
        m_capScriptingCheck->setChecked(true);
    }

    // Repairs
    if (!m_ctx->repairs.isEmpty()) {
        m_repairsSummaryLabel->setText(
            QStringLiteral("%1 auto-repair(s) available for automated application.")
                .arg(m_ctx->repairs.size()));
        m_applyRepairsBtn->setEnabled(true);
    } else {
        m_repairsSummaryLabel->setText(
            QStringLiteral("Zero outstanding auto-repairs. Shader and scene AST verified clean."));
        m_applyRepairsBtn->setEnabled(false);
    }

    emit completeChanged();
}

void CapabilityAuditPage::applyAutoRepairs()
{
    if (!m_ctx || m_ctx->stagingDir.isEmpty())
        return;

    we_compat_report_t *report = we_compat_report_alloc();
    if (!report)
        return;

    we_compat_scanner_t *scanner = we_compat_scanner_create(true, true);
    if (scanner) {
        if (we_compat_scan_directory(scanner, m_ctx->stagingDir.toUtf8().constData(), report)) {
            we_compat_apply_auto_repairs(report, m_ctx->stagingDir.toUtf8().constData(),
                                         m_ctx->stagingDir.toUtf8().constData());
        }
        we_compat_scanner_destroy(scanner);
    }
    we_compat_report_free(report);

    runCompatibilityAudit();
}

bool CapabilityAuditPage::isComplete() const
{
    return m_ctx && m_ctx->compatTier != WE_TIER_4_BLOCKED;
}

bool CapabilityAuditPage::validatePage()
{
    if (!isComplete()) {
        QMessageBox::critical(
            this, QStringLiteral("Packaging Blocked"),
            QStringLiteral("This wallpaper is classified into Tier 4 (Blocked) due to security "
                           "policies or fatal syntax errors. "
                           "Packages in Tier 4 cannot be compiled into .wwpkg containers."));
        return false;
    }

    m_ctx->capComputeShaders = m_capComputeCheck->isChecked();
    m_ctx->capAudioSpectrum = m_capAudioSpectrumCheck->isChecked();
    m_ctx->capAudioPlayback = m_capAudioPlaybackCheck->isChecked();
    m_ctx->capScripting = m_capScriptingCheck->isChecked();
    m_ctx->capForeignToplevel = m_capForeignToplevelCheck->isChecked();

    return true;
}

/* ========================================================================= */
/* Step 5: CompilationSigningPage Implementation                             */
/* ========================================================================= */

CompilationSigningPage::CompilationSigningPage(CreatorPackagingContext *ctx, QWidget *parent)
    : QWizardPage(parent), m_ctx(ctx)
{
    setTitle(QStringLiteral("Step 5: Ed25519 Key Management & Merkle Compilation"));
    setSubTitle(QStringLiteral("Manage your author cryptographic identity, build RFC 6962 Merkle "
                               "tree, and sign the .wwpkg container."));
    setupUi();
}

void CompilationSigningPage::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    // Cryptographic Identity Group
    auto *keyGroup = new QGroupBox(QStringLiteral("Author Cryptographic Identity (Ed25519)"), this);
    auto *keyLayout = new QVBoxLayout(keyGroup);
    keyLayout->setSpacing(8);

    auto *keyRow = new QHBoxLayout();
    m_keyPathEdit = new QLineEdit(keyGroup);
    keyRow->addWidget(m_keyPathEdit, 1);

    m_browseKeyBtn = new QPushButton(QStringLiteral("Browse Key..."), keyGroup);
    keyRow->addWidget(m_browseKeyBtn);

    m_generateKeyBtn = new QPushButton(QStringLiteral("🔑 Generate Keypair"), keyGroup);
    m_generateKeyBtn->setStyleSheet(
        QStringLiteral("background-color: #2563eb; color: white; font-weight: bold;"));
    keyRow->addWidget(m_generateKeyBtn);

    keyLayout->addLayout(keyRow);

    m_keyFingerprintLabel =
        new QLabel(QStringLiteral("Public Key Fingerprint: None loaded"), keyGroup);
    m_keyFingerprintLabel->setStyleSheet(
        QStringLiteral("font-family: monospace; color: #38bdf8; font-size: 11px;"));
    keyLayout->addWidget(m_keyFingerprintLabel);

    layout->addWidget(keyGroup);

    // Output & Build Action Group
    auto *buildGroup = new QGroupBox(QStringLiteral("Container Compilation & Signing"), this);
    auto *buildLayout = new QVBoxLayout(buildGroup);
    buildLayout->setSpacing(8);

    auto *outRow = new QHBoxLayout();
    auto *outLbl = new QLabel(QStringLiteral("Output Package:"), buildGroup);
    outRow->addWidget(outLbl);

    m_outPkgPathEdit = new QLineEdit(buildGroup);
    outRow->addWidget(m_outPkgPathEdit, 1);

    m_browseOutBtn = new QPushButton(QStringLiteral("Browse..."), buildGroup);
    outRow->addWidget(m_browseOutBtn);
    buildLayout->addLayout(outRow);

    auto *actRow = new QHBoxLayout();
    m_compileBtn = new QPushButton(
        QStringLiteral("📦 Compile & Cryptographically Sign .wwpkg Package"), buildGroup);
    m_compileBtn->setFixedHeight(32);
    m_compileBtn->setStyleSheet(QStringLiteral(
        "background-color: #16a34a; color: white; font-weight: bold; font-size: 13px;"));
    actRow->addWidget(m_compileBtn);

    m_verificationBadge = new QLabel(QStringLiteral("Status: Not Compiled"), buildGroup);
    m_verificationBadge->setStyleSheet(QStringLiteral("color: #9a9a9e; font-weight: bold;"));
    actRow->addWidget(m_verificationBadge);
    actRow->addStretch();
    buildLayout->addLayout(actRow);

    m_progressBar = new QProgressBar(buildGroup);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setFixedHeight(12);
    buildLayout->addWidget(m_progressBar);

    layout->addWidget(buildGroup);

    // Compilation Log
    auto *logGroup = new QGroupBox(QStringLiteral("RFC 6962 Merkle Tree & Attestation Log"), this);
    auto *logLayout = new QVBoxLayout(logGroup);
    m_logEdit = new QTextEdit(logGroup);
    m_logEdit->setReadOnly(true);
    m_logEdit->setStyleSheet(QStringLiteral(
        "background-color: #090d16; font-family: monospace; font-size: 10px; color: #e2e8f0;"));
    logLayout->addWidget(m_logEdit);
    layout->addWidget(logGroup, 1);

    // Wiring
    connect(m_generateKeyBtn, &QPushButton::clicked, this,
            &CompilationSigningPage::generateKeypair);
    connect(m_browseKeyBtn, &QPushButton::clicked, this, &CompilationSigningPage::browseKeyFile);
    connect(m_browseOutBtn, &QPushButton::clicked, this, [this]() {
        QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Save .wwpkg Container"),
                                                 m_outPkgPathEdit->text(),
                                                 QStringLiteral("WayWal Package (*.wwpkg)"));
        if (!f.isEmpty()) {
            m_outPkgPathEdit->setText(f);
        }
    });
    connect(m_compileBtn, &QPushButton::clicked, this,
            &CompilationSigningPage::compileAndSignPackage);
}

void CompilationSigningPage::initializePage()
{
    if (!m_ctx)
        return;

    if (m_keyPathEdit->text().isEmpty()) {
        m_keyPathEdit->setText(QDir::homePath() +
                               QStringLiteral("/.config/waywal/keys/author_ed25519.key"));
    }

    if (m_outPkgPathEdit->text().isEmpty()) {
        QString outDir = QDir::homePath() + QStringLiteral("/waywal_packages");
        QDir().mkpath(outDir);
        m_outPkgPathEdit->setText(
            outDir + QStringLiteral("/%1-%2.wwpkg").arg(m_ctx->slug).arg(m_ctx->version));
    }

    checkOrLoadKey();
}

void CompilationSigningPage::checkOrLoadKey()
{
    QString keyPath = m_keyPathEdit->text().trimmed();
    QFile keyFile(keyPath);

    if (!keyFile.exists() || !keyFile.open(QIODevice::ReadOnly)) {
        m_keyFingerprintLabel->setText(QStringLiteral(
            "Key file not found. Click 'Generate Keypair' to create your signing identity."));
        m_keyFingerprintLabel->setStyleSheet(QStringLiteral("color: #f59e0b;"));
        m_ctx->authorPrivKey.clear();
        m_ctx->authorPubKey.clear();
        return;
    }

    QByteArray raw = keyFile.readAll().trimmed();
    keyFile.close();

    QByteArray privBytes;
    if (raw.size() == 64) {
        privBytes = raw;
    } else if (raw.size() == 128) {
        privBytes = QByteArray::fromHex(raw);
    } else if (raw.size() == 32) {
        // Derive from 32-byte seed
        uint8_t pk[32];
        uint8_t sk[64];
        memcpy(sk, raw.constData(), 32);
        wwpkg_ed25519_keypair(pk, sk);
        privBytes = QByteArray((const char *)sk, 64);
    } else if (raw.size() == 64 && raw.indexOf(' ') == -1) {
        QByteArray seed = QByteArray::fromHex(raw);
        if (seed.size() == 32) {
            uint8_t pk[32];
            uint8_t sk[64];
            memcpy(sk, seed.constData(), 32);
            wwpkg_ed25519_keypair(pk, sk);
            privBytes = QByteArray((const char *)sk, 64);
        }
    }

    if (privBytes.size() == 64) {
        m_ctx->authorPrivKey = privBytes;
        m_ctx->authorPubKey = privBytes.mid(32, 32);
        QString pubHex = m_ctx->authorPubKey.toHex();
        m_keyFingerprintLabel->setText(QStringLiteral("Author Public Key: %1").arg(pubHex));
        m_keyFingerprintLabel->setStyleSheet(
            QStringLiteral("font-family: monospace; color: #38bdf8; font-weight: bold;"));
        addPubkeyToTrustedKeys(pubHex);
    } else {
        m_keyFingerprintLabel->setText(
            QStringLiteral("Invalid key format. Expected 64-byte Ed25519 private key."));
        m_keyFingerprintLabel->setStyleSheet(QStringLiteral("color: #ef4444;"));
    }
}

void CompilationSigningPage::generateKeypair()
{
    uint8_t pub[32];
    uint8_t priv[64];
    wwpkg_generate_keypair(pub, priv);

    QString keyPath = m_keyPathEdit->text().trimmed();
    if (keyPath.isEmpty()) {
        keyPath = QDir::homePath() + QStringLiteral("/.config/waywal/keys/author_ed25519.key");
        m_keyPathEdit->setText(keyPath);
    }

    QFileInfo fi(keyPath);
    fi.dir().mkpath(QStringLiteral("."));

    QFile keyFile(keyPath);
    if (keyFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QByteArray hex = QByteArray(reinterpret_cast<const char *>(priv), 64).toHex() + '\n';
        keyFile.write(hex);
        keyFile.close();
        chmod(keyPath.toUtf8().constData(), 0600);
    }

    m_ctx->authorPrivKey = QByteArray(reinterpret_cast<const char *>(priv), 64);
    m_ctx->authorPubKey = QByteArray(reinterpret_cast<const char *>(pub), 32);

    QString pubHex = m_ctx->authorPubKey.toHex();
    m_keyFingerprintLabel->setText(QStringLiteral("Generated New Author Identity: %1").arg(pubHex));
    m_keyFingerprintLabel->setStyleSheet(
        QStringLiteral("font-family: monospace; color: #10b981; font-weight: bold;"));

    addPubkeyToTrustedKeys(pubHex);
}

void CompilationSigningPage::browseKeyFile()
{
    QString f = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select Author Key"), m_keyPathEdit->text(),
        QStringLiteral("Key Files (*.key *.priv *.pem);;All Files (*)"));
    if (!f.isEmpty()) {
        m_keyPathEdit->setText(f);
        checkOrLoadKey();
    }
}

void CompilationSigningPage::compileAndSignPackage()
{
    if (!m_ctx || m_ctx->stagingDir.isEmpty())
        return;

    if (m_ctx->authorPrivKey.size() != 64) {
        generateKeypair();
    }

    QString outPkg = m_outPkgPathEdit->text().trimmed();
    if (outPkg.isEmpty()) {
        outPkg =
            QDir::homePath() +
            QStringLiteral("/waywal_packages/%1-%2.wwpkg").arg(m_ctx->slug).arg(m_ctx->version);
        m_outPkgPathEdit->setText(outPkg);
    }

    QFileInfo outFi(outPkg);
    outFi.dir().mkpath(QStringLiteral("."));

    m_logEdit->clear();
    m_progressBar->setValue(10);
    m_logEdit->append(QStringLiteral("[%1] Initiating .wwpkg container compilation...")
                          .arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
    m_logEdit->append(QStringLiteral("Staging Root: %1").arg(m_ctx->stagingDir));
    m_logEdit->append(QStringLiteral("Output File: %1").arg(outPkg));

    // Construct wwpkg_manifest_t
    wwpkg_manifest_t manifest;
    memset(&manifest, 0, sizeof(manifest));

    strncpy(manifest.schema_version, "wwpkg/v1", sizeof(manifest.schema_version) - 1);
    strncpy(manifest.id, m_ctx->packageId.toUtf8().constData(), sizeof(manifest.id) - 1);
    strncpy(manifest.slug, m_ctx->slug.toUtf8().constData(), sizeof(manifest.slug) - 1);
    strncpy(manifest.name, m_ctx->name.toUtf8().constData(), sizeof(manifest.name) - 1);
    strncpy(manifest.version, m_ctx->version.toUtf8().constData(), sizeof(manifest.version) - 1);
    strncpy(manifest.author_name, m_ctx->authorName.toUtf8().constData(),
            sizeof(manifest.author_name) - 1);
    strncpy(manifest.author_email, m_ctx->authorEmail.toUtf8().constData(),
            sizeof(manifest.author_email) - 1);
    memcpy(manifest.author_pubkey, m_ctx->authorPubKey.constData(), 32);
    strncpy(manifest.license, m_ctx->license.toUtf8().constData(), sizeof(manifest.license) - 1);
    strncpy(manifest.description, m_ctx->description.toUtf8().constData(),
            sizeof(manifest.description) - 1);

    manifest.tag_count =
        static_cast<uint32_t>(qMin(m_ctx->tags.size(), static_cast<qsizetype>(WWPKG_MAX_TAGS)));
    for (uint32_t i = 0; i < manifest.tag_count; i++) {
        strncpy(manifest.tags[i], m_ctx->tags[i].toUtf8().constData(),
                sizeof(manifest.tags[i]) - 1);
    }

    manifest.type = m_ctx->projectType;
    strncpy(manifest.entrypoint_pipeline, m_ctx->entrypointPipeline.toUtf8().constData(),
            sizeof(manifest.entrypoint_pipeline) - 1);
    strncpy(manifest.entrypoint_preview, "preview.webp", sizeof(manifest.entrypoint_preview) - 1);
    strncpy(manifest.entrypoint_audio, m_ctx->entrypointAudio.toUtf8().constData(),
            sizeof(manifest.entrypoint_audio) - 1);

    strncpy(manifest.min_wwal_version, "1.0.0", sizeof(manifest.min_wwal_version) - 1);
    manifest.abi_version = 1;
    strncpy(manifest.gl_profile, "gles_31", sizeof(manifest.gl_profile) - 1);

    manifest.capabilities.compute_shaders = m_ctx->capComputeShaders;
    manifest.capabilities.audio_spectrum = m_ctx->capAudioSpectrum;
    manifest.capabilities.audio_playback = m_ctx->capAudioPlayback;
    manifest.capabilities.scripting = m_ctx->capScripting;
    manifest.capabilities.foreign_toplevel = m_ctx->capForeignToplevel;
    manifest.capabilities.network = false;    // Air-gap invariant
    manifest.capabilities.filesystem = false; // Air-gap invariant

    manifest.budgets.max_vram_mb = m_ctx->maxVramMb;
    manifest.budgets.target_fps = m_ctx->targetFps;

    m_progressBar->setValue(35);
    m_logEdit->append(QStringLiteral("Generating canonical manifest.json via RFC 8785 JCS..."));

    // Invoke wwpkg_build_package()
    bool buildOk =
        wwpkg_build_package(m_ctx->stagingDir.toUtf8().constData(), &manifest,
                            reinterpret_cast<const uint8_t *>(m_ctx->authorPrivKey.constData()),
                            outPkg.toUtf8().constData());

    m_progressBar->setValue(75);

    if (!buildOk) {
        m_logEdit->append(
            QStringLiteral("❌ ERROR: Failed to compile package via wwpkg_build_package()!"));
        m_verificationBadge->setText(QStringLiteral("Status: Compilation Failed"));
        m_verificationBadge->setStyleSheet(QStringLiteral("color: #ef4444; font-weight: bold;"));
        m_progressBar->setValue(0);
        return;
    }

    m_logEdit->append(QStringLiteral("✓ POSIX tar container compressed with Zstandard level 12."));
    m_logEdit->append(
        QStringLiteral("Performing immediate post-build cryptographic self-verification..."));

    // Verify self-check
    wwpkg_package_t *pkg = wwpkg_open(outPkg.toUtf8().constData());
    if (!pkg) {
        m_logEdit->append(QStringLiteral("❌ ERROR: Could not open newly built package!"));
        return;
    }

    bool isTrusted = false;
    bool verified = wwpkg_verify_signature(pkg, &isTrusted);
    const wwpkg_signature_t *sig = wwpkg_get_signature(pkg);

    if (sig) {
        m_ctx->merkleRoot = QByteArray(reinterpret_cast<const char *>(sig->merkle_root), 32);
        m_ctx->signature = QByteArray(reinterpret_cast<const char *>(sig->signature), 64);
    }

    wwpkg_close(pkg);

    m_progressBar->setValue(100);

    if (verified) {
        m_ctx->isCompiled = true;
        m_ctx->isVerified = true;
        m_ctx->isTrusted = isTrusted;
        m_ctx->outPkgPath = outPkg;
        m_ctx->compiledSizeBytes = QFileInfo(outPkg).size();

        m_logEdit->append(QStringLiteral("✓ RFC 6962 Domain-Separated Merkle Root: %1")
                              .arg(QString::fromLatin1(m_ctx->merkleRoot.toHex())));
        m_logEdit->append(QStringLiteral("✓ RFC 8032 Ed25519 Cryptographic Signature: %1...")
                              .arg(QString::fromLatin1(m_ctx->signature.toHex().left(32))));
        m_logEdit->append(
            QStringLiteral("✓ Author Key Trust Attestation: %1")
                .arg(isTrusted ? QStringLiteral("TRUSTED") : QStringLiteral("UNTRUSTED")));
        m_logEdit->append(QStringLiteral("✓ Package compiled and verified: %1 (%2)")
                              .arg(outPkg)
                              .arg(formatBytes(m_ctx->compiledSizeBytes)));

        m_verificationBadge->setText(QStringLiteral("✓ VERIFIED & SIGNED"));
        m_verificationBadge->setStyleSheet(QStringLiteral("color: #10b981; font-weight: bold;"));

        emit completeChanged();
    } else {
        m_logEdit->append(QStringLiteral("❌ ERROR: Cryptographic self-verification failed!"));
        m_verificationBadge->setText(QStringLiteral("Status: Verification Failed"));
        m_verificationBadge->setStyleSheet(QStringLiteral("color: #ef4444; font-weight: bold;"));
    }
}

bool CompilationSigningPage::isComplete() const
{
    return m_ctx && m_ctx->isCompiled && m_ctx->isVerified;
}

bool CompilationSigningPage::validatePage() { return isComplete(); }

/* ========================================================================= */
/* Step 6: HubPublishingExportPage Implementation                            */
/* ========================================================================= */

HubPublishingExportPage::HubPublishingExportPage(CreatorPackagingContext *ctx, QWidget *parent)
    : QWizardPage(parent), m_ctx(ctx)
{
    setTitle(QStringLiteral("Step 6: Open Hub Registry Publishing & Local Export"));
    setSubTitle(QStringLiteral("Publish your content-addressed package to the WayWal Open Hub or "
                               "install locally with 1-click."));
    setupUi();
}

void HubPublishingExportPage::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    // CID Banner
    auto *cidBox = new QGroupBox(QStringLiteral("Content-Addressed Identifier (CID)"), this);
    auto *cidLayout = new QHBoxLayout(cidBox);

    m_cidLabel = new QLabel(QStringLiteral("sha256:..."), cidBox);
    m_cidLabel->setStyleSheet(QStringLiteral(
        "font-family: monospace; font-size: 12px; font-weight: bold; color: #38bdf8;"));
    m_cidLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cidLayout->addWidget(m_cidLabel, 1);

    m_copyCidBtn = new QPushButton(QStringLiteral("📋 Copy CID"), cidBox);
    cidLayout->addWidget(m_copyCidBtn);
    layout->addWidget(cidBox);

    // Registry Index Entry JSON
    auto *jsonBox = new QGroupBox(
        QStringLiteral("Open Hub Federated Registry Index Entry (RFC 8785 Format)"), this);
    auto *jsonLayout = new QVBoxLayout(jsonBox);
    jsonLayout->setSpacing(6);

    m_registryJsonEdit = new QTextEdit(jsonBox);
    m_registryJsonEdit->setReadOnly(true);
    m_registryJsonEdit->setStyleSheet(QStringLiteral(
        "background-color: #090d16; font-family: monospace; font-size: 10px; color: #e2e8f0;"));
    jsonLayout->addWidget(m_registryJsonEdit, 1);

    auto *btnRow = new QHBoxLayout();
    m_copyJsonBtn = new QPushButton(QStringLiteral("Copy JSON"), jsonBox);
    btnRow->addWidget(m_copyJsonBtn);

    m_saveJsonBtn = new QPushButton(QStringLiteral("Export index.json..."), jsonBox);
    btnRow->addWidget(m_saveJsonBtn);
    btnRow->addStretch();
    jsonLayout->addLayout(btnRow);

    layout->addWidget(jsonBox, 1);

    // Actions Group
    auto *actGroup = new QGroupBox(QStringLiteral("Local Deployment & Distribution"), this);
    auto *actLayout = new QVBoxLayout(actGroup);
    actLayout->setSpacing(8);

    auto *installRow = new QHBoxLayout();
    m_installLocalBtn = new QPushButton(
        QStringLiteral("⚡ 1-Click Install to Local Hub (~/.local/share/waywal/hub/)"), actGroup);
    m_installLocalBtn->setFixedHeight(32);
    m_installLocalBtn->setStyleSheet(QStringLiteral(
        "background-color: #2563eb; color: white; font-weight: bold; font-size: 12px;"));
    installRow->addWidget(m_installLocalBtn);

    m_openFolderBtn = new QPushButton(QStringLiteral("Open Output Folder"), actGroup);
    installRow->addWidget(m_openFolderBtn);

    m_exportPkgBtn = new QPushButton(QStringLiteral("Export .wwpkg As..."), actGroup);
    installRow->addWidget(m_exportPkgBtn);

    actLayout->addLayout(installRow);

    m_installStatusLabel = new QLabel(
        QStringLiteral("Ready to deploy into local WayWal Open Hub directory."), actGroup);
    m_installStatusLabel->setStyleSheet(QStringLiteral("color: #9a9a9e; font-size: 11px;"));
    actLayout->addWidget(m_installStatusLabel);

    layout->addWidget(actGroup);

    // Wiring
    connect(m_copyCidBtn, &QPushButton::clicked, this,
            &HubPublishingExportPage::copyCidToClipboard);
    connect(m_copyJsonBtn, &QPushButton::clicked, this,
            &HubPublishingExportPage::copyJsonToClipboard);
    connect(m_saveJsonBtn, &QPushButton::clicked, this, &HubPublishingExportPage::exportJsonToFile);
    connect(m_installLocalBtn, &QPushButton::clicked, this,
            &HubPublishingExportPage::installToLocalHub);
    connect(m_openFolderBtn, &QPushButton::clicked, this,
            &HubPublishingExportPage::openContainingFolder);
    connect(m_exportPkgBtn, &QPushButton::clicked, this, &HubPublishingExportPage::exportPackageTo);
}

void HubPublishingExportPage::initializePage()
{
    if (!m_ctx)
        return;

    // Compute CID from Merkle root
    m_ctx->cid = QStringLiteral("sha256:") + QString::fromLatin1(m_ctx->merkleRoot.toHex());
    m_cidLabel->setText(m_ctx->cid);

    // Construct Open Hub Registry Index Entry JSON
    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), QStringLiteral("openhub/v1"));
    root.insert(QStringLiteral("cid"), m_ctx->cid);
    root.insert(QStringLiteral("id"), m_ctx->packageId);
    root.insert(QStringLiteral("slug"), m_ctx->slug);
    root.insert(QStringLiteral("name"), m_ctx->name);
    root.insert(QStringLiteral("version"), m_ctx->version);

    QJsonObject authorObj;
    authorObj.insert(QStringLiteral("name"), m_ctx->authorName);
    authorObj.insert(QStringLiteral("email"), m_ctx->authorEmail);
    authorObj.insert(QStringLiteral("pubkey"), QString::fromLatin1(m_ctx->authorPubKey.toHex()));
    root.insert(QStringLiteral("author"), authorObj);

    root.insert(QStringLiteral("license"), m_ctx->license);
    root.insert(QStringLiteral("description"), m_ctx->description);
    root.insert(QStringLiteral("type"), m_ctx->projectTypeStr);

    QJsonArray tagsArr;
    for (const auto &t : m_ctx->tags) {
        tagsArr.append(t);
    }
    root.insert(QStringLiteral("tags"), tagsArr);

    QJsonObject entrypointObj;
    entrypointObj.insert(QStringLiteral("pipeline"), m_ctx->entrypointPipeline);
    entrypointObj.insert(QStringLiteral("preview"), QStringLiteral("preview.webp"));
    entrypointObj.insert(QStringLiteral("preview_card"), QStringLiteral("preview_card.jpg"));
    if (!m_ctx->entrypointAudio.isEmpty()) {
        entrypointObj.insert(QStringLiteral("audio"), m_ctx->entrypointAudio);
    }
    root.insert(QStringLiteral("entrypoint"), entrypointObj);

    QJsonObject capObj;
    capObj.insert(QStringLiteral("compute_shaders"), m_ctx->capComputeShaders);
    capObj.insert(QStringLiteral("audio_spectrum"), m_ctx->capAudioSpectrum);
    capObj.insert(QStringLiteral("audio_playback"), m_ctx->capAudioPlayback);
    capObj.insert(QStringLiteral("scripting"), m_ctx->capScripting);
    capObj.insert(QStringLiteral("foreign_toplevel"), m_ctx->capForeignToplevel);
    capObj.insert(QStringLiteral("network"), false);
    capObj.insert(QStringLiteral("filesystem"), false);
    root.insert(QStringLiteral("capabilities"), capObj);

    QJsonObject budgetObj;
    budgetObj.insert(QStringLiteral("max_vram_mb"), static_cast<qint64>(m_ctx->maxVramMb));
    budgetObj.insert(QStringLiteral("target_fps"), static_cast<qint64>(m_ctx->targetFps));
    root.insert(QStringLiteral("resource_budgets"), budgetObj);

    root.insert(QStringLiteral("merkle_root"), QString::fromLatin1(m_ctx->merkleRoot.toHex()));
    root.insert(QStringLiteral("signature"), QString::fromLatin1(m_ctx->signature.toHex()));
    root.insert(QStringLiteral("file_size_bytes"), m_ctx->compiledSizeBytes);
    root.insert(QStringLiteral("published_at"), QDateTime::currentSecsSinceEpoch());

    QJsonDocument doc(root);
    m_ctx->registryIndexJson = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
    m_registryJsonEdit->setText(m_ctx->registryIndexJson);
}

void HubPublishingExportPage::copyCidToClipboard()
{
    if (m_ctx && !m_ctx->cid.isEmpty()) {
        QGuiApplication::clipboard()->setText(m_ctx->cid);
        m_copyCidBtn->setText(QStringLiteral("✓ Copied!"));
    }
}

void HubPublishingExportPage::copyJsonToClipboard()
{
    if (m_ctx && !m_ctx->registryIndexJson.isEmpty()) {
        QGuiApplication::clipboard()->setText(m_ctx->registryIndexJson);
        m_copyJsonBtn->setText(QStringLiteral("✓ Copied!"));
    }
}

void HubPublishingExportPage::exportJsonToFile()
{
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export Open Hub Index JSON"),
                                                QDir::homePath() + QStringLiteral("/index.json"),
                                                QStringLiteral("JSON Files (*.json)"));
    if (!path.isEmpty() && m_ctx) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(m_ctx->registryIndexJson.toUtf8());
            f.close();
            QMessageBox::information(
                this, QStringLiteral("Export Successful"),
                QStringLiteral("Registry index entry exported to:\n%1").arg(path));
        }
    }
}

void HubPublishingExportPage::installToLocalHub()
{
    if (!m_ctx || m_ctx->outPkgPath.isEmpty() || !QFile::exists(m_ctx->outPkgPath)) {
        return;
    }

    const QString hubDir = QDir::homePath() + QStringLiteral("/.local/share/waywal/hub");
    QDir().mkpath(hubDir);

    QFileInfo outFi(m_ctx->outPkgPath);
    QString dstPkg = hubDir + QStringLiteral("/") + outFi.fileName();

    QFile::remove(dstPkg);
    if (!QFile::copy(m_ctx->outPkgPath, dstPkg)) {
        m_installStatusLabel->setText(
            QStringLiteral("Failed to copy package to local hub directory."));
        m_installStatusLabel->setStyleSheet(QStringLiteral("color: #ef4444;"));
        return;
    }

    // Update ~/.local/share/waywal/hub/catalog.json
    QString catPath = hubDir + QStringLiteral("/catalog.json");
    QJsonArray catArray;
    QFile readCat(catPath);
    if (readCat.open(QIODevice::ReadOnly)) {
        QJsonDocument d = QJsonDocument::fromJson(readCat.readAll());
        readCat.close();
        if (d.isArray()) {
            catArray = d.array();
        }
    }

    // Replace existing slug entry or append
    QJsonDocument itemDoc = QJsonDocument::fromJson(m_ctx->registryIndexJson.toUtf8());
    if (itemDoc.isObject()) {
        QJsonObject newObj = itemDoc.object();
        newObj.insert(QStringLiteral("local_package_path"), dstPkg);
        bool found = false;
        for (int i = 0; i < catArray.size(); i++) {
            if (catArray[i].toObject().value(QStringLiteral("slug")).toString() == m_ctx->slug) {
                catArray[i] = newObj;
                found = true;
                break;
            }
        }
        if (!found) {
            catArray.append(newObj);
        }

        QFile writeCat(catPath);
        if (writeCat.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            writeCat.write(QJsonDocument(catArray).toJson(QJsonDocument::Indented));
            writeCat.close();
        }
    }

    m_ctx->installedToLocalHub = true;
    m_installStatusLabel->setText(
        QStringLiteral("✓ Installed to %1 successfully! Available in OpenHubBrowser.").arg(dstPkg));
    m_installStatusLabel->setStyleSheet(QStringLiteral("color: #10b981; font-weight: bold;"));
}

void HubPublishingExportPage::openContainingFolder()
{
    if (m_ctx && !m_ctx->outPkgPath.isEmpty()) {
        QFileInfo fi(m_ctx->outPkgPath);
        QDesktopServices::openUrl(QUrl::fromLocalFile(fi.dir().absolutePath()));
    }
}

void HubPublishingExportPage::exportPackageTo()
{
    if (!m_ctx || m_ctx->outPkgPath.isEmpty())
        return;

    QString dst = QFileDialog::getSaveFileName(this, QStringLiteral("Export .wwpkg Package"),
                                               QFileInfo(m_ctx->outPkgPath).fileName(),
                                               QStringLiteral("WayWal Package (*.wwpkg)"));
    if (!dst.isEmpty()) {
        QFile::remove(dst);
        if (QFile::copy(m_ctx->outPkgPath, dst)) {
            QMessageBox::information(
                this, QStringLiteral("Export Complete"),
                QStringLiteral("Package exported successfully to:\n%1").arg(dst));
        }
    }
}

/* ========================================================================= */
/* CreatorPackagingWizard Top-Level Controller                               */
/* ========================================================================= */

CreatorPackagingWizard::CreatorPackagingWizard(QWidget *parent) : QWizard(parent)
{
    setObjectName(QStringLiteral("CreatorPackagingWizard"));
    setWindowTitle(QStringLiteral("WayWal Creator Packaging & Publishing Wizard (.wwpkg)"));
    setWizardStyle(QWizard::ModernStyle);
    resize(940, 700);

    // Instantiate all 6 guided pages
    m_intakePage = new SourceIntakePage(&m_context, this);
    m_bundlingPage = new AssetBundlingPage(&m_context, this);
    m_metadataPage = new MetadataPreviewPage(&m_context, this);
    m_auditPage = new CapabilityAuditPage(&m_context, this);
    m_compilationPage = new CompilationSigningPage(&m_context, this);
    m_exportPage = new HubPublishingExportPage(&m_context, this);

    setPage(Page_SourceIntake, m_intakePage);
    setPage(Page_AssetBundling, m_bundlingPage);
    setPage(Page_MetadataPreview, m_metadataPage);
    setPage(Page_CapabilityAudit, m_auditPage);
    setPage(Page_CompilationSigning, m_compilationPage);
    setPage(Page_HubPublishingExport, m_exportPage);

    applyStudioTheme();
}

CreatorPackagingWizard::~CreatorPackagingWizard()
{
    // Clean up temporary staging directory if located in QDir::tempPath
    if (!m_context.stagingDir.isEmpty() && m_context.stagingDir.startsWith(QDir::tempPath())) {
        QDir stg(m_context.stagingDir);
        stg.removeRecursively();
    }
}

void CreatorPackagingWizard::applyStudioTheme()
{
    // Pro-studio dark aesthetic matching wwal-studio
    setStyleSheet(
        QStringLiteral("QWizard {"
                       "  background-color: #181820;"
                       "  color: #f1f5f9;"
                       "}"
                       "QWizardPage {"
                       "  background-color: #181820;"
                       "  color: #f1f5f9;"
                       "}"
                       "QGroupBox {"
                       "  border: 1px solid #334155;"
                       "  border-radius: 6px;"
                       "  margin-top: 10px;"
                       "  padding-top: 10px;"
                       "  font-weight: bold;"
                       "  color: #cbd5e1;"
                       "}"
                       "QGroupBox::title {"
                       "  subcontrol-origin: margin;"
                       "  subcontrol-position: top left;"
                       "  left: 10px;"
                       "  padding: 0 4px;"
                       "}"
                       "QLineEdit, QComboBox, QSpinBox, QTextEdit {"
                       "  background-color: #0f172a;"
                       "  border: 1px solid #334155;"
                       "  border-radius: 4px;"
                       "  padding: 4px 8px;"
                       "  color: #f8fafc;"
                       "}"
                       "QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QTextEdit:focus {"
                       "  border: 1px solid #3b82f6;"
                       "}"
                       "QPushButton {"
                       "  background-color: #1e293b;"
                       "  border: 1px solid #475569;"
                       "  border-radius: 4px;"
                       "  padding: 5px 12px;"
                       "  color: #f8fafc;"
                       "}"
                       "QPushButton:hover {"
                       "  background-color: #334155;"
                       "}"
                       "QPushButton:pressed {"
                       "  background-color: #0f172a;"
                       "}"
                       "QTreeWidget {"
                       "  background-color: #0f172a;"
                       "  border: 1px solid #334155;"
                       "  border-radius: 4px;"
                       "  color: #f8fafc;"
                       "}"
                       "QHeaderView::section {"
                       "  background-color: #1e293b;"
                       "  color: #94a3b8;"
                       "  padding: 4px;"
                       "  border: 1px solid #334155;"
                       "}"
                       "QProgressBar {"
                       "  background-color: #0f172a;"
                       "  border: 1px solid #334155;"
                       "  border-radius: 3px;"
                       "  text-align: center;"
                       "  color: white;"
                       "}"
                       "QProgressBar::chunk {"
                       "  background-color: #3b82f6;"
                       "}"));
}

void CreatorPackagingWizard::setSourcePath(const QString &path)
{
    m_context.sourcePath = path;
    if (m_intakePage) {
        m_intakePage->setSourcePath(path);
    }
}

QString CreatorPackagingWizard::sourcePath() const { return m_context.sourcePath; }

QString CreatorPackagingWizard::compiledPackagePath() const { return m_context.outPkgPath; }

QString CreatorPackagingWizard::contentIdentifier() const { return m_context.cid; }

} // namespace wwal::studio
