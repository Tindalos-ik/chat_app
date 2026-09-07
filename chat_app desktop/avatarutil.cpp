#include "avatarutil.h"

#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace {

constexpr int kAvatarCount = 5;

QString DefaultAvatarPath(int uid)
{
    const int index = uid > 0 ? uid % kAvatarCount : 0;
    return QStringLiteral(":/res/head_%1.jpg").arg(index + 1);
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

    QPixmap original(ResolvePath(uid, avatarPath));
    if (original.isNull()) {
        label->clear();
        return;
    }

    const QSize targetSize = label->size();
    if (targetSize.isEmpty()) {
        label->clear();
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

} // namespace AvatarUtil
