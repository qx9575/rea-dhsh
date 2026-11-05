# 新水浒Q传原生客户端 M1 实施计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 交付一个不加载原 EXE/保护 DLL、直接读取原资源的 C++ 原生客户端，在应天府完成角色动画、点击寻路、碰撞遮挡、场景动画和最小 SCP/Lua 交互闭环。

**Architecture:** 新代码放在 `client/`，与 Python 逆向工具及 `kimi_viewer` 分离。资源格式通过统一 `ResourceProvider` 访问，CPU 完成原始 2D 合成，Win32/D3D11 负责窗口与显示；复杂格式可暂用 Python 生成的缓存，但接口保持不变。

**Tech Stack:** C++20、CMake、CTest、Win32、D3D11、WIC、XAudio2/Media Foundation、经证据确认版本的 Lua；Python 仅用于参考实现、缓存生成和对拍。

---

## 执行约定

- 在独立 worktree 中执行，例如 `.worktrees/native-client`，不污染当前资源分析工作区。
- 原始目录 `D:\software\dhsh` 始终只读；不运行原游戏或保护组件。
- 所有默认单元测试使用合成字节或测试时生成的小样本；真实 PAK 回归通过 `DHSH_SHDATA=D:/software/dhsh/shdata` 显式启用，大文件不提交 Git。
- 每个任务按 RED → GREEN → 回归 → 提交执行。测试未红不能写实现，测试未绿不能提交。
- 下列构建命令假定机器已有 Visual Studio 2022 和 CMake；执行前先运行 `cmake --version` 和 `where cl`。若生成器不同，只替换生成器，不改变交付范围。
- 通用配置命令：

```bash
cmake -S client -B client/build -G "Visual Studio 17 2022" -A Win32
cmake --build client/build --config Debug
ctest --test-dir client/build -C Debug --output-on-failure
```

预期：构建无错误，已注册测试全部 `PASS`。

## 参考实现与证据

- PAK/NZ：`tools/dhsh/pak.py`、`tools/dhsh/nz.py`、`nz_lzw10.py`、`nz_lzo.py`。
- 图像与动画：`tools/dhsh/formats/a16.py`、`apg.py`、`pif.py`、`apci.py`。
- 地图：`tools/dhsh/formats/map.py`、`docs/formats/map.md`。
- UI：`tools/dhsh/formats/scp.py`、`docs/formats/scp.md`。
- 场景与纸娃娃行为：`tools/kimi_viewer/scene.py`、`avatar.py` 及对应 README。
- 总体约束：`docs/plans/2026-11-06-native-client-design.md`。

### Task 1: 建立原生工程、测试骨架与窗口烟测

**Files:**
- Create: `client/CMakeLists.txt`
- Create: `client/src/CMakeLists.txt`
- Create: `client/tests/CMakeLists.txt`
- Create: `client/tests/test_smoke.cpp`
- Create: `client/src/main.cpp`
- Create: `client/src/app/App.h`
- Create: `client/src/app/App.cpp`

**Step 1: 写失败的构建烟测**

在 `test_smoke.cpp` 定义最小断言，不引入测试框架：

```cpp
#include "app/App.h"
int main() {
    dhsh::App app;
    return app.logical_width() == 800 && app.logical_height() == 600 ? 0 : 1;
}
```

`client/CMakeLists.txt` 要求 C++20、启用 CTest，并添加 `src` 与 `tests`；`tests/CMakeLists.txt` 建立 `dhsh_test_smoke` 并注册 `smoke`。

**Step 2: 验证 RED**

Run: `cmake -S client -B client/build -G "Visual Studio 17 2022" -A Win32`
Expected: 配置或构建失败，因为 `app/App.h` 与目标尚不存在。

**Step 3: 写最小应用壳**

`App` 仅暴露逻辑尺寸和 `int run()`；`main.cpp` 调用 `dhsh::App{}.run()`。Win32 窗口在本任务只创建、显示并处理 `WM_DESTROY`，暂不加载资源。

**Step 4: 验证 GREEN 与真实窗口**

Run: `cmake --build client/build --config Debug && ctest --test-dir client/build -C Debug -R smoke --output-on-failure`
Expected: `smoke` PASS；随后运行 `client/build/Debug/dhsh_client.exe` 能打开并关闭空窗口。

**Step 5: 提交**

```bash
git add client
 git commit -m "feat(client): add native application skeleton"
```

### Task 2: 防御性二进制读取器与 PAKF 索引

**Files:**
- Create: `client/src/resource/ByteReader.h`
- Create: `client/src/resource/ByteReader.cpp`
- Create: `client/src/resource/PakArchive.h`
- Create: `client/src/resource/PakArchive.cpp`
- Create: `client/tests/test_pak.cpp`

**Step 1: 写失败测试**

测试合成一个 16 字节 `PAKF3001` 头和一个 48 字节条目，断言叶子路径与绝对偏移；另测错误 magic、条目数大于 200000、截断记录、超过 8 层嵌套和 `..` 路径全部抛出 `FormatError`。

核心接口：

```cpp
struct PakEntry {
    std::string name;
    std::string path;
    std::uint32_t offset;
    std::uint32_t size;
    std::uint32_t kind;
    std::uint32_t depth;
};
class PakArchive {
public:
    explicit PakArchive(std::filesystem::path file);
    const std::vector<PakEntry>& entries() const;
    std::vector<std::byte> read(const PakEntry& entry) const;
};
```

**Step 2: 验证 RED**

Run: `cmake --build client/build --config Debug --target dhsh_test_pak`
Expected: 编译失败，PAK 类型不存在。

**Step 3: 移植解析器**

严格复用 `tools/dhsh/pak.py` 的规则：子归档偏移相对当前 `base`，输出叶子使用绝对文件偏移，支持 `size=0` 别名，递归上限 8，路径分隔统一为 `/`。任何乘法或加法先检查溢出和文件边界。

**Step 4: 验证 GREEN 和真实 PAK**

Run: `ctest --test-dir client/build -C Debug -R pak --output-on-failure`
Expected: 全部 PASS。真实 PAK 烟测读取 `config.pak`，条目数与 `pak_inventory.json` 对应 PAK 计数一致。

**Step 5: 提交**

```bash
git add client/src/resource client/tests/test_pak.cpp client/tests/CMakeLists.txt
 git commit -m "feat(client): parse PAKF archives safely"
```

### Task 3: 路径解析与统一资源提供接口

**Files:**
- Create: `client/src/resource/ResourceProvider.h`
- Create: `client/src/resource/PakResourceProvider.h`
- Create: `client/src/resource/PakResourceProvider.cpp`
- Create: `client/src/resource/ResourceError.h`
- Create: `client/tests/test_resource_provider.cpp`

**Step 1: 写失败测试**

用临时目录和合成 PAK 验证：`map\YingTianFu\YingTianFu.map` 与 `MAP/YingTianFu/YingTianFu.map` 可命中同一条目；缺失文件返回带逻辑路径的错误，不产生空成功结果。

定义：

```cpp
class ResourceProvider {
public:
    virtual ~ResourceProvider() = default;
    virtual std::vector<std::byte> bytes(std::string_view logical_path) = 0;
    virtual bool exists(std::string_view logical_path) const = 0;
};
```

**Step 2: 验证 RED**

Run: `cmake --build client/build --config Debug --target dhsh_test_resource_provider`
Expected: 编译失败，接口未实现。

**Step 3: 实现索引与只读访问**

`PakResourceProvider` 扫描指定 `shdata` 下所需 PAK，建立小写规范化路径到 `(archive, entry)` 的映射。重复路径记录所有来源并采用当前查看器一致的优先级，不静默覆盖。文件流只以只读方式打开。

**Step 4: 验证 GREEN**

Run: `ctest --test-dir client/build -C Debug -R resource_provider --output-on-failure`
Expected: 大小写、分隔符、缺失与重复路径测试全部 PASS。

**Step 5: 提交**

```bash
git add client/src/resource client/tests/test_resource_provider.cpp client/tests/CMakeLists.txt
 git commit -m "feat(client): add logical resource lookup"
```

### Task 4: NZ 0/0A/0B 解码与 Python 对拍

**Files:**
- Create: `client/src/resource/NzDecoder.h`
- Create: `client/src/resource/NzDecoder.cpp`
- Create: `client/tests/test_nz.cpp`
- Create: `tools/client_fixture_export.py`

**Step 1: 写失败测试与参考样本**

测试 NZ type 0 的精确剥离、错误 `size1/size2`、LZW 结束码与字典回退、LZO 重叠拷贝和截断输入。`client_fixture_export.py` 只按显式 `--pak`、`--path`、`--limit` 导出小型参考样本和 SHA-256，不允许无过滤全量导出。

**Step 2: 验证 RED**

Run: `cmake --build client/build --config Debug --target dhsh_test_nz`
Expected: 编译失败，解码器不存在。

**Step 3: 移植三种解码**

保持 `tools/dhsh/nz.py`、`nz_lzw10.py`、`nz_lzo.py` 的码宽、字典和边界语义。输出长度必须严格等于 `size1`，任何额外输出或输入越界均为 `FormatError`。

**Step 4: 验证 GREEN 与真实样本一致**

Run: `ctest --test-dir client/build -C Debug -R nz --output-on-failure`
Expected: 单元测试 PASS；对每种类型至少 20 个真实样本，C++ 输出与 Python 逐字节一致，失败立即停止扩展样本。

**Step 5: 提交**

```bash
git add client/src/resource client/tests/test_nz.cpp client/tests/CMakeLists.txt tools/client_fixture_export.py
 git commit -m "feat(client): decode NZ resource payloads"
```

### Task 5: 图像模型、A16/CTI 与 WIC 标准图像解码

**Files:**
- Create: `client/src/render/Image.h`
- Create: `client/src/formats/A16.h`
- Create: `client/src/formats/A16.cpp`
- Create: `client/src/formats/Cti.h`
- Create: `client/src/formats/Cti.cpp`
- Create: `client/src/platform/WicImage.h`
- Create: `client/src/platform/WicImage.cpp`
- Create: `client/tests/test_a16.cpp`

**Step 1: 写失败测试**

用合成 RGB565、RLE16 和 RLE8 alpha 样本断言 RGBA 字节；覆盖 compression 0/1/2、alpha 0/2、AI 调色板索引、短输出与尺寸溢出。`Image` 固定保存宽度、高度、紧密排列 RGBA8 字节。

**Step 2: 验证 RED**

Run: `cmake --build client/build --config Debug --target dhsh_test_a16`
Expected: 编译失败，图像与解码类型不存在。

**Step 3: 实现最小解码**

逐函数对照 `a16.py` 与 `apg.py` 的 RGB565、RLE 和 alpha 规则。WIC 仅用于标准 JPG/PNG/BMP/TGA 中其明确支持的格式；不支持的格式保留明确错误，不能返回黑图。

**Step 4: 验证 GREEN 与像素对拍**

Run: `ctest --test-dir client/build -C Debug -R a16 --output-on-failure`
Expected: 全部 PASS；至少 40 个真实 A16/CTI 样本与 Python RGBA 输出逐字节一致。

**Step 5: 提交**

```bash
git add client/src/render client/src/formats client/src/platform client/tests/test_a16.cpp client/tests/CMakeLists.txt
 git commit -m "feat(client): decode Hero2D images"
```

### Task 6: APG/PIF 动画数据模型

**Files:**
- Create: `client/src/formats/Apg.h`
- Create: `client/src/formats/Apg.cpp`
- Create: `client/src/formats/Pif.h`
- Create: `client/src/formats/Pif.cpp`
- Create: `client/src/animation/AnimationClip.h`
- Create: `client/src/animation/AnimationClip.cpp`
- Create: `client/tests/test_animation.cpp`

**Step 1: 写失败测试**

合成两帧 APG 与 PIF，断言帧边界、AG delta 遍历、AI 必须有 CTI、帧 x/y 锚点和 `f5` 延时；延时 0 必须归一化为 80ms，24 字节 PIF 行必须归一化为 28 字节布局。

**Step 2: 验证 RED**

Run: `cmake --build client/build --config Debug --target dhsh_test_animation`
Expected: 编译失败，APG/PIF 类型不存在。

**Step 3: 实现帧迭代与时间推进**

`AnimationClip` 保存 `Image`、锚点和每帧持续时间；`advance(milliseconds)` 可跨多帧，但不自行选择方向或业务动作。compression 5 的 JPEG 走 WIC，AI 走 CTI。

**Step 4: 验证 GREEN 与真实动画**

Run: `ctest --test-dir client/build -C Debug -R animation --output-on-failure`
Expected: 全部 PASS；至少一套角色、NPC 和技能 APG 的帧数、RGBA、锚点和延时与 Python 一致。

**Step 5: 提交**

```bash
git add client/src/formats client/src/animation client/tests/test_animation.cpp client/tests/CMakeLists.txt
 git commit -m "feat(client): add APG and PIF animation playback"
```

### Task 7: MAP/CEL/MAN/MLK/core 地图解析

**Files:**
- Create: `client/src/formats/MapFormats.h`
- Create: `client/src/formats/MapFormats.cpp`
- Create: `client/tests/test_map_formats.cpp`

**Step 1: 写失败测试**

按 `map.py` 构造最小文件，分别断言：

- MAP section/key 与 GB18030 文本；
- CEL MSB-first、1=阻挡、第一张为地面层、越界为阻挡；
- MAN 100 字节记录及 x/y 左上坐标；
- MLK 出生格与朝向字段；
- core 记录、24 字节放置项、行内 RLE 和 `sortY = y + baseY - ((x * slope) >> 8)`。

**Step 2: 验证 RED**

Run: `cmake --build client/build --config Debug --target dhsh_test_map_formats`
Expected: 编译失败，地图格式类型不存在。

**Step 3: 实现解析与边界检查**

完整保留 MAN 字段，但 M1 只消费已确认字段；core 的 RLE 不跨扫描行，每行长度必须等于宽度。CEL 地图尺寸不等于文件尺寸时实现已验证的左上复制、裁剪和边界补齐，不使用辅助层作为地面碰撞。

**Step 4: 验证 GREEN 与应天府数字对拍**

Run: `ctest --test-dir client/build -C Debug -R map_formats --output-on-failure`
Expected: 全部 PASS；应天府 MAN/core/CEL 计数和已知格值与 `docs/PROGRESS.md` 及现有 manifest 一致。

**Step 5: 提交**

```bash
git add client/src/formats client/tests/test_map_formats.cpp client/tests/CMakeLists.txt
 git commit -m "feat(client): parse Hero2D map formats"
```

### Task 8: 应天府静态场景与背景缓存

**Files:**
- Create: `client/src/scene/MapScene.h`
- Create: `client/src/scene/MapScene.cpp`
- Create: `client/src/render/SoftwareRenderer.h`
- Create: `client/src/render/SoftwareRenderer.cpp`
- Create: `client/src/resource/CachedImageProvider.h`
- Create: `client/src/resource/CachedImageProvider.cpp`
- Create: `tools/client_map_cache.py`
- Create: `client/tests/test_scene.cpp`

**Step 1: 写失败测试**

用小型背景与 alpha 精灵验证视口裁剪、源像素混合和越界安全。场景加载测试要求背景、MAP 或地面 CEL 缺失时返回致命错误，单个 MAN 对象缺失只记录并跳过。

**Step 2: 验证 RED**

Run: `cmake --build client/build --config Debug --target dhsh_test_scene`
Expected: 编译失败，场景和渲染器不存在。

**Step 3: 实现静态加载与缓存**

`MapScene::load("YingTianFu")` 读取地图逻辑尺寸、背景、MAN 和 core。`client_map_cache.py` 调用现有 `special_jpg` 解码器，只导出选定地图背景 PNG，并写含来源大小、mtime、SHA-256 的 manifest；C++ 用 WIC 读取。缓存路径不含版本号或 SHA。

**Step 4: 验证 GREEN 与静态截图**

Run: `ctest --test-dir client/build -C Debug -R scene --output-on-failure`
Expected: 全部 PASS；应天府静态视口与 `map_preview.py` 同视口对拍，差异仅允许来自已记录的特殊 JPG 误差。

**Step 5: 提交**

```bash
git add client/src/scene client/src/render client/src/resource client/tests/test_scene.cpp client/tests/CMakeLists.txt tools/client_map_cache.py
 git commit -m "feat(client): render static YingTianFu scene"
```

### Task 9: D3D11 显示、主循环与输入坐标

**Files:**
- Create: `client/src/platform/D3D11Presenter.h`
- Create: `client/src/platform/D3D11Presenter.cpp`
- Create: `client/src/platform/Win32Window.h`
- Create: `client/src/platform/Win32Window.cpp`
- Modify: `client/src/app/App.h`
- Modify: `client/src/app/App.cpp`
- Create: `client/tests/test_viewport.cpp`

**Step 1: 写失败测试**

测试 800×600 逻辑画面在不同窗口尺寸下的等比缩放、黑边和屏幕坐标到地图坐标逆变换；覆盖点击黑边不产生地图操作。

**Step 2: 验证 RED**

Run: `cmake --build client/build --config Debug --target dhsh_test_viewport`
Expected: 编译失败，视口换算不存在。

**Step 3: 实现显示与固定逻辑更新**

使用 `QueryPerformanceCounter` 计算帧间隔，动画和移动接受实际毫秒，渲染每帧显示当前状态。D3D11 创建交换链和 RGBA 纹理，仅显示 CPU framebuffer。窗口消息转成逻辑坐标，不直接写游戏状态。

**Step 4: 验证 GREEN 与真实场景窗口**

Run: `ctest --test-dir client/build -C Debug -R viewport --output-on-failure`
Expected: 全部 PASS；`dhsh_client.exe --shdata D:/software/dhsh/shdata --map YingTianFu` 显示可滚动/缩放的应天府，无 Python 进程依赖。

**Step 5: 提交**

```bash
git add client/src client/tests/test_viewport.cpp client/tests/CMakeLists.txt
 git commit -m "feat(client): present native scene with D3D11"
```

### Task 10: MAN 场景动画、排序与 core 遮挡

**Files:**
- Create: `client/src/scene/SceneObject.h`
- Create: `client/src/scene/SceneObject.cpp`
- Create: `client/src/scene/CoverMask.h`
- Create: `client/src/scene/CoverMask.cpp`
- Modify: `client/src/scene/MapScene.cpp`
- Create: `client/tests/test_scene_layers.cpp`

**Step 1: 写失败测试**

验证 MAN `sortY = y + f48 + ((f58 * arg) >> 8)` 的使用、同深度稳定顺序、PIF 帧延时和 core 深度条件 `y < ((slope*x)>>8)+sortY`。遮挡只暗化被覆盖角色像素，不重画背景，也不参与点击。

**Step 2: 验证 RED**

Run: `cmake --build client/build --config Debug --target dhsh_test_scene_layers`
Expected: 编译失败，场景对象与遮罩不存在。

**Step 3: 实现动画对象与 ghost 暗化**

按现有证据实现对象排序和 RLE 遮罩测试。暗化算法逐项对照当前 `ApplyMask` 证据；静态 `flag=1` 在 M1 启用，帮派 `group/frame` 状态留接口但不伪造网络状态。

**Step 4: 验证 GREEN 与视觉对拍**

Run: `ctest --test-dir client/build -C Debug -R scene_layers --output-on-failure`
Expected: 全部 PASS；应天府灯笼等 MAN 对象位置、排序和遮挡与查看器/原截图一致。

**Step 5: 提交**

```bash
git add client/src/scene client/tests/test_scene_layers.cpp client/tests/CMakeLists.txt
 git commit -m "feat(client): animate and occlude scene objects"
```

### Task 11: APCI 查询与纸娃娃角色

**Files:**
- Create: `client/src/formats/Apci.h`
- Create: `client/src/formats/Apci.cpp`
- Create: `client/src/avatar/AvatarCatalog.h`
- Create: `client/src/avatar/AvatarCatalog.cpp`
- Create: `client/src/avatar/Avatar.h`
- Create: `client/src/avatar/Avatar.cpp`
- Create: `client/tests/test_avatar.cpp`

**Step 1: 写失败测试**

测试 APCI 20 组边界、8 字节 entry、动作 ID 取 `mid.header` 低 16 位、blob 长度 16 位截断和文件恰好消费；纸娃娃测试按 1B 层叠键升序、稳定同键顺序、方向缺失回退和部件缺失错误。

**Step 2: 验证 RED**

Run: `cmake --build client/build --config Debug --target dhsh_test_avatar`
Expected: 编译失败，APCI/Avatar 不存在。

**Step 3: 实现查询和装配**

移植 `apci.py` 与 `kimi_viewer/avatar.py` 的已确认规则。M1 只加载一个显式预设，不移植名称覆盖清单或职业筛选 UI；预设只包含真实存在的 body/head 等必要部件。

**Step 4: 验证 GREEN 与角色对拍**

Run: `ctest --test-dir client/build -C Debug -R avatar --output-on-failure`
Expected: 全部 PASS；预设角色至少 8 个方向的待机/行走帧、锚点、延时和层叠与查看器一致。

**Step 5: 提交**

```bash
git add client/src/formats client/src/avatar client/tests/test_avatar.cpp client/tests/CMakeLists.txt
 git commit -m "feat(client): assemble native paper-doll avatar"
```

### Task 12: NPC、角色状态机与场景实体

**Files:**
- Create: `client/src/scene/Entity.h`
- Create: `client/src/scene/Entity.cpp`
- Create: `client/src/scene/Character.h`
- Create: `client/src/scene/Character.cpp`
- Modify: `client/src/scene/MapScene.h`
- Modify: `client/src/scene/MapScene.cpp`
- Create: `client/tests/test_entities.cpp`

**Step 1: 写失败测试**

验证 Idle/Walk 状态转换、八方向向量到动作方向的映射、位置不变时不错误切换行走、NPC 只播放本地动画，以及 MLK 出生格中心与 `extra0` 朝向映射。

**Step 2: 验证 RED**

Run: `cmake --build client/build --config Debug --target dhsh_test_entities`
Expected: 编译失败，实体和角色类型不存在。

**Step 3: 实现本地实体**

`Character` 只持有位置、方向、状态和 AnimationClip，不包含网络或战斗。NPC 来自已确认的应天府本地配置/资源；若 NPC 放置数据尚未闭合，使用显式本地 M1 预设并在文档标注，不猜造原文件语义。

**Step 4: 验证 GREEN**

Run: `ctest --test-dir client/build -C Debug -R entities --output-on-failure`
Expected: 全部 PASS；角色出生在合法可走格，NPC/角色动画方向和锚点正确。

**Step 5: 提交**

```bash
git add client/src/scene client/tests/test_entities.cpp client/tests/CMakeLists.txt
 git commit -m "feat(client): add local characters and NPCs"
```

### Task 13: CEL A*、点击移动与相机跟随

**Files:**
- Create: `client/src/navigation/NavGrid.h`
- Create: `client/src/navigation/NavGrid.cpp`
- Create: `client/src/navigation/PathFinder.h`
- Create: `client/src/navigation/PathFinder.cpp`
- Create: `client/src/input/MapInput.h`
- Create: `client/src/input/MapInput.cpp`
- Create: `client/tests/test_navigation.cpp`

**Step 1: 写失败测试**

覆盖直线可达、绕障、不可达目标、起终点越界、阻挡格、斜向穿墙和重复目标。路径代价与邻居顺序固定，保证结果可重现；不可达返回空路径和明确状态，不瞬移。

**Step 2: 验证 RED**

Run: `cmake --build client/build --config Debug --target dhsh_test_navigation`
Expected: 编译失败，导航类型不存在。

**Step 3: 实现八方向 A* 与连续移动**

对角移动要求相邻正交格均可走。角色按像素位置沿格中心路径移动，速度来自显式 M1 配置并在对拍后调整，不宣称是原客户端已确认常数。相机跟随角色并夹在地图边界。

**Step 4: 验证 GREEN 与真实操作**

Run: `ctest --test-dir client/build -C Debug -R navigation --output-on-failure`
Expected: 全部 PASS；在应天府真实点击可绕行建筑、不进入阻挡格，点击 UI 或黑边不触发移动。

**Step 5: 提交**

```bash
git add client/src/navigation client/src/input client/src/scene client/tests/test_navigation.cpp client/tests/CMakeLists.txt
 git commit -m "feat(client): add click-to-move navigation"
```

### Task 14: 地图音乐与音频错误隔离

**Files:**
- Create: `client/src/audio/AudioPlayer.h`
- Create: `client/src/audio/AudioPlayer.cpp`
- Modify: `client/src/scene/MapScene.cpp`
- Create: `client/tests/test_audio_selection.cpp`

**Step 1: 写失败测试**

测试 MAP `MUSIC=<id>` 只解析到 `sound/<id>.mp3`；缺失、非法 ID 和解码失败返回可诊断错误，不阻止场景运行，不访问网络。

**Step 2: 验证 RED**

Run: `cmake --build client/build --config Debug --target dhsh_test_audio_selection`
Expected: 编译失败，音频选择逻辑不存在。

**Step 3: 实现 Windows 音频后端**

用 Media Foundation 解码、XAudio2 播放，支持循环和停止。将后端与音乐选择分离，单元测试不依赖声卡；初始化失败时静音继续。

**Step 4: 验证 GREEN 与真实音乐**

Run: `ctest --test-dir client/build -C Debug -R audio_selection --output-on-failure`
Expected: 全部 PASS；应天府播放其配置音乐，退出或重载场景时无残留播放。

**Step 5: 提交**

```bash
git add client/src/audio client/src/scene client/tests/test_audio_selection.cpp client/tests/CMakeLists.txt
 git commit -m "feat(client): play map music locally"
```

### Task 15: SCP 最小控件树与消息分发

**Files:**
- Create: `client/src/script/ScpParser.h`
- Create: `client/src/script/ScpParser.cpp`
- Create: `client/src/ui/Widget.h`
- Create: `client/src/ui/Widget.cpp`
- Create: `client/src/ui/ScpWindow.h`
- Create: `client/src/ui/ScpWindow.cpp`
- Create: `client/src/ui/MessageBus.h`
- Create: `client/src/ui/MessageBus.cpp`
- Create: `client/tests/test_scp_ui.cpp`

**Step 1: 写失败测试**

使用包含 WIN/BUTTON/STATIC/EDIT、RECT/BMP/TEXT、include 和 `MESSAGE FSM_BTN_MSG 1 CODE "On..."` 的样本，断言树结构、坐标、资源加载和按钮消息。未知控件/API 必须返回带文件与控件名的 Unsupported 错误。

**Step 2: 验证 RED**

Run: `cmake --build client/build --config Debug --target dhsh_test_scp_ui`
Expected: 编译失败，SCP/UI 类型不存在。

**Step 3: 实现 M1 必需子集**

文本按 `scp.py` 规则处理 GB18030/UTF-8、注释、include 和 `#ifdef WINWIDTH800`。只实现主界面实际命中的控件；其余控件保留节点并报告未实现。消息总线按控件名和消息类型注册回调，不在这里伪造 Lua。

**Step 4: 验证 GREEN 与主界面截图**

Run: `ctest --test-dir client/build -C Debug -R scp_ui --output-on-failure`
Expected: 全部 PASS；主界面所需 SCP 控件显示在正确位置，地图点击与 UI 点击不互相穿透。

**Step 5: 提交**

```bash
git add client/src/script client/src/ui client/tests/test_scp_ui.cpp client/tests/CMakeLists.txt
 git commit -m "feat(client): render SCP widgets and dispatch messages"
```

### Task 16: 确认 Lua 兼容版本并打通原回调

**Files:**
- Create: `docs/formats/lua-runtime.md`
- Create: `client/src/script/LuaVm.h`
- Create: `client/src/script/LuaVm.cpp`
- Create: `client/src/script/FsLuaHost.h`
- Create: `client/src/script/FsLuaHost.cpp`
- Modify: `client/CMakeLists.txt`
- Modify: `client/src/CMakeLists.txt`
- Create: `client/tests/test_lua_host.cpp`

**Step 1: 先写证据结论与失败测试**

从现有 Lua 源、SCP `#script` 和 Game.exe 静态字符串确认 VM 版本与所需 API，写入 `docs/formats/lua-runtime.md`，明确 Observed/Inferred/Unknown。测试加载一个原样回调片段，注册按钮消息并触发，断言调用计数和窗口可见性变化；Lua 语法错误必须带脚本名和行号。

**Step 2: 验证 RED**

Run: `cmake --build client/build --config Debug --target dhsh_test_lua_host`
Expected: 配置或测试失败，因为 Lua 依赖和宿主尚未接入。若版本证据不足，先补静态分析，禁止猜测版本。

**Step 3: 接入确认版本的 Lua 与最小 FSLua**

通过显式 `DHSH_LUA_ROOT` 或 CMake `find_package` 定位已确认版本的 Lua，不在配置阶段隐式下载。只实现回调所需 `FSLuaWndScript.RegMsgCall`、窗口状态接口和必要模块加载；其他 API 调用抛出带名称的未实现错误。

**Step 4: 验证 GREEN 与真实 SCP 回调**

Run: `ctest --test-dir client/build -C Debug -R lua_host --output-on-failure`
Expected: 全部 PASS；至少一个原 SCP 按钮完成“消息 → 原 Lua 函数 → 窗口状态变化”，日志无未处理 Lua 错误。

**Step 5: 提交**

```bash
git add client docs/formats/lua-runtime.md
 git commit -m "feat(client): run original Lua UI callbacks"
```

### Task 17: M1 集成、诊断与可重复启动

**Files:**
- Modify: `client/src/app/App.cpp`
- Create: `client/src/diagnostics/Log.h`
- Create: `client/src/diagnostics/Log.cpp`
- Create: `client/src/diagnostics/StartupReport.h`
- Create: `client/src/diagnostics/StartupReport.cpp`
- Create: `client/tests/test_startup_report.cpp`
- Create: `client/README.md`

**Step 1: 写失败测试**

测试启动报告包含 shdata 路径、地图、资源成功/缓存/跳过/失败计数和致命错误；普通资源失败不崩溃，核心资源失败返回非零退出码。日志不得包含账号、密码或网络凭据。

**Step 2: 验证 RED**

Run: `cmake --build client/build --config Debug --target dhsh_test_startup_report`
Expected: 编译失败，报告类型不存在。

**Step 3: 完成 CLI 与诊断**

支持 `--shdata <dir>`、`--map YingTianFu`、`--cache <dir>`、`--log <file>`；默认地图固定为 M1 应天府。README 写清构建、缓存生成、运行、测试、只读边界和当前未实现功能。

**Step 4: 验证 GREEN 与冷启动**

Run: `ctest --test-dir client/build -C Debug --output-on-failure`
Expected: 全部 PASS。删除新缓存后按 README 重建并启动，原生 exe 不依赖 Python 进程；第二次启动使用有效缓存，原 PAK 无写入。

**Step 5: 提交**

```bash
git add client
 git commit -m "feat(client): integrate offline M1 startup"
```

### Task 18: 全量 M1 验收与性能记录

**Files:**
- Create: `client/tests/test_m1_acceptance.cpp`
- Create: `docs/VERIFY-NATIVE-CLIENT-M1.md`
- Modify: `CLAUDE.md`
- Modify: `docs/PROGRESS.md`

**Step 1: 写失败验收驱动**

验收驱动使用真实 `DHSH_SHDATA`，依次验证场景加载、出生点、点击可达/不可达目标、八方向动画、NPC/MAN 播放、core 遮挡、SCP/Lua 回调和退出清理。缺少真实资源时明确 `SKIP`，不得把它算作通过。

**Step 2: 验证 RED 并记录差距**

Run: `cmake --build client/build --config Release && ctest --test-dir client/build -C Release -R m1_acceptance --output-on-failure`
Expected: 未完成的端到端行为 FAIL；逐项修复，不降低设计验收条件。

**Step 3: 完成最终验证**

Run: `ctest --test-dir client/build -C Release --output-on-failure`
Expected: 所有非 SKIP 测试 PASS，真实 PAK 验收未 SKIP。人工运行 Release 客户端完成同一流程，记录帧耗时、启动耗时、缓存命中和截图对拍结果。

**Step 4: 更新文档并检查交付**

`VERIFY-NATIVE-CLIENT-M1.md` 记录命令、实际输出、未决项和证据；`CLAUDE.md` 增加 `client/` 定位与构建入口；`docs/PROGRESS.md` 增加 M1 真实状态。运行 `git diff --check`，确认没有写入 `D:\software\dhsh`，也没有提交缓存或原资源。

**Step 5: 提交**

```bash
git add client docs/VERIFY-NATIVE-CLIENT-M1.md docs/PROGRESS.md CLAUDE.md
 git commit -m "test(client): verify offline exploration M1"
```

## 完成定义

只有以下全部成立，M1 才能完成：

1. Release 构建和完整测试通过，真实 PAK 验收没有 SKIP。
2. 用户从原生 exe 进入应天府，可点击寻路并看到正确动画、碰撞、排序和遮挡。
3. 至少一个原 SCP/Lua 回调真实执行。
4. 不加载原 EXE/保护 DLL，不连接服务器，不修改原资源目录。
5. 构建、运行、缓存、测试和已知限制均已写入文档。
