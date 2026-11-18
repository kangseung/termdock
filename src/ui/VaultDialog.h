#pragma once

#include <QDialog>
#include <QStringList>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace termdock {

// 「密码恢复保险」与明文查看：可开可关，开启后用恢复口令随时解出已保存的密码。
class VaultDialog : public QDialog {
    Q_OBJECT

public:
    explicit VaultDialog(QWidget *parent = nullptr);

    // 供首次启动/菜单调用：询问口令并开启保险
    static bool promptEnable(QWidget *parent);

private:
    void setupUi();
    void refreshState();
    void enableRecovery();
    void disableRecovery();
    void unlock();
    void loadTable(const QByteArray &masterKey);
    void clearTable();
    void applyMask();

    QLabel *m_stateLabel = nullptr;
    QLabel *m_hint = nullptr;
    QLineEdit *m_passphrase = nullptr;
    QPushButton *m_unlockButton = nullptr;
    QPushButton *m_enableButton = nullptr;
    QPushButton *m_disableButton = nullptr;
    QCheckBox *m_showPlain = nullptr;
    QTableWidget *m_table = nullptr;
    QStringList m_secrets;
};

} // namespace termdock
