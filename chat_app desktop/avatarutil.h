#ifndef AVATARUTIL_H
#define AVATARUTIL_H

#include <QString>

class QLabel;

namespace AvatarUtil {

QString ResolvePath(int uid, const QString &avatarPath);
void SetRoundAvatar(QLabel *label, int uid, const QString &avatarPath);

} // namespace AvatarUtil

#endif // AVATARUTIL_H
