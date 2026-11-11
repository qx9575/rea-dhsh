# dhsh_client 原生离线 M1

`dhsh_client` 是 Windows 原生离线客户端。当前里程碑只加载应天府（`YingTianFu`）的已确认本地出生点，显示地图、MAN 对象、本地角色、一个可选 NPC 和两个 SCP 界面；它不启动或加载原版 EXE/保护 DLL，也不连接游戏服务。

## 构建

需要 Visual Studio 2022（Win32）、CMake 3.26 或更新版本。构建目录为 `client/build`；以下命令从本仓库根目录执行。

```bash
DHSH_SHDATA='D:/software/dhsh/shdata' \
DHSH_MAP_CACHE="$PWD/client/build/map-cache" \
cmake -S client -B client/build \
  -G 'Visual Studio 17 2022' -A Win32 \
  -DPython3_EXECUTABLE='D:/software/anaconda3/python.exe'
cmake --build client/build --config Debug
cmake --build client/build --config Release
```

配置时的 `DHSH_SHDATA` 和 `DHSH_MAP_CACHE` 用于注册真实资源测试。Python 只用于缓存生成和测试校验；原生 EXE 的运行不依赖 Python 进程。

## 生成应天府背景缓存

原生客户端直接读取 PAK 中的地图逻辑、角色和界面资源，但 M1 的私有地图背景先由已验证的 `tools/client_map_cache.py` 解码为 PNG：

```bash
'D:/software/anaconda3/python.exe' tools/client_map_cache.py \
  --shdata 'D:/software/dhsh/shdata' \
  --map YingTianFu \
  --cache client/build/map-cache --png-level 1
```

输出为 `map/YingTianFu/YingTianFu_10Year.png` 和 `client_map_cache.json`。重复运行会校验来源和输出哈希，有效缓存不会重解码。客户端默认读取配置构建目录下的 `map-cache`，也可用 `--cache` 指定其他专用缓存目录。

## 运行与诊断

```bash
client/build/Debug/dhsh_client.exe \
  --shdata 'D:/software/dhsh/shdata' \
  --map YingTianFu \
  --cache client/build/map-cache \
  --log client/build/startup.log
```

参数：

- `--shdata <dir>`：必填，旧版只读 `shdata` 目录。
- `--map YingTianFu`：可选，默认且当前只支持 `YingTianFu`（M1 应天府）。
- `--cache <dir>`：可选，覆盖默认背景缓存目录。
- `--log <file>`：可选，写入启动报告及验收模式的 SCP 事件；父目录不存在时会创建。每行写入后立即刷新，文件由 `Log` 的作用域自动关闭。打开、建目录或写入失败会产生明确错误；核心启动或日志写入失败返回非零退出码。
- `--capture <ppm>`：可选，保留现有首帧 PPM 截图行为。
- `--acceptance`：可选诊断模式，从专用 `config.pak` 挂载加载真实 `ui/login/bulletin2.scp`，避免 `800config.pak` 同名空壳遮蔽；点击公告页签会执行原 Lua 回调并记录窗口状态，F8 隐藏/重开公告。正常模式不加载公告。
- `--help`：显示参数说明。

启动报告包含 `shdata`、地图、资源 `success/cache/skipped/failure` 计数、`fatal_error` 和 `exit_code`。计数来自实际启动结果：成功挂载的 PAK、场景及其 MAN 对象、角色、NPC、音频工作线程创建和 SCP；`cache` 记录从专用 PNG 缓存加载的地图背景；`skipped` 记录跳过的 MAN 对象及可选 NPC；`failure` 记录已隔离的普通 NPC、音频创建或 SCP 失败。普通资源失败不会终止客户端，核心 PAK、地图/出生点或角色资源失败会写入致命诊断并返回 1。

日志不记录账号、密码或网络凭据；诊断中这类命名字段的值会替换为 `<redacted>`。音频后端仍由其专用工作线程拥有。

## 操作

- 左键点击地图：设置寻路目标；左键拖动不再移动相机。
- 右键按住并拖动：移动相机，角色继续沿原路径移动；松开后保持当前视角。
- 下一次有效左键寻路：平滑恢复相机跟随。
- 鼠标滚轮：缩放地图；方向键或 WASD：键盘平移相机。

## 打包

在仓库根目录运行：

```bash
'D:/software/anaconda3/python.exe' tools/package_client.py
```

默认读取 `client/build/Release/dhsh_client.exe` 和 `client/build/map-cache`，生成 `client/dist/dhsh-client/` 与同名 ZIP。包内包含 EXE、README、`run_client.cmd` 和地图缓存，不复制原游戏 PAK。可用 `--client`、`--cache`、`--output-dir`、`--zip` 覆盖路径，或用 `--no-zip` 只生成目录。输出已存在时默认报错，不覆盖旧文件。

解压后运行 `run_client.cmd "D:\path\to\shdata"`；参数必须是原客户端的只读 `shdata` 目录。

## 测试

```bash
cmake --build client/build --config Debug --target dhsh_test_startup_report
ctest --test-dir client/build -C Debug --output-on-failure
```

使用配置命令中的两个环境变量可注册完整真实资源测试。`m1_acceptance` 是真实 PAK 子系统验收，缺资源时明确显示 SKIP；`m1_app_acceptance` 启动真实客户端，在 `--acceptance` 模式完成 Lua、寻路、阻挡、非连通目标、相机和退出验收：

```bash
ctest --test-dir client/build -C Release -R m1_ --output-on-failure
```

最终签收必须确认这两个测试均为 Passed，不能把 SKIP 当作真实验收通过。窗口烟测可单独运行：

```bash
'D:/software/anaconda3/python.exe' tools/client_window_smoke.py \
  --client "$PWD/client/build/Debug/dhsh_client.exe" \
  --shdata 'D:/software/dhsh/shdata' \
  --cache "$PWD/client/build/map-cache" \
  --sample-seconds 25
```

烟测脚本只启动并关闭上述原生客户端。不要对正在运行的新版游戏或任何其他进程使用关闭操作。

## 只读边界与当前限制

- `D:/software/dhsh` 是只读旧版基线；客户端和缓存工具不得修改其中的 PAK、MAP 或其他资源。所有缓存、日志和截图只能写入本构建树中的新专用路径。
- 不读取或修改 `D:/software/dhsh2`，不关闭其进程，不加载原 EXE/保护 DLL，不联网，不修改原资源。
- 当前没有登录、账号、角色选择、网络协议、完整游戏流程或完整 SCP 功能；只有 M1 本地场景和已实现的有限交互。
- 当前只确认应天府本地出生点和一个本地 NPC 预设；其他地图、出生点和 NPC 不会自动替代。
- 私有地图背景需要预生成缓存；这并非完整资源的通用解码或导出管线。
