#!/usr/bin/env node
/**
 * agentlink-desktop 验收：协议核心**真连**一个 AgentLink 服务端。
 *
 * 验的是**客户端侧**的协议正确性：Web 客户端跑的那套协议，
 * C++ 客户端能不能说对、能不能真的完成注册→认证→建群→发消息→收推送→上传附件。
 * 帧格式或握手顺序错了，这里会红。
 *
 * ## 怎么跑
 *
 *   node scripts/acceptance.mjs
 *
 * 本脚本自己把两件事准备好（缺什么建什么）：
 *   1. agentlink-desktop:latest —— 本仓库的 Dockerfile
 *   2. agentlink-server:latest  —— 服务端。优先用本地已构建的；
 *      没有就用 AGENTLINK_SERVER_DIR 指向的 agentlink 检出；
 *      再没有就直接从 GitHub 拉（docker build 接受 git URL）。
 *
 * 然后做四件事：
 *   1. 起一个**隔离的服务端**容器（独立数据卷 + 随机端口，跑完删掉）
 *   2. `agentlink-qt --version`：GUI 二进制能起来（平台插件 / TLS 后端 / 动态库都在位）
 *   3. 协议层单测（frames / address / settings，不需要服务端）
 *   4. 端到端：真注册、真建群、真发消息、真收消息、真传附件
 */
import { spawn } from 'node:child_process';
import { createServer } from 'node:net';
import { mkdirSync, readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import zlib from 'node:zlib';

// ---- 极简断言（让本仓库不依赖 agentlink 仓库的任何东西） ----
let failed = 0;
const section = (t) => console.log(`\n\x1b[1m${t}\x1b[0m`);
function ok(cond, label) {
  console.log(`  ${cond ? '\x1b[32m✓\x1b[0m' : '\x1b[31m✗\x1b[0m'} ${label}`);
  if (!cond) failed++;
}
function eq(actual, expected, label) {
  ok(actual === expected, `${label}（实际 ${JSON.stringify(actual)}，期望 ${JSON.stringify(expected)}）`);
}
function gte(actual, min, label) {
  ok(actual >= min, `${label}（${actual} >= ${min}）`);
}

const REPO_ROOT = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const SERVER_IMAGE = 'agentlink-server:latest';
/** 截图留档目录：人想看一眼渲染结果时不用再跑一遍。 */
const SHOTS_DIR =
  process.env.AGENTLINK_SHOT_DIR ?? path.join(REPO_ROOT, '..', '.runtime', 'qt-shots');
const DESKTOP_IMAGE = 'agentlink-desktop:latest';
const SERVER_GIT_URL =
  // 注意 #refs/heads/main：docker build 从 git URL 拉源码时**默认找 master 分支**，
  // 而仓库默认分支是 main —— 不写 fragment 会报"无法找到远程引用 master"。
  process.env.AGENTLINK_SERVER_GIT_URL ??
  'https://github.com/EzraX21/agentlink.git#refs/heads/main';

/** 跑一个 docker 命令并收集输出。 */
function docker(args, opts = {}) {
  return new Promise((resolve) => {
    const child = spawn('docker', args, {
      env: { ...process.env },
      stdio: ['ignore', 'pipe', 'pipe'],
      ...opts,
    });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (b) => { stdout += b.toString(); });
    child.stderr.on('data', (b) => { stderr += b.toString(); });
    child.on('close', (code) => resolve({ code, stdout, stderr }));
  });
}

/** docker build（可能要十几分钟），失败返回 null。 */
async function dockerBuild(args, label) {
  console.log(`  ${label}（几分钟）…`);
  const res = await dockerBuildInner(args);
  return res;
}
function dockerBuildInner(args) {
  return new Promise((resolve) => {
    const child = spawn('docker', ['build', ...args], {
      env: { ...process.env },
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let out = '';
    child.stdout.on('data', (b) => { out += b.toString(); });
    child.stderr.on('data', (b) => { out += b.toString(); });
    child.on('close', (code) => resolve({ code, out }));
  });
}

async function imageExists(tag) {
  return (await docker(['image', 'inspect', tag])).code === 0;
}

async function freePort() {
  return new Promise((resolve, reject) => {
    const srv = createServer();
    srv.unref();
    srv.on('error', reject);
    srv.listen(0, '127.0.0.1', () => {
      const port = srv.address().port;
      srv.close(() => resolve(port));
    });
  });
}

/** 等一个端口能访问。 */
async function waitFor(url, timeoutMs = 90_000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const res = await fetch(url);
      if (res.ok) return true;
    } catch {
      /* 还没起来 */
    }
    await new Promise((r) => setTimeout(r, 500));
  }
  return false;
}

/**
 * 极简 PNG 统计（只支持我们要的形态：8 位、非隔行、RGB 或 RGBA）。
 *
 * 为什么自己解而不是装 pngjs：验收脚本目前零依赖，装个库就为了数颜色
 * 不划算；而 PNG 的 deflate 解压 zlib 就有，反过滤也就几十行。
 *
 * 返回的 stats 用来断言"这张图不是空白"：
 *   - width/height  尺寸合理
 *   - colors        不同颜色数（有文字就该有抗锯齿产生的大量中间色）
 *   - darkRatio     深色像素占比（文字）
 *   - contentRows   含内容的行数（布局是多行而不是一块纯色）
 */
function pngStats(buf) {
  if (buf.length < 33) throw new Error('not a png');
  const width = buf.readUInt32BE(16);
  const height = buf.readUInt32BE(20);
  const depth = buf[24];
  const colorType = buf[25];
  if (depth !== 8 || (colorType !== 2 && colorType !== 6)) {
    throw new Error(`不支持的 PNG 形态：depth=${depth} colorType=${colorType}`);
  }
  const channels = colorType === 6 ? 4 : 3;

  // 收集 IDAT
  const idat = [];
  let off = 8;
  while (off + 8 <= buf.length) {
    const len = buf.readUInt32BE(off);
    const type = buf.toString('ascii', off + 4, off + 8);
    const dataStart = off + 8;
    if (type === 'IDAT') idat.push(buf.subarray(dataStart, dataStart + len));
    if (type === 'IEND') break;
    off = dataStart + len + 4;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat));

  const stride = width * channels;
  const out = Buffer.alloc(height * stride);
  let pos = 0;
  for (let y = 0; y < height; y += 1) {
    const filter = raw[pos];
    pos += 1;
    const rowStart = y * stride;
    for (let x = 0; x < stride; x += 1) {
      const rawByte = raw[pos + x];
      const a = x >= channels ? out[rowStart + x - channels] : 0;
      const b = y > 0 ? out[rowStart - stride + x] : 0;
      const c = x >= channels && y > 0 ? out[rowStart - stride + x - channels] : 0;
      let v;
      switch (filter) {
        case 0: v = rawByte; break;
        case 1: v = rawByte + a; break;
        case 2: v = rawByte + b; break;
        case 3: v = rawByte + ((a + b) >> 1); break;
        case 4: {
          const p = a + b - c;
          const pa = Math.abs(p - a);
          const pb = Math.abs(p - b);
          const pc = Math.abs(p - c);
          v = rawByte + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
          break;
        }
        default: throw new Error(`未知 PNG 过滤器 ${filter}`);
      }
      out[rowStart + x] = v & 0xff;
    }
    pos += stride;
  }

  const colors = new Set();
  let dark = 0;
  const bgR = out[0];
  const bgG = out[1];
  const bgB = out[2];
  const rows = new Array(height).fill(false);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const i = y * stride + x * channels;
      const r = out[i];
      const g = out[i + 1];
      const bl = out[i + 2];
      colors.add((r << 16) | (g << 8) | bl);
      const lum = 0.299 * r + 0.587 * g + 0.114 * bl;
      if (lum < 100) dark += 1;
      if (Math.abs(r - bgR) + Math.abs(g - bgG) + Math.abs(bl - bgB) > 12) rows[y] = true;
    }
  }
  const total = width * height;
  return {
    width,
    height,
    colors: colors.size,
    darkRatio: dark / total,
    contentRows: rows.filter(Boolean).length,
  };
}

// ---------------------------------------------------------------------------

async function main() {
  console.log('\x1b[1magentlink-desktop 验收\x1b[0m');

  section('0. 环境');
  const probe = await docker(['run', '--rm', 'alpine:3', 'sh', '-c', 'echo ok']);
  eq(probe.code, 0, 'Docker 可用');
  if (probe.code !== 0) {
    console.log('  \x1b[33m- 跳过：没有 Docker\x1b[0m');
    process.exit(1);
  }

  // 客户端镜像：就住在本仓库，缺了自己建
  if (!(await imageExists(DESKTOP_IMAGE))) {
    const built = await dockerBuild(['-t', DESKTOP_IMAGE, '.'], '构建 agentlink-desktop 镜像');
    if (built.code !== 0) {
      ok(false, `构建客户端镜像失败\n${built.out.slice(-1500)}`);
      process.exit(1);
    }
  }
  ok(await imageExists(DESKTOP_IMAGE), `${DESKTOP_IMAGE} 就绪`);

  // 服务端镜像：本地 → 本机检出 → GitHub
  if (!(await imageExists(SERVER_IMAGE))) {
    const dir = process.env.AGENTLINK_SERVER_DIR;
    const srcArgs = dir
      ? [dir]
      : [SERVER_GIT_URL];
    console.log(`  ${dir ? `用本地检出 ${dir}` : `从 ${SERVER_GIT_URL} 拉`}服务端源码…`);
    const built = await dockerBuild(['-t', SERVER_IMAGE, ...srcArgs], '构建 agentlink-server 镜像');
    if (built.code !== 0) {
      ok(false, `构建服务端镜像失败（可设 AGENTLINK_SERVER_DIR 指向本地 agentlink 检出）\n${built.out.slice(-1500)}`);
      process.exit(1);
    }
  }
  ok(await imageExists(SERVER_IMAGE), `${SERVER_IMAGE} 就绪`);

  // ---- 起一个隔离的服务端 ----
  section('1. 起隔离的服务端容器');
  const port = await freePort();
  const name = `agentlink-desktop-server-${port}`;
  // 用 **Docker 具名卷**而不是宿主目录挂载：
  // 宿主目录挂进容器后属主是当前用户，容器里的 `node` 用户写不了 SQLite
  // （SQLITE_CANTOPEN），而容器里的 root 又改不动那个属主。
  // 具名卷由 Docker 自己管属主，没这个问题。
  const volumeName = `agentlink-desktop-vol-${port}`;
  await docker(['volume', 'create', volumeName]);

  const started = await docker([
    'run', '-d', '--name', name,
    '-p', `${port}:3000`,
    '-v', `${volumeName}:/data`,
    '-e', `JWT_SECRET=${'q'.repeat(40)}`,
    '-e', `AGENT_TOKEN_PEPPER=${'p'.repeat(20)}`,
    SERVER_IMAGE,
  ]);
  eq(started.code, 0, '服务端容器启动');

  try {
    const healthy = await waitFor(`http://127.0.0.1:${port}/healthz`);
    ok(healthy, `服务端健康（http://127.0.0.1:${port}）`);
    if (!healthy) throw new Error('服务端没有起来');

    section('2. GUI 二进制能起来（平台插件 / TLS 后端 / 动态库都在位）');
    // 不带显示环境也能验：`--version` 打印完就退出，不进事件循环。
    // 这条能抓到"镜像缺 Qt 平台插件"或"动态库没打进去"这类问题 ——
    // 只跑单测是抓不到的（单测是 GUILESS，不需要平台插件）。
    const version = await docker([
      'run', '--rm', '-e', 'QT_QPA_PLATFORM=offscreen', DESKTOP_IMAGE, '--version',
    ]);
    ok(
      version.code === 0 && version.stdout.includes('agentlink-qt'),
      `GUI 二进制可执行：${version.stdout.trim() || version.stderr.trim().slice(0, 300)}`,
    );
    ok(/Qt \d+\.\d+/.test(version.stdout), `带出 Qt 版本（${/Qt [0-9.]+/.exec(version.stdout)?.[0] ?? '无'}）`);

    section('2b. GUI 真的画得出界面（离屏渲染 + 截图分析）');
    // `--version` 只证明"二进制能起来"。缺字体、样式表加载失败、布局塌成 0×0
    // 这些问题它一个都抓不到 —— 必须真的渲染一遍再检查像素。
    //
    // 用 offscreen 平台插件，所以不需要 Xvfb 或任何显示环境。
    // 直接挂到 SHOTS_DIR：不能用 mkdtemp('/tmp')，docker daemon 看不到
    // 本进程的 /tmp（沙箱里 /tmp 是私有的），容器写成功了宿主机却读不到。
    mkdirSync(SHOTS_DIR, { recursive: true });
    const shotPath = path.join(SHOTS_DIR, 'onboarding.png');
    const shot = await docker([
      'run', '--rm',
      '-e', 'QT_QPA_PLATFORM=offscreen',
      '-v', `${SHOTS_DIR}:/shots`,
      DESKTOP_IMAGE,
      '--screenshot', '/shots/onboarding.png',
    ]);
    ok(
      shot.code === 0 && /screenshot ok/.test(shot.stdout),
      `离屏渲染成功（${shot.stdout.trim().split('\n').pop() ?? shot.stderr.trim().slice(0, 200)}）`,
    );

    let stats = null;
    try {
      stats = pngStats(readFileSync(shotPath));
    } catch (err) {
      ok(false, `截图解析失败：${err.message}`);
    }
    if (stats !== null) {
      // 尺寸：引导对话框定的是 440×280 左右，给它一个下限而不是精确值，
      // 免得以后调窗口大小就要改测试。
      ok(stats.width >= 400 && stats.height >= 250,
        `截图尺寸合理（${stats.width}×${stats.height}）`);
      // 颜色数：纯色背景 + 有文字 → 抗锯齿会产生几十上百种中间色。
      // 少于 20 基本可以断定"只画了个底色，控件没出来"。
      ok(stats.colors >= 20, `不是纯色块（${stats.colors} 种颜色）`);
      // 深色像素 = 文字。完全没有文字说明标签/按钮没渲染。
      ok(stats.darkRatio > 0.002,
        `有文字像素（深色占比 ${(stats.darkRatio * 100).toFixed(2)}%）`);
      // 含内容的行数：多行说明是"若干标签 + 输入框 + 按钮"的布局，
      // 而不是一条横线或者一块色。
      ok(stats.contentRows >= 20,
        `有纵向布局（${stats.contentRows}/${stats.height} 行含内容）`);
      console.log(`    截图留在：${shotPath}`);
    }

    section('3. 协议层单测（不需要服务端）');
    // 注意 --entrypoint sh：镜像的 ENTRYPOINT 是 GUI 可执行文件，
    // 不覆盖的话 "sh -c ..." 会变成 GUI 程序的参数 → 它去开窗口 → 找不到 xcb 插件。
    const tests = await docker([
      'run', '--rm', '--entrypoint', 'sh', DESKTOP_IMAGE,
      '-c', 'cd /build && ctest --output-on-failure -E e2e',
    ]);
    // 断言：三个测试文件都跑了，且没红。
    // CTest 的输出格式是 "100% tests passed, 0 tests failed out of 3"，
    // 不是 "Tests run:" —— 那是 QTest 单测自己的输出，会被 ctest 折叠掉。
    const passLine = /(\d+)% tests passed/.exec(tests.stdout);
    ok(
      tests.code === 0 && passLine !== null,
      `协议层单测通过\n${(tests.stdout + tests.stderr).slice(-1200)}`,
    );
    // ctest 输出里的测试名不带 "test-" 前缀（"Test #1: frames ..."）
    for (const t of ['frames', 'address', 'settings']) {
      ok(new RegExp(`Test #[0-9]+: ${t}\\b`).test(tests.stdout), `跑了 ${t}`);
    }

    section('4. 端到端：真连服务端');
    // 容器里的 127.0.0.1 指向它自己，不是宿主的服务端。
    // 用 --network host：容器直接走宿主网络，127.0.0.1:<port> 就是上面的服务端。
    // 直接跑二进制而不是 ctest：ctest 通过时会折叠 QTest 的用例细节
    // （Totals: N passed 看不到），直接用二进制才能数每个用例。
    const e2eHost = await docker([
      'run', '--rm', '--network', 'host', '--entrypoint', 'sh', DESKTOP_IMAGE,
      '-c',
      `cd /build && AGENTLINK_TEST_URL=http://127.0.0.1:${port} AGENTLINK_TEST_WS_URL=ws://127.0.0.1:${port}/ws ./test-e2e`,
    ]);

    ok(
      e2eHost.code === 0,
      `端到端通过\n${(e2eHost.stdout + e2eHost.stderr).slice(-1500)}`,
    );
    const e2eTotals = /Totals:\s*(\d+) passed/.exec(e2eHost.stdout);
    gte(e2eTotals ? Number(e2eTotals[1]) : 0, 4, `端到端至少 4 个用例（实际 ${e2eTotals ? e2eTotals[1] : '未知'}）`);
  } finally {
    await docker(['rm', '-f', name]);
    await docker(['volume', 'rm', '-f', volumeName]);
  }

  console.log(`\n${failed === 0 ? '\x1b[32m全部通过\x1b[0m' : `\x1b[31m${failed} 项失败\x1b[0m`}`);
  process.exit(failed === 0 ? 0 : 1);
}

await main();
