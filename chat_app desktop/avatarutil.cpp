#include "avatarutil.h"

#include <QLabel>
#include <QCoreApplication>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QUrl>

namespace {

constexpr int kAvatarCount = 5;

QString DefaultAvatarPath(int uid)
{
    const int index = uid > 0 ? uid % kAvatarCount : 0;
    return QStringLiteral(":/res/head_%1.jpg").arg(index + 1);
}

void ApplyRoundedPixmap(QLabel *label, QPixmap original)
{
    if (label == nullptr || original.isNull()) {
        return;
    }

    const QSize targetSize = label->size();
    if (targetSize.isEmpty()) {
        return;
    }

    original = original.scaled(targetSize, Qt::KeepAspectRatioByExpanding,
                               Qt::SmoothTransformation);
    QPixmap rounded(targetSize);
    rounded.fill(Qt::transparent);

    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addEllipse(rounded.rect());
    painter.setClipPath(clip);
    painter.drawPixmap((targetSize.width() - original.width()) / 2,
                       (targetSize.height() - original.height()) / 2, original);
    label->setPixmap(rounded);
}

QNetworkAccessManager *AvatarNetworkManager()
{
    static auto *manager = new QNetworkAccessManager(QCoreApplication::instance());
    return manager;
}

QHash<QString, QPixmap> &AvatarCache()
{
    static QHash<QString, QPixmap> cache;
    return cache;
}

} // namespace

namespace AvatarUtil {

QString ResolvePath(int uid, const QString &avatarPath)
{
    return avatarPath.trimmed().isEmpty() ? DefaultAvatarPath(uid) : avatarPath;
}

void SetRoundAvatar(QLabel *label, int uid, const QString &avatarPath)
{
    if (label == nullptr) {
        return;
    }

    const QString source = ResolvePath(uid, avatarPath);
    label->setProperty("avatar_source", source);
    const QUrl url(source);
    if (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) {
        QPixmap original(source);
        if (original.isNull()) {
            original.load(DefaultAvatarPath(uid));
        }
        ApplyRoundedPixmap(label, original);
        return;
    }

    // UserMgr 保存的是服务端资源地址。远端头像下载期间先显示稳定的本地默认头像，
    // 并用 label 上的 source 标记阻止旧请求覆盖刚切换的新头像。
    ApplyRoundedPixmap(label, QPixmap(DefaultAvatarPath(uid)));
    const auto cached = AvatarCache().constFind(source);
    if (cached != AvatarCache().cend()) {
        ApplyRoundedPixmap(label, cached.value());
        return;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = AvatarNetworkManager()->get(request);
    const QPointer<QLabel> guardedLabel(label);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, guardedLabel, source]() {
        const QByteArray bytes = reply->readAll();
        const bool succeeded = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (!succeeded || guardedLabel.isNull()
            || guardedLabel->property("avatar_source").toString() != source) {
            return;
        }
        QPixmap downloaded;
        if (!downloaded.loadFromData(bytes)) {
            return;
        }
        AvatarCache().insert(source, downloaded);
        ApplyRoundedPixmap(guardedLabel, downloaded);
    });
}

} // namespace AvatarUtil
