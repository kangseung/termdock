#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;

namespace termdock {

// 首次启动时选择数据保存目录（默认在软件自身目录下，便于整目录拷走）。
class DataDirDialog : public QDialog {
    Q_OBJECT

public:
    explicit DataDirDialog(const QString &defaultDir, QWidget *parent = nullptr);

    QString dataDir() const;

private:
    void browse();
    void accept() override;
    void updateHint();

    QLineEdit *m_path = nullptr;
    QLabel *m_hint = nullptr;
};

} // namespace termdock
