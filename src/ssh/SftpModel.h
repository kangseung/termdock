#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QString>
#include <QVector>

#include "ssh/SftpTypes.h"

namespace termdock {

// 远端目录树的懒加载模型：只有展开的目录才会发 SFTP 请求。
class SftpModel : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Column {
        NameColumn = 0,
        SizeColumn,
        ModifiedColumn,
        ModeColumn,
        ColumnCount,
    };

    explicit SftpModel(QObject *parent = nullptr);
    ~SftpModel() override;

    void setHomePath(const QString &path);
    QString homePath() const { return m_root ? m_root->entry.path : QString(); }
    QString pathForIndex(const QModelIndex &index) const;
    QString nameForIndex(const QModelIndex &index) const;
    bool isDirIndex(const QModelIndex &index) const;
    QModelIndex indexForPath(const QString &path) const;

    void applyListing(const QString &path, const SftpEntryList &entries);
    void markLoadingFailed(const QString &path);
    void refresh(const QModelIndex &index = QModelIndex());

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;
    // 未展开过的目录也要报“有子项”，否则视图不画展开箭头
    bool hasChildren(const QModelIndex &parent = QModelIndex()) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

signals:
    void listRequested(const QString &path);

private:
    struct Node {
        SftpEntry entry;
        Node *parent = nullptr;
        QVector<Node *> children;
        bool fetched = false;
        bool loading = false;
    };

    Node *nodeFromIndex(const QModelIndex &index) const;
    QModelIndex indexForNode(const Node *node) const;
    void clearChildren(Node *node);
    void deleteNode(Node *node);
    void requestListing(Node *node);
    void collectPath(Node *node, const QString &path, QVector<Node *> *out) const;

    Node *m_root = nullptr;
    QHash<QString, QVector<Node *>> m_pending;
};

} // namespace termdock
