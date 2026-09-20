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

## 上传与保存

个人设置页先通过 ResourceClient 把图片分片上传到 ResourceServer。最终分片落盘并原子重命名后，服务端回包才携带 `resource_url`；随后客户端通过 1031 把昵称、签名和该地址交给当前 ChatServer 保存。上传成功但资料更新失败时不能提前改写本地 UserMgr，用户可以再次提交。

当前 ResourceServer 尚未提供 HTTP 静态文件接口，`resource_url` 是资源服务机器上的绝对文件路径，适用于客户端与 ResourceServer 同机的开发部署。客户端头像工具也支持 HTTP(S) 地址；跨机器部署前只需让资源服务返回真实的 HTTP/CDN URL。ChatServer 始终把该字段当作不透明字符串保存。
