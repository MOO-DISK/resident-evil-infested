#pragma once
// "PC assets" tab: migrate a USA or JPN asset tree from a folder or a disc image.

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLineEdit;
class RunPanel;

class PcAssetsTab : public QWidget {
    Q_OBJECT
public:
    explicit PcAssetsTab(QWidget* parent = nullptr);

private slots:
    void onSourceKindChanged();
    void onBrowseSource();
    void onBrowseTarget();
    void onBrowseFfmpeg();
    void onDetectFfmpeg();

private:
    void syncEnabled();

    QComboBox* m_sourceKind = nullptr;
    QLineEdit* m_source = nullptr;
    QLineEdit* m_target = nullptr;
    QComboBox* m_version = nullptr;
    QCheckBox* m_convert = nullptr;
    QCheckBox* m_keepAvi = nullptr;
    QLineEdit* m_ffmpeg = nullptr;
    RunPanel* m_run = nullptr;
};
