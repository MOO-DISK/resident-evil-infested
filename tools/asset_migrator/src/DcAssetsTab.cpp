#include "DcAssetsTab.h"

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

DcAssetsTab::DcAssetsTab(QWidget* parent) : QWidget(parent) {
    m_image = new QLineEdit(this);
    auto* browseImage = new QPushButton(tr("Browse..."), this);

    m_target = new QLineEdit(this);
    m_target->setText(QCoreApplication::applicationDirPath());
    auto* browseTarget = new QPushButton(tr("Browse..."), this);

    m_base = new QComboBox(this);
    m_base->addItem(tr("USA (North American / GOG)"), (int)re1::AssetVersion::USA);
    m_base->addItem(tr("Japanese (Biohazard)"), (int)re1::AssetVersion::JPN);

    m_convert = new QCheckBox(tr("Convert movies (STR -> MP4)"), this);
    m_convert->setChecked(true);
    m_verify = new QCheckBox(tr("Verify the overlay after building"), this);
    m_verify->setChecked(true);
    m_verify->setToolTip(
        tr("After writing DC/, run the read-only completeness check:\n"
           "\u2022 every file the base tree has is present in the overlay "
           "(except the folders the DC legitimately falls back for), and\n"
           "\u2022 every DC room resolves to a camera-0 background pak.\n"
           "A half-populated overlay fails quietly at runtime by mixing DC and "
           "base content, so this reports it as an error instead. It writes "
           "nothing; untick it to skip the check."));

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
    form->addRow(tr("Disc image:"), pathRow(m_image, browseImage));
    form->addRow(tr("Target game folder:"), pathRow(m_target, browseTarget));
    form->addRow(tr("Base tree:"), m_base);
    form->addRow(QString(), m_convert);
    form->addRow(QString(), m_verify);
    form->addRow(tr("ffmpeg:"), ffmpegRow);

    m_run = new RunPanel(this);

    auto* note = new QLabel(
        tr("Builds the DC overlay (<target>/DC) from a Director's Cut disc "
           "image. Every DC room and all backgrounds (STAGE1-7 and STAGE8-E) "
           "are migrated. A raw 2352-byte .bin/.cue image is required so the "
           "movies keep their CD-XA audio; an .iso is accepted but its audio is "
           "degraded. The base tree is never written to."),
        this);
    note->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note);
    layout->addLayout(form);
    layout->addWidget(m_run, 1);

    connect(browseImage, &QPushButton::clicked, this, &DcAssetsTab::onBrowseImage);
    connect(browseTarget, &QPushButton::clicked, this, &DcAssetsTab::onBrowseTarget);
    connect(browseFfmpeg, &QPushButton::clicked, this, &DcAssetsTab::onBrowseFfmpeg);
    connect(detectFfmpeg, &QPushButton::clicked, this, &DcAssetsTab::onDetectFfmpeg);
    connect(m_convert, &QCheckBox::toggled, this, &DcAssetsTab::syncEnabled);

    m_run->setTaskFactory([this]() -> Worker* {
        re1::DcMigrationOptions opts;
        opts.imagePath = m_image->text().toStdString();
        opts.targetRoot = m_target->text().toStdString();
        opts.base = (re1::AssetVersion)m_base->currentData().toInt();
        opts.convertMovies = m_convert->isChecked();
        opts.verify = m_verify->isChecked();
        opts.ffmpegPath = m_ffmpeg->text().toStdString();
        return new Worker(
            [opts](const re1::Progress& p, QString& error) {
                std::string err;
                const bool ok = re1::migrateDcAssets(opts, p, &err);
                if (!ok) error = QString::fromStdString(err);
                return ok;
            });
    });

    syncEnabled();
}

void DcAssetsTab::syncEnabled() {
    m_ffmpeg->setEnabled(m_convert->isChecked());
}

void DcAssetsTab::onBrowseImage() {
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Select the Director's Cut disc image"), m_image->text(),
        tr("Disc images (*.bin *.cue *.iso);;All files (*)"));
    if (!f.isEmpty()) m_image->setText(f);
}

void DcAssetsTab::onBrowseTarget() {
    const QString d = QFileDialog::getExistingDirectory(
        this, tr("Select the game folder"), m_target->text());
    if (!d.isEmpty()) m_target->setText(d);
}

void DcAssetsTab::onBrowseFfmpeg() {
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Select ffmpeg"), m_ffmpeg->text(),
        tr("ffmpeg (ffmpeg*.exe);;All files (*)"));
    if (!f.isEmpty()) m_ffmpeg->setText(f);
}

void DcAssetsTab::onDetectFfmpeg() {
    const QString f = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!f.isEmpty()) {
        m_ffmpeg->setText(f);
        m_run->appendLog(tr("Found ffmpeg: %1").arg(f));
    } else {
        m_run->appendLog(tr("ffmpeg was not found on PATH."));
    }
}
