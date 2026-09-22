#include "PcAssetsTab.h"

#include "RunPanel.h"
#include "Worker.h"
#include "core/Migrate.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace {

QWidget* pathRow(QLineEdit* edit, QPushButton* browse) {
    auto* w = new QWidget();
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(edit, 1);
    l->addWidget(browse);
    return w;
}

}  // namespace

PcAssetsTab::PcAssetsTab(QWidget* parent) : QWidget(parent) {
    m_sourceKind = new QComboBox(this);
    m_sourceKind->addItem(tr("Folder (already extracted)"), false);
    m_sourceKind->addItem(tr("Disc image (.iso / .bin / .cue)"), true);

    m_source = new QLineEdit(this);
    auto* browseSource = new QPushButton(tr("Browse..."), this);

    m_target = new QLineEdit(this);
    m_target->setText(QCoreApplication::applicationDirPath());
    auto* browseTarget = new QPushButton(tr("Browse..."), this);

    m_version = new QComboBox(this);
    m_version->addItem(tr("USA (North American / GOG)"), (int)re1::AssetVersion::USA);
    m_version->addItem(tr("Japanese (Biohazard) / GOG"), (int)re1::AssetVersion::JPN);

    m_convert = new QCheckBox(tr("Convert movies (AVI -> MP4)"), this);
    m_keepAvi = new QCheckBox(tr("Keep the original .avi next to the .mp4"), this);
    m_keepAvi->setChecked(true);

    m_ffmpeg = new QLineEdit(this);
    m_ffmpeg->setText(QStringLiteral("ffmpeg"));
    auto* browseFfmpeg = new QPushButton(tr("Browse..."), this);
    auto* detectFfmpeg = new QPushButton(tr("Detect"), this);
    auto* ffmpegRow = new QWidget();
    {
        auto* l = new QHBoxLayout(ffmpegRow);
        l->setContentsMargins(0, 0, 0, 0);
        l->addWidget(m_ffmpeg, 1);
        l->addWidget(browseFfmpeg);
        l->addWidget(detectFfmpeg);
    }

    auto* form = new QFormLayout();
    form->addRow(tr("Source type:"), m_sourceKind);
    form->addRow(tr("Source:"), pathRow(m_source, browseSource));
    form->addRow(tr("Target game folder:"), pathRow(m_target, browseTarget));
    form->addRow(tr("Asset type:"), m_version);
    form->addRow(QString(), m_convert);
    form->addRow(QString(), m_keepAvi);
    form->addRow(tr("ffmpeg:"), ffmpegRow);

    m_run = new RunPanel(this);

    auto* note = new QLabel(
        tr("Copies the release's asset folders into <target>/USA or "
           "<target>/JPN. From a disc image the folders are extracted first. "
           "The base trees are only added to, never replaced."),
        this);
    note->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note);
    layout->addLayout(form);
    layout->addWidget(m_run, 1);

    connect(m_sourceKind, &QComboBox::currentIndexChanged, this,
            &PcAssetsTab::onSourceKindChanged);
    connect(browseSource, &QPushButton::clicked, this, &PcAssetsTab::onBrowseSource);
    connect(browseTarget, &QPushButton::clicked, this, &PcAssetsTab::onBrowseTarget);
    connect(browseFfmpeg, &QPushButton::clicked, this, &PcAssetsTab::onBrowseFfmpeg);
    connect(detectFfmpeg, &QPushButton::clicked, this, &PcAssetsTab::onDetectFfmpeg);
    connect(m_convert, &QCheckBox::toggled, this, &PcAssetsTab::syncEnabled);

    m_run->setTaskFactory([this]() -> Worker* {
        re1::PcMigrationOptions opts;
        opts.sourcePath = m_source->text().toStdString();
        opts.sourceIsImage = m_sourceKind->currentData().toBool();
        opts.targetRoot = m_target->text().toStdString();
        opts.version =
            (re1::AssetVersion)m_version->currentData().toInt();
        opts.convertMovies = m_convert->isChecked();
        opts.keepAvi = m_keepAvi->isChecked();
        opts.ffmpegPath = m_ffmpeg->text().toStdString();
        return new Worker(
            [opts](const re1::Progress& p, QString& error) {
                std::string err;
                const bool ok = re1::migratePcAssets(opts, p, &err);
                if (!ok) error = QString::fromStdString(err);
                return ok;
            });
    });

    syncEnabled();
}

void PcAssetsTab::syncEnabled() {
    const bool image = m_sourceKind->currentData().toBool();
    m_keepAvi->setEnabled(m_convert->isChecked());
    m_ffmpeg->setEnabled(m_convert->isChecked());
    (void)image;
}

void PcAssetsTab::onSourceKindChanged() {
    m_source->clear();
    syncEnabled();
}

void PcAssetsTab::onBrowseSource() {
    if (m_sourceKind->currentData().toBool()) {
        const QString f = QFileDialog::getOpenFileName(
            this, tr("Select a disc image"), m_source->text(),
            tr("Disc images (*.iso *.bin *.cue);;All files (*)"));
        if (!f.isEmpty()) m_source->setText(f);
    } else {
        const QString d = QFileDialog::getExistingDirectory(
            this, tr("Select the extracted asset folder"), m_source->text());
        if (!d.isEmpty()) m_source->setText(d);
    }
}

void PcAssetsTab::onBrowseTarget() {
    const QString d = QFileDialog::getExistingDirectory(
        this, tr("Select the game folder"), m_target->text());
    if (!d.isEmpty()) m_target->setText(d);
}

void PcAssetsTab::onBrowseFfmpeg() {
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Select ffmpeg"), m_ffmpeg->text(),
        tr("ffmpeg (ffmpeg*.exe);;All files (*)"));
    if (!f.isEmpty()) m_ffmpeg->setText(f);
}

void PcAssetsTab::onDetectFfmpeg() {
    const QString f = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!f.isEmpty()) {
        m_ffmpeg->setText(f);
        m_run->appendLog(tr("Found ffmpeg: %1").arg(f));
    } else {
        m_run->appendLog(tr("ffmpeg was not found on PATH."));
    }
}
