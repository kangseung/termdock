#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace termdock {

// 一条会话配置：双击列表项即可连上，不需要再记 IP / 账号 / 密码。
struct Session {
    QString id;
    QString name;
    QString host;
    int port = 22;
    QString user;
    QString password;
    // 原始密文：解密失败（比如 key 丢了）时原样写回，避免把还能救的密码抹掉
    QString passwordCipher; // 内存中为明文，落盘时由 Crypto 加密
    QString note;
    QString color;

    QString displayTarget() const;
    QString displaySubtitle() const;
    bool isValid() const { return !host.isEmpty() && !user.isEmpty(); }

    QJsonObject toJson() const;
    static Session fromJson(const QJsonObject &object, bool *passwordDecryptFailed = nullptr);
    static Session createEmpty();
    static QString newId();
};

using SessionList = QVector<Session>;

} // namespace termdock
