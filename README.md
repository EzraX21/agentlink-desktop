# AgentLink Desktop

[AgentLink](https://github.com/EzraX21/agentlink) 的原生 C++/Qt 6 桌面客户端，
不是网页套壳。

用户要的是"像 QQ / Element 那样"：**客户端装在任意电脑上，服务端部署在服务器上**，
客户端**自己手填服务器地址**，先注册账号、再连上去聊天。

Web 客户端做不到这点（它是跑在浏览器里的，地址由部署方决定）。
所以需要一个原生应用：

- 客户端里**不写死任何地址**，全靠用户填 + 本地记住
- 一台客户端可以连**多台**服务端（公司一台、私人一台、测试一台）
- 服务端部署在哪儿都无所谓，客户端只认地址

## 架构

```
agentlink-core（静态库，不依赖 GUI）
  ├── frames    帧编解码（req/res/evt，与服务端 @agentlink/protocol 同一份协议）
  ├── client    连接管理：WS 握手 → auth → 请求路由 → 事件分发 → 断线重连 + resume
  ├── auth_api  HTTP 面：注册 / 登录 / 刷新 token / 上传下载附件 / 缩略图
  └── settings  本地存储：服务器列表 + 登录态

agentlink-qt（可执行程序，依赖 Widgets）
  ├── Onboarding       引导：填地址 → 注册/登录
  ├── MainWindow       胶水层：事件分发 + 菜单（建群 / 群信息 / 搜索 / 切换服务器）
  ├── ConversationList 左侧：会话列表
  ├── ChatWindow       右侧：消息流（QListWidget + 每条消息一个 MessageItem）
  ├── MessageItem      单条消息：文本 / 图片缩略图 / 视频缩略图 / 流式 + 打断
  ├── CreateGroupDialog / GroupInfoDialog / MembersDialog / SearchPanel
```

**为什么协议核心单独成库且不依赖 GUI**：协议逻辑是最容易出错、也最值得测的部分，
但 GUI 测试要起显示服务。拆成 `agentlink_core` 之后，验收脚本可以**不起窗口**
就把帧编解码、地址解析、设置读写、以及**真连服务端**的端到端全跑一遍。

## 构建

需要 Qt 6（Core / Network / Widgets / Test）、CMake、一个 C++17 编译器。

```bash
# Ubuntu 22.04
sudo apt-get install build-essential cmake qt6-base-dev qt6-base-dev-tools

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

./build/agentlink-qt                      # 启动客户端
ctest --test-dir build --output-on-failure  # 测试
```

只想编译协议核心（不想要 GUI 依赖）：`-DBUILD_GUI=OFF`。

### 容器构建（环境固定，适合 CI）

```bash
docker build -t agentlink-desktop:latest .
docker run --rm agentlink-desktop:latest --version   # GUI 二进制能起来
```

### 一键验收（真连服务端的端到端）

```bash
node scripts/acceptance.mjs
```

脚本自己准备两个镜像（客户端镜像就在本仓库；服务端镜像本地没有就
从 agentlink 仓库构建，可用 `AGENTLINK_SERVER_DIR` 指向本地检出），
起一个隔离服务端容器，然后跑：GUI 冒烟 + 协议单测 + 真注册真建群真发消息的端到端。

## 首次使用（引导流程）

1. **填服务器地址**
   可以写 `chat.example.com`，也可以写完整的 `https://chat.example.com`
   （`https` 自动映射到 `wss`）。地址与用户填的原文都会记下来。
2. **注册或登录**
   新服务器 → 注册账号（密码至少 8 位，与服务端一致）；已有账号 → 登录。
3. **进聊天**

地址与登录态写到 `QStandardPaths::AppConfigLocation`（Linux 上是
`~/.config/AgentLink/client.ini`），下次启动直接连上次那台，不再问。

**随时能加/换服务器**：菜单「服务器 → 添加 / 切换服务器…」会再走一遍同一套引导，
填新地址、注册或登录，然后用新地址与新 token 就地重连（不用重启进程）。

## 连接逻辑

- 启动时优先用**上次激活的服务器**；没有才进引导
- `auth` 失败**不重连** —— token 错了，重连多少次都一样；给用户重新登录
- 非认证失败的断线按 500ms / 1s / 2s / …（封顶 30s）退避重连，重连成功后用
  `resume`（协议 §3.3）把断线期间的全局 seq 补回来
- 请求有 30 秒超时，超时回调明确失败而不是永远挂住

## 与服务端的关系

客户端**只实现协议**，不碰数据库、不碰配置。服务端把它当成一个普通的人类客户端
（协议 §2 的 `auth` 用 access token 握手，与 Web 客户端完全一样）。

所以：

- 服务端可以是同源托管前端的形态（单容器），也可以是**只开 API**的分离形态
- 客户端连 `ws://` 还是 `wss://` 取决于你填的地址（明文还是 TLS）
- 生产环境请用 `wss://` —— 聊天内容走明文 WebSocket 等于裸奔

## 已知限制

- **token 明文存本地**。这是桌面客户端的常态（Element 也这样），但意味着
  能读你 home 目录的人能拿到你的聊天身份。v1 不接系统钥匙串（那要额外的
  平台依赖：Linux 上是 libsecret / kwallet，各家还不一样）。
- **不做端到端加密**。协议明确排除（需求确认书 §5）。
- **UI 是功能导向的**：会话列表、消息流（含内联图片/视频缩略图）、回复、撤回、
  打断 AI、建群、群信息、**成员管理（邀请/踢出/禁言/设角色）**、消息搜索、
  表情回应、编辑、置顶、转发、正在输入、已读回执、发附件。不做多语言、不做移动端、不做音视频。
- **编辑/撤回消息用"重拉历史"**重渲染，而不是增量更新。v1 频率低，不值得做增量。

## 测试

```bash
ctest --test-dir build --output-on-failure

# 端到端（需要先起一个服务端）
AGENTLINK_TEST_URL=http://127.0.0.1:3000 \
AGENTLINK_TEST_WS_URL=ws://127.0.0.1:3000/ws \
ctest --test-dir build -R e2e --output-on-failure
```

| 测试 | 覆盖 |
|---|---|
| `test-frames` | 帧编解码：req/res/evt、瞬时事件 seq=null、坏帧拒收、1 MiB 上限、往返一致 |
| `test-address` | 服务器地址解析：补协议头、https→wss、保端口、拒垃圾输入、http base |
| `test-settings` | 多服务器读写：覆盖不丢激活标记、激活切换、重载持久化 |
| `test-e2e` | **真连服务端**：注册 → WS 认证 → 建群 → 发消息 → 收推送 → 读历史 → 上传附件 → 回复 → 撤回 → 群信息 → 搜索 → 表情回应 → 编辑 → 置顶 → 转发 → 成员管理（邀请/设管理员/禁言/踢出） |

`test-e2e` 在没有 `AGENTLINK_TEST_URL` 时**跳过**（退出码 77），不是失败。

## 许可证

[GPL-3.0-or-later](LICENSE)，与服务端相同。
