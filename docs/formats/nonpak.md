# 非 PAK 资源（sound / font / animation / update.skin）

- 日期：2026-10-23
- 目标：`D:\software\dhsh\shdata` 下不经过 `*.pak` 的资源（只读）
- 工具：`tools/scan_nonpak.py`
- 清单：`manifests/batches/nonpak-index.json`、`nonpak-index.csv`

## 1. 总览

| 类别 | 文件数 | 大小 | 组成 |
|---|---:|---:|---|
| `sound/` | 456 | 126.96 MiB | 435 WAV + 21 MP3 |
| `font/` | 6 | 34.97 MiB | 4 TTF + 1 TTC + `fontinfo.ini` |
| `animation/` | 7 | 24.61 MiB | 3 APG + 3 PIF(`.dat`) + 1 MP3 |
| `update/skin/` | 41 | 2.94 MiB | 32 BMP + 8 `BMP.rNNNN` + 1 JPG |
| **合计** | **510** | **189.5 MiB** | 0 解析错误 |

所有文件记录 `size` 与 SHA-256；内容重复的 14 个文件带
`duplicate_of`（13 组重复音效、`STXINGKA.TTF` 与 `ARKai_T.ttf` 相同）。
`save/`（61 个本地用户配置，约 0.15 MiB）是可变的运行期数据，不纳入
资源索引。

## 2. sound/

- WAV：371 个单声道 44.1 kHz/16-bit、38 个立体声 44.1 kHz、
  14 个立体声 48 kHz、11 个立体声 96 kHz、1 个 IMA ADPCM
  （22.05 kHz/4-bit）。
- MP3：MPEG1 Layer III，20 个 44.1 kHz + 1 个 48 kHz；按帧头累计时长
  1.5–3.7 分钟，码率 128/160/192 kbps 或 VBR。
- 文件名是数字 ID（`18xxx.wav`、`190xx.mp3`），可直接与 MAP INI 的
  `MUSIC=`、SCP/技能配置里的音效 ID 交叉引用（~~暂未做消费端绑定~~
  **2026-10-26 已闭合**，见 §2.1）。

### 2.1 消费端绑定（2026-10-26 闭合）

- **地图音乐**：MAP INI `[ext] MUSIC=<id>` → 加载器 `FUN_00420790`
  解析 `MUSIC` 键（串 `0x9808a4`）→ `FUN_005cd9b0` 拼
  `sound/%d.mp3` → FMOD 播放。全部 292 张图共用 16 个 MUSIC id
  （19000–19020），**16/16 均有对应文件**。
- **音效表驱动**：`config/UI/sound.csv`（138 条 UI/系统音效 ID→路径→
  说明）、`monstersound.csv`（103 条：音效 ID + 怪物 ID → 攻击/受击/
  死亡 `A-/H-/D-<id>.wav`）、`simsound.csv`/`simmonstersound.csv`
  （简化版：全部指向 `18000.wav`/`A-18500.wav` 占位）。磁盘文件名用
  下划线（`A_18570.wav`），CSV 用连字符（`A-18500.wav`），一一对应。
  4 张表共 **762 个路径引用，751 个命中文件（98.6%）**；缺 11 个：
  `18433.wav`/`18436.wav`（表情音效，真缺）、`19012.mp3`（音乐，真缺）、
  怪物 `18552`/`18578` 的 A/H/D 全套（6 个，真缺）、`simsound.csv` 里
  2 处把 `.mp3` 误写成 `.mp4`（游戏数据本身的笔误）。
- **UI 音效代码佐证**：UI 声音对象构造 `FUN_008d24f0` 内置默认
  `sound/18004.wav`（按钮确认）；音频引擎为 FMOD（`fmodex.dll`，
  6 处 `fmod` 串引用）。
- 怪物表第二列（`25003` 等）为怪物 ID，与 npc/monster 配置交叉。

## 3. font/

| 文件 | 面数 | family / full name | 字形数 |
|---|---:|---|---:|
| `simsun.ttc` | 2 | SimSun / NSimSun | 22,141 |
| `SIMYOU.TTF` | 1 | YouYuan | 21,991 |
| `STFANGSO.TTF` | 1 | STFangsong | 25,185 |
| `STXINGKA.TTF` | 1 | STXingkai | 8,159 |
| `ARKai_T.ttf` | 1 | STXingkai | 8,159 |
| `fontinfo.ini` | — | 界面字体槽位（NORMAL/LARGE/MIDDLE/…） | — |

`fontinfo.ini` 为 GBK INI，记录各字体槽位的 `PATH`/`NAME`/`CHATSET`
（当前多指向 `simsun.ttc`，`MIDDLE2` 指 `STXINGKA.TTF`）。
`STXINGKA.TTF` 与 `ARKai_T.ttf` 字节完全相同。

## 4. animation/

| 前缀 | APG 帧数 | PIF width | PIF 其它 |
|---|---:|---:|---|
| `1` | 44 | 44 | `count=3, height=28, flags=2` |
| `2` | 26 | 26 | 同上 |
| `3` | 20 | 20 | 同上 |

APG 帧数与 PIF `width` 一致，`.dat` 即 PIF 表；`1.mp3` 为 31.9 s
VBR 音频。PIF 帧延时/GIF 合成已在 `docs/formats/pif.md` 闭合。

## 5. update/skin/

- 32 个标准 BMP + 1 个 JPG；另有 8 个 `*.bmp.r1571` / `*.bmp.r1911`，
  **扩展名只是版本后缀，内容仍是 BMP**（`BM` 头，索引按图像解析）。
- 按钮资源多为 32-bit BMP（70×22、121×35 等）；`bk.bmp`/`bk_mask.bmp`
  为 600×600/24-bit。
- `.r1571` 与 `.r1911` 两份内容不同，是同一按钮的不同版本，不是重复。

## 6. 复跑

```bash
python tools/scan_nonpak.py --dry-run --limit 10
python tools/scan_nonpak.py \
    --out manifests/batches/nonpak-index.json \
    --csv manifests/batches/nonpak-index.csv
```

JSON 的 `categories.<name>.items[]` 保存逐文件元数据；CSV 是扁平表
（`category,path,size,sha256,kind,width,height,bpp,codec,channels,
sample_rate,bits,bitrate_kbps,duration_s,frames,font_family,duplicate_of`）。
