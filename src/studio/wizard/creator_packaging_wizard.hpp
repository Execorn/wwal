// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_CREATOR_PACKAGING_WIZARD_HPP
#define WAYWAL_STUDIO_CREATOR_PACKAGING_WIZARD_HPP

#include <QWizard>
#include <QWizardPage>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QImage>
#include <QList>
#include <cstring>
#include <memory>

#include "waywal/wwpkg.h"
#include "waywal/we_compat_scanner.h"

// Forward declarations of Qt UI widgets
class QLineEdit;
class QTextEdit;
class QComboBox;
class QCheckBox;
class QLabel;
class QProgressBar;
class QTreeWidget;
class QPushButton;
class QSpinBox;

namespace wwal::studio {

/**
 * @brief Scanned asset entry representation for packaging.
 */
struct ScannedAssetEntry {
    QString relativePath;
    qint64 sizeBytes{0};
    QString category; // "shader", "texture", "model", "audio", "script", "metadata", "other"
    bool isReferenced{true};
    bool isOptimized{false};
};

/**
 * @brief Audit issue item from capability scanner.
 */
struct AuditIssueEntry {
    QString code;
    int severity{0}; // 0 = Info, 1 = Warning, 2 = Error, 3 = Fatal
    QString message;
    QString filePath;
    uint32_t lineNumber{0};
    QString suggestedFix;
};

/**
 * @brief Auto-repair transformation item.
 */
struct AuditRepairEntry {
    QString transformationId;
    QString targetFile;
    QString description;
};

/**
 * @brief Shared packaging context maintaining mutable state across all 6 wizard steps.
 */
struct CreatorPackagingContext {
    // Step 1: Intake
    QString sourcePath;
    QString projectTypeStr{"shader_fragment"};
    wwpkg_type_t projectType{WWPKG_TYPE_SHADER_FRAGMENT};
    QString detectedTitle;
    QString detectedAuthor;
    QString detectedDescription;
    QString detectedLicense;
    QString entrypointPipeline;
    QString entrypointPreview{"preview.webp"};
    QString entrypointAudio;
    QString stagingDir;

    // Step 2: Assets
    QList<ScannedAssetEntry> scannedAssets;
    bool pruneDeadFiles{true};
    bool transcodeTextures{true};

    // Step 3: Metadata
    QString packageId; // UUID
    QString slug;
    QString name;
    QString version{"1.0.0"};
    QString authorName;
    QString authorEmail;
    QString license{"MIT"};
    QString description;
    QStringList tags;
    uint32_t targetFps{60};
    uint32_t maxVramMb{256};
    QImage previewImage;
    QString previewWebpPath;
    QString previewCardJpgPath;

    // Step 4: Capability & Compatibility Audit
    we_compat_tier_t compatTier{WE_TIER_UNKNOWN};
    uint32_t compatScore{0};
    float estimatedVramMb{0.0f};
    uint32_t estimatedFps{60};
    bool capComputeShaders{false};
    bool capAudioSpectrum{false};
    bool capAudioPlayback{false};
    bool capScripting{false};
    bool capForeignToplevel{false};
    // Invariant: network and filesystem must always remain false (air-gap)
    static constexpr bool capNetwork{false};
    static constexpr bool capFilesystem{false};
    QList<AuditIssueEntry> issues;
    QList<AuditRepairEntry> repairs;

    // Step 5: Signing & Compilation
    QString keyPath;
    QByteArray authorPrivKey; // 64 bytes
    QByteArray authorPubKey;  // 32 bytes
    QString outPkgPath;
    QByteArray merkleRoot; // 32 bytes
    QByteArray signature;  // 64 bytes
    bool isCompiled{false};
    bool isVerified{false};
    bool isTrusted{false};
    qint64 compiledSizeBytes{0};

    // Step 6: Hub Publishing & Export
    QString cid; // "sha256:<64 hex digits>"
    QString registryIndexJson;
    bool installedToLocalHub{false};

    ~CreatorPackagingContext()
    {
        if (!authorPrivKey.isEmpty()) {
            explicit_bzero(authorPrivKey.data(), static_cast<size_t>(authorPrivKey.size()));
            authorPrivKey.clear();
        }
    }
};

class CreatorPackagingWizard;

/**
 * @brief Step 1: Source Intake Page
 * Allows selecting a source folder or GLSL shader/scene file and auto-populates metadata.
 */
class SourceIntakePage : public QWizardPage {
    Q_OBJECT
  public:
    explicit SourceIntakePage(CreatorPackagingContext *ctx, QWidget *parent = nullptr);
    ~SourceIntakePage() override = default;

    void initializePage() override;
    bool validatePage() override;
    bool isComplete() const override;

    void setSourcePath(const QString &path);

  private:
    void setupUi();
    void analyzeSourcePath(const QString &path);

    CreatorPackagingContext *m_ctx{nullptr};
    QLineEdit *m_sourcePathEdit{nullptr};
    QPushButton *m_browseDirBtn{nullptr};
    QPushButton *m_browseFileBtn{nullptr};
    QComboBox *m_typeCombo{nullptr};
    QLabel *m_detectedTitleLabel{nullptr};
    QLabel *m_detectedAuthorLabel{nullptr};
    QLabel *m_entrypointLabel{nullptr};
    QLabel *m_fileCountLabel{nullptr};
    QLabel *m_statusLabel{nullptr};
};

/**
 * @brief Step 2: Asset Bundling & Optimization Page
 * Scans referenced assets, prunes unreferenced dead files, and transcodes textures.
 */
class AssetBundlingPage : public QWizardPage {
    Q_OBJECT
  public:
    explicit AssetBundlingPage(CreatorPackagingContext *ctx, QWidget *parent = nullptr);
    ~AssetBundlingPage() override = default;

    void initializePage() override;
    bool validatePage() override;

  private:
    void setupUi();
    void scanAssets();

    CreatorPackagingContext *m_ctx{nullptr};
    QTreeWidget *m_assetTree{nullptr};
    QCheckBox *m_pruneCheck{nullptr};
    QCheckBox *m_transcodeCheck{nullptr};
    QLabel *m_summaryLabel{nullptr};
    QPushButton *m_rescanBtn{nullptr};
};

/**
 * @brief Step 3: Metadata, SPDX License & Automated Preview Capture Page
 * Validates slug, SemVer, author information, SPDX license, and generates/loads 512x288 preview.
 */
class MetadataPreviewPage : public QWizardPage {
    Q_OBJECT
  public:
    explicit MetadataPreviewPage(CreatorPackagingContext *ctx, QWidget *parent = nullptr);
    ~MetadataPreviewPage() override = default;

    void initializePage() override;
    bool validatePage() override;
    bool isComplete() const override;

  private:
    void setupUi();
    void captureOffscreenPreview();
    void loadCustomPreview();
    void updatePreviewDisplay();
    void validateFields();

    CreatorPackagingContext *m_ctx{nullptr};
    QLineEdit *m_idEdit{nullptr};
    QLineEdit *m_slugEdit{nullptr};
    QLabel *m_slugValidationLabel{nullptr};
    QLineEdit *m_nameEdit{nullptr};
    QLineEdit *m_versionEdit{nullptr};
    QLabel *m_versionValidationLabel{nullptr};
    QLineEdit *m_authorNameEdit{nullptr};
    QLineEdit *m_authorEmailEdit{nullptr};
    QComboBox *m_licenseCombo{nullptr};
    QTextEdit *m_descriptionEdit{nullptr};
    QLineEdit *m_tagsEdit{nullptr};
    QComboBox *m_fpsCombo{nullptr};
    QSpinBox *m_vramSpin{nullptr};

    QLabel *m_previewDisplay{nullptr};
    QPushButton *m_captureBtn{nullptr};
    QPushButton *m_browsePreviewBtn{nullptr};
    QLabel *m_previewInfoLabel{nullptr};
};

/**
 * @brief Step 4: Deterministic Capability & Sandbox Audit Page
 * Executes we_compat_scanner, displays compatibility badge, estimated VRAM/FPS, and locked
 * capabilities.
 */
class CapabilityAuditPage : public QWizardPage {
    Q_OBJECT
  public:
    explicit CapabilityAuditPage(CreatorPackagingContext *ctx, QWidget *parent = nullptr);
    ~CapabilityAuditPage() override = default;

    void initializePage() override;
    bool validatePage() override;
    bool isComplete() const override;

  private:
    void setupUi();
    void runCompatibilityAudit();
    void applyAutoRepairs();

    CreatorPackagingContext *m_ctx{nullptr};
    QLabel *m_tierBadge{nullptr};
    QLabel *m_scoreLabel{nullptr};
    QProgressBar *m_scoreBar{nullptr};
    QLabel *m_metricsLabel{nullptr};

    QCheckBox *m_capComputeCheck{nullptr};
    QCheckBox *m_capAudioSpectrumCheck{nullptr};
    QCheckBox *m_capAudioPlaybackCheck{nullptr};
    QCheckBox *m_capScriptingCheck{nullptr};
    QCheckBox *m_capForeignToplevelCheck{nullptr};
    QCheckBox *m_capNetworkCheck{nullptr};    // Locked false
    QCheckBox *m_capFilesystemCheck{nullptr}; // Locked false

    QTreeWidget *m_issuesTree{nullptr};
    QLabel *m_repairsSummaryLabel{nullptr};
    QPushButton *m_applyRepairsBtn{nullptr};
};

/**
 * @brief Step 5: Ed25519 Key Management & Merkle Compilation Page
 * Manages Ed25519 signing keypair, canonicalizes manifest, builds and signs .wwpkg container.
 */
class CompilationSigningPage : public QWizardPage {
    Q_OBJECT
  public:
    explicit CompilationSigningPage(CreatorPackagingContext *ctx, QWidget *parent = nullptr);
    ~CompilationSigningPage() override = default;

    void initializePage() override;
    bool validatePage() override;
    bool isComplete() const override;

  private:
    void setupUi();
    void checkOrLoadKey();
    void generateKeypair();
    void browseKeyFile();
    void compileAndSignPackage();

    CreatorPackagingContext *m_ctx{nullptr};
    QLineEdit *m_keyPathEdit{nullptr};
    QLabel *m_keyFingerprintLabel{nullptr};
    QPushButton *m_generateKeyBtn{nullptr};
    QPushButton *m_browseKeyBtn{nullptr};

    QLineEdit *m_outPkgPathEdit{nullptr};
    QPushButton *m_browseOutBtn{nullptr};
    QPushButton *m_compileBtn{nullptr};
    QProgressBar *m_progressBar{nullptr};

    QTextEdit *m_logEdit{nullptr};
    QLabel *m_verificationBadge{nullptr};
};

/**
 * @brief Step 6: Open Hub Registry Publishing & Local Export Page
 * Displays content-addressed CID, outputs Open Hub registry index JSON, and provides 1-click local
 * hub installation.
 */
class HubPublishingExportPage : public QWizardPage {
    Q_OBJECT
  public:
    explicit HubPublishingExportPage(CreatorPackagingContext *ctx, QWidget *parent = nullptr);
    ~HubPublishingExportPage() override = default;

    void initializePage() override;

  signals:
    void packageInstallRequested(const QString &packagePath, const QString &targetOutput);

  private:
    void setupUi();
    void installToLocalHub();
    void copyCidToClipboard();
    void copyJsonToClipboard();
    void exportJsonToFile();
    void exportPackageTo();
    void openContainingFolder();

    CreatorPackagingContext *m_ctx{nullptr};
    QLabel *m_cidLabel{nullptr};
    QPushButton *m_copyCidBtn{nullptr};
    QTextEdit *m_registryJsonEdit{nullptr};
    QPushButton *m_copyJsonBtn{nullptr};
    QPushButton *m_saveJsonBtn{nullptr};

    QPushButton *m_installLocalBtn{nullptr};
    QLabel *m_installStatusLabel{nullptr};
    QPushButton *m_openFolderBtn{nullptr};
    QPushButton *m_exportPkgBtn{nullptr};
};

/**
 * @brief Guided 6-Step Creator Packaging Wizard for WayWal Open Hub packages (.wwpkg).
 */
class CreatorPackagingWizard : public QWizard {
    Q_OBJECT

  public:
    enum PageId {
        Page_SourceIntake = 0,
        Page_AssetBundling = 1,
        Page_MetadataPreview = 2,
        Page_CapabilityAudit = 3,
        Page_CompilationSigning = 4,
        Page_HubPublishingExport = 5,
    };

    explicit CreatorPackagingWizard(QWidget *parent = nullptr);
    ~CreatorPackagingWizard() override;

    void setSourcePath(const QString &path);
    [[nodiscard]] QString sourcePath() const;
    [[nodiscard]] QString compiledPackagePath() const;
    [[nodiscard]] QString contentIdentifier() const;

    [[nodiscard]] CreatorPackagingContext *context() { return &m_context; }
    [[nodiscard]] const CreatorPackagingContext *context() const { return &m_context; }

    [[nodiscard]] SourceIntakePage *sourceIntakePage() const { return m_intakePage; }
    [[nodiscard]] AssetBundlingPage *assetBundlingPage() const { return m_bundlingPage; }
    [[nodiscard]] MetadataPreviewPage *metadataPreviewPage() const { return m_metadataPage; }
    [[nodiscard]] CapabilityAuditPage *capabilityAuditPage() const { return m_auditPage; }
    [[nodiscard]] CompilationSigningPage *compilationSigningPage() const
    {
        return m_compilationPage;
    }
    [[nodiscard]] HubPublishingExportPage *hubPublishingExportPage() const { return m_exportPage; }

  signals:
    void packageInstallRequested(const QString &packagePath, const QString &targetOutput);

  private:
    void applyStudioTheme();

    CreatorPackagingContext m_context;
    SourceIntakePage *m_intakePage{nullptr};
    AssetBundlingPage *m_bundlingPage{nullptr};
    MetadataPreviewPage *m_metadataPage{nullptr};
    CapabilityAuditPage *m_auditPage{nullptr};
    CompilationSigningPage *m_compilationPage{nullptr};
    HubPublishingExportPage *m_exportPage{nullptr};
};

} // namespace wwal::studio

namespace waywal::studio {
using ::wwal::studio::CreatorPackagingWizard;
}

#endif // WAYWAL_STUDIO_CREATOR_PACKAGING_WIZARD_HPP
