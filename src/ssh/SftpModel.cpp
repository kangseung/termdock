#include "ssh/SftpModel.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileIconProvider>
#include <QStyle>

namespace termdock {

namespace {

QString formatSize(qint64 size, bool isDir)
{
    if (isDir)
        return QStringLiteral("<目录>");
    if (size < 1024)
        return QStringLiteral("%1 B").arg(size);
    const double kb = size / 1024.0;
    if (kb < 1024.0)
        return QStringLiteral("%1 KB").arg(kb, 0, 'f', 1);
    const double mb = kb / 1024.0;
    if (mb < 1024.0)
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 2);
}

QIcon iconFor(bool isDir)
{
    // 用自带的彩色图标：Fusion/QSS 皮肤下 style() 给的 SP_DirIcon 是深色的，
    // 放在深色背景上等于"看不见"（用户反馈文件夹图标黑咕隆咚）
    static const QIcon dirIcon(QStringLiteral(":/icons/folder.png"));
    static const QIcon fileIcon(QStringLiteral(":/icons/file.png"));
    return isDir ? dirIcon : fileIcon;
}

} // namespace

SftpModel::SftpModel(QObject *parent)
    : QAbstractItemModel(parent)
{
}

SftpModel::~SftpModel()
{
    delete m_root;
}

void SftpModel::deleteNode(Node *node)
{
    if (!node)
        return;
    for (Node *child : node->children)
        deleteNode(child);
    node->children.clear();
    delete node;
}

void SftpModel::setHomePath(const QString &path)
{
    beginResetModel();
    deleteNode(m_root);
    m_pending.clear();
    m_prefetchPaths.clear();
    m_probedDirs.clear();
    m_emptyDirs.clear();
    m_root = new Node();
    m_root->entry.path = path;
    m_root->entry.name = QFileInfo(path).fileName();
    if (m_root->entry.name.isEmpty())
        m_root->entry.name = path;
    m_root->entry.isDir = true;
    endResetModel();
    requestListing(m_root);
}

void SftpModel::clearChildren(Node *node)
{
    if (!node || node->children.isEmpty())
        return;
    for (Node *child : node->children)
        deleteNode(child);
    node->children.clear();
}

void SftpModel::requestListing(Node *node, bool prefetch)
{
    if (!node || node->loading || node->fetched)
        return;
    node->loading = true;
    m_pending[node->entry.path].append(node);
    if (prefetch)
        m_prefetchPaths.insert(node->entry.path);
    emit listRequested(node->entry.path, prefetch);
}

void SftpModel::markLoadingFailed(const QString &path)
{
    m_prefetchPaths.remove(path);
    const QVector<Node *> nodes = m_pending.take(path);
    for (Node *node : nodes)
        node->loading = false;
}

void SftpModel::applyListing(const QString &path, const SftpEntryList &entries)
{
    const bool probeOnly = m_prefetchPaths.remove(path);
    const QVector<Node *> nodes = m_pending.take(path);
    for (Node *node : nodes) {
        node->loading = false;
        node->fetched = true;
        const QModelIndex parentIndex = indexForNode(node);
        if (!entries.isEmpty()) {
            beginInsertRows(parentIndex, 0, int(entries.size()) - 1);
            node->children.reserve(entries.size());
            for (const SftpEntry &entry : entries) {
                Node *child = new Node();
                child->entry = entry;
                child->parent = node;
                node->children.append(child);
            }
            endInsertRows();
        }
        if (parentIndex.isValid())
            emit dataChanged(parentIndex, parentIndex);
    }
    // 记住哪些目录是空的：刷新后重新插进来的节点还没加载，靠这个集合照样不画箭头
    if (nodes.isEmpty())
        return;
    if (entries.isEmpty())
        m_emptyDirs.insert(path);
    else
        m_emptyDirs.remove(path);
    if (probeOnly)
        return;
    // 列完之后顺带问一下它直接子目录的内容，好知道哪些是空的（空目录不该画箭头）
    for (Node *node : nodes)
        probeChildDirs(node);
}

void SftpModel::probeChildren(const QModelIndex &parent)
{
    probeChildDirs(nodeFromIndex(parent));
}

void SftpModel::probeChildDirs(Node *node)
{
    if (!node)
        return;
    // 只探一层，不往下串；展开一层探一层；已经探过的目录不再重复探，
    // 刷新目录也不会把所有子目录重新探一遍。上限 64 个，免得超大目录把队列塞满。
    int budget = 64;
    for (Node *child : node->children) {
        if (budget <= 0)
            return;
        const QString &childPath = child->entry.path;
        if (!child->entry.isDir || child->fetched || m_probedDirs.contains(childPath))
            continue;
        --budget;
        m_probedDirs.insert(childPath);
        requestListing(child, true);
    }
}

void SftpModel::refresh(const QModelIndex &index)
{
    Node *node = index.isValid() ? nodeFromIndex(index) : m_root;
    if (!node)
        return;
    if (!node->children.isEmpty()) {
        beginRemoveRows(index, 0, int(node->children.size()) - 1);
        clearChildren(node);
        endRemoveRows();
    }
    node->fetched = false;
    node->loading = false;
    m_pending.remove(node->entry.path);
    m_prefetchPaths.remove(node->entry.path);
    if (index.isValid())
        requestListing(node);
    else
        requestListing(m_root);
}

QModelIndex SftpModel::indexForNode(const Node *node) const
{
    if (!node || node == m_root)
        return QModelIndex();
    const Node *parent = node->parent;
    if (!parent)
        return QModelIndex();
    const int row = int(parent->children.indexOf(const_cast<Node *>(node)));
    if (row < 0)
        return QModelIndex();
    return createIndex(row, NameColumn, const_cast<Node *>(node));
}

SftpModel::Node *SftpModel::nodeFromIndex(const QModelIndex &index) const
{
    if (!index.isValid())
        return m_root;
    return static_cast<Node *>(index.internalPointer());
}

QModelIndex SftpModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
        return QModelIndex();
    Node *parentNode = nodeFromIndex(parent);
    if (!parentNode || row < 0 || row >= int(parentNode->children.size()))
        return QModelIndex();
    return createIndex(row, column, parentNode->children[row]);
}

QModelIndex SftpModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return QModelIndex();
    Node *node = nodeFromIndex(child);
    if (!node || !node->parent || node->parent == m_root)
        return QModelIndex();
    return indexForNode(node->parent);
}

int SftpModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0)
        return 0;
    const Node *node = nodeFromIndex(parent);
    return node ? int(node->children.size()) : 0;
}

int SftpModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return ColumnCount;
}

QVariant SftpModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();
    const Node *node = nodeFromIndex(index);
    if (!node)
        return QVariant();
    const SftpEntry &entry = node->entry;

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case NameColumn:
            return entry.name;
        case SizeColumn:
            return formatSize(entry.size, entry.isDir);
        case ModifiedColumn:
            if (entry.mtime <= 0)
                return QString();
            return QDateTime::fromSecsSinceEpoch(entry.mtime).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        case ModeColumn:
            return entry.permissionsText();
        default:
            break;
        }
        return QVariant();
    case Qt::DecorationRole:
        if (index.column() == NameColumn)
            return iconFor(entry.isDir);
        return QVariant();
    case Qt::TextAlignmentRole:
        if (index.column() == SizeColumn)
            return QVariant(int(Qt::AlignRight | Qt::AlignVCenter));
        return QVariant(int(Qt::AlignLeft | Qt::AlignVCenter));
    case Qt::ToolTipRole:
        return QStringLiteral("%1\n%2\n权限 %3\n大小 %4 字节")
            .arg(entry.path, entry.isDir ? QStringLiteral("目录") : QStringLiteral("文件"),
                 entry.permissionsText(), QString::number(entry.size));
    default:
        return QVariant();
    }
}

QVariant SftpModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();
    switch (section) {
    case NameColumn:
        return QStringLiteral("名称");
    case SizeColumn:
        return QStringLiteral("大小");
    case ModifiedColumn:
        return QStringLiteral("修改时间");
    case ModeColumn:
        return QStringLiteral("权限");
    default:
        return QVariant();
    }
}

bool SftpModel::canFetchMore(const QModelIndex &parent) const
{
    if (parent.column() > 0)
        return false;
    const Node *node = nodeFromIndex(parent);
    if (!node)
        return false;
    return node->entry.isDir && !node->fetched && !node->loading;
}

void SftpModel::fetchMore(const QModelIndex &parent)
{
    Node *node = nodeFromIndex(parent);
    requestListing(node);
}

bool SftpModel::hasChildren(const QModelIndex &parent) const
{
    if (parent.column() > 0)
        return false;
    const Node *node = nodeFromIndex(parent);
    if (!node)
        return false;
    if (!node->children.isEmpty())
        return true;
    // 还没展开过、内容未知的目录也算“有子项”：QTreeView 只根据 hasChildren
    // 决定画不画展开箭头（canFetchMore 不算），不这么写用户看不出哪些目录能展开。
    // 探过且确认为空的目录（哪怕刷新后重新插入、尚未加载）也不再画箭头。
    return node->entry.isDir && !node->fetched && !m_emptyDirs.contains(node->entry.path);
}

Qt::ItemFlags SftpModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QString SftpModel::pathForIndex(const QModelIndex &index) const
{
    const Node *node = nodeFromIndex(index);
    return node ? node->entry.path : QString();
}

QString SftpModel::nameForIndex(const QModelIndex &index) const
{
    const Node *node = nodeFromIndex(index);
    return node ? node->entry.name : QString();
}

bool SftpModel::isDirIndex(const QModelIndex &index) const
{
    const Node *node = nodeFromIndex(index);
    return node && node->entry.isDir;
}

void SftpModel::collectPath(Node *node, const QString &path, QVector<Node *> *out) const
{
    if (!node)
        return;
    if (node->entry.path == path)
        out->append(node);
    for (Node *child : node->children)
        collectPath(child, path, out);
}

QModelIndex SftpModel::indexForPath(const QString &path) const
{
    QVector<Node *> nodes;
    collectPath(m_root, path, &nodes);
    if (nodes.isEmpty())
        return QModelIndex();
    return indexForNode(nodes.first());
}

} // namespace termdock
