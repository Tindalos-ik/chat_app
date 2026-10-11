#ifndef AVATARUTIL_H
#define AVATARUTIL_H

#include <QString>

class QLabel;
class QImage;

namespace AvatarUtil {

QString ResolvePath(int uid, const QString &avatarPath);
void SetRoundAvatar(QLabel *label, int uid, const QString &avatarPath);
/** 将群 PNG/JPEG 本地缓存裁为圆形；cachePath 为空或解码失败时显示稳定默认群图标。 */
void SetRoundGroupAvatar(QLabel *label, const QString &cachePath);
/** 将已解码的群头像预览裁为圆形；image 为空时显示同一默认群图标。 */
void SetRoundGroupAvatar(QLabel *label, const QImage &image);

} // namespace AvatarUtil

#endif // AVATARUTIL_H
