# agentlink-desktop 的容器构建/运行环境。
#
# 用途：
#   1. 构建与验收（无显示环境）：cmake + ctest 都在容器里跑，宿主机不需要装 Qt
#   2. 把 agentlink-qt 作为一个"随时能跑"的二进制带在镜像里
#
#   docker build -t agentlink-desktop:latest .
#   docker run --rm agentlink-desktop:latest --version        # GUI 能起来
#   docker run --rm --entrypoint sh agentlink-desktop:latest -c "ctest --test-dir /build"  # 单测
#
# 端到端测试需要一台真的 AgentLink 服务端，见 scripts/acceptance.mjs。
FROM ubuntu:22.04 AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates \
      qt6-base-dev qt6-base-dev-tools \
      libqt6websockets6-dev \
      libgl1-mesa-dev libx11-dev libxext-dev \
      libxcb1-dev libxcb-util-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY tests ./tests

# 构建目录用 /build（不是 /src/build）：CMake 会把**绝对路径**写进
# CTestTestfile.cmake，之后把 /build 整个拷走时路径仍然成立。
RUN cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /build -j"$(nproc)"

# ---- 运行阶段 ----
FROM ubuntu:22.04 AS runtime

# 注意包名：Ubuntu 22.04 (jammy) 是 libqt6core6，**没有** t64 后缀
# （t64 是 Debian trixie / Ubuntu 24.10 之后的命名）。
#
# qt6-qpa-plugins 里装着**两个**必需的东西：
#   - plugins/tls/libqopensslbackend.so —— 没有它 Qt 报 "No functional TLS backend"，
#     wss:// 根本连不上（生产环境必须）
#   - plugins/platforms/libqxcb.so —— 有显示环境时的界面后端
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libqt6core6 libqt6network6 libqt6widgets6 libqt6gui6 \
      libqt6websockets6 libqt6test6 cmake qt6-qpa-plugins \
      libssl3 \
      libgl1 libegl1 libxkbcommon0 libxkbcommon-x11-0 libdbus-1-3 \
      fonts-noto-cjk \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /build /build
RUN ln -s /build/agentlink-qt /usr/local/bin/agentlink-qt \
 && ln -s /build/test-frames /usr/local/bin/agentlink-test-frames \
 && ln -s /build/test-address /usr/local/bin/agentlink-test-address \
 && ln -s /build/test-settings /usr/local/bin/agentlink-test-settings \
 && ln -s /build/test-e2e /usr/local/bin/agentlink-test-e2e

# 有显示环境时（-e DISPLAY -v /tmp/.X11-unix）直接出界面；
# 没有时用 --version / ctest 做无头验证。
ENTRYPOINT ["/usr/local/bin/agentlink-qt"]
