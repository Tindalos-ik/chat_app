# Qt 头像统一显示

客户端同一位用户会出现在会话列表、联系人列表、好友资料、好友申请和消息气泡中。每个页面各自缩放或在头像为空时随机选图，会让同一用户在不同位置看起来不一样。

## 统一入口

所有用户头像通过 `chat_app desktop/avatarutil.h` 的两个函数处理：

| 函数 | 用途 |
| --- | --- |
| `AvatarUtil::ResolvePath(uid, path)` | 有头像路径时保留路径；空路径时按 UID 返回稳定的内置头像。 |
| `AvatarUtil::SetRoundAvatar(label, uid, path)` | 保持比例放大并中心裁切，再渲染为圆形头像。 |

```cpp
AvatarUtil::SetRoundAvatar(ui->icon_lb, userInfo->_uid, userInfo->_icon);
```

## 兜底规则

服务端没有返回头像时，不要随机选择 `head_1.jpg` 到 `head_5.jpg`。统一使用 UID 对头像数量取模，同一个 UID 在列表、详情和消息中都会得到相同的内置头像。

搜索接口当前不包含头像字段，因此搜索结果也传 UID 和空路径，由同一规则显示兜底头像。

> 新增含用户头像的界面时，不要重复编写 `QPainterPath` 裁切代码，直接调用 `AvatarUtil::SetRoundAvatar`。
