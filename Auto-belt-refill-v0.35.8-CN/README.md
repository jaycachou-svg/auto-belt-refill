# Auto Belt Refill（腰带自动补货）

一个 [D2RLoader](https://d2rloader.net) 插件，用于《暗黑破坏神2：重制版》：
自动用背包中的药水保持腰带满员。

两种模式：

- **手动补满**——按下配置的补货键（或在控制台运行 `belt-fill`），
  按各列记忆把腰带所有空位从背包补满。
- **自动补给**——插件持续监视腰带，药水一被喝掉立即自动补上。
  同时响应状态变化：进游戏时有空位、以及直接捡进背包的药水。

## 工作原理（玩家视角）

- 腰带每一**列**有一条记忆：一个按补给优先级排序的物品代码列表。
  每列只补自己列表里的药水。
- 记忆来自配置文件，或在游戏中**自动学习**：把药水放进某列热键行
  （腰带最下面一行，按键 1-4 对应行）、按下快照键，或第一次补货时自动记录。
  移除（喝掉、拖出、丢弃、卖出）永远不会改变记忆。
- 完全空掉的列保留原记忆并保持空置，直到玩家重新锚定——插件绝不会
  自作主张把别的药水塞进空列（手动补货同样遵守）。
- 补货走游戏自己的槽位检索逻辑（与 shift-click 放药同一条代码路径），
  槽位路由与原版行为完全一致。

## 环境要求

- D2RLoader（基于 PluginSDK 的登录器）。
- **游戏版本 3.2.0 (92777)。已实测：3.2.0 (92777) 和 3.3.0 (93854) 两个版本**——
  两个版本的游戏代码逐字节相同，同一个插件文件都能用。
  插件通过硬编码 RVA 挂接游戏内部函数
  （CanPutInBelt、GetFreeBeltSlot、BeltTransfer、Pickup）；游戏版本不同
  会在启动校验时失败，插件会安全拒绝加载。其他版本需要重新定位地址。

## 安装

本包已按登录器目录结构排好——把 `plugins\` 和 `config\` 两个文件夹
拖进你的 `d2rloader\` 文件夹（或把两个文件复制到相同位置）：

1. `plugins\d2rl-auto-belt-refill.dll` -> `d2rloader\plugins\`（已编译好，随包附带）。
2. `config\auto-belt-refill.toml` -> `d2rloader\config\`（可选：没有配置文件时，插件首次启动会自动生成默认配置）。
3. 重启游戏。登录器日志出现 `AutoBeltRefill vX.Y.Z loaded.` 即成功。

**没有默认快捷键**。想要按键：在 toml 里设置 `fill_key` / `snapshot_key`
（mod 玩家：选其他 mod 没占用的键），或直接在游戏选项 > 控制设置 > Auto Belt Refill** 分类里绑定（那里会显示为"无"的条目）；
也可以什么都不绑，用控制台命令。游戏内绑定由游戏自身保存：重启后保留，并且始终覆盖 toml 里的默认值。

## 配置

见 `config\auto-belt-refill.toml`——每个键都有注释。速查：

| 键 | 含义 |
| --- | --- |
| `enabled` | 插件总开关。 |
| `fill_key` / `snapshot_key` | 快捷键；`""` = 不绑定。 |
| `auto_refill` | 腰带自动补给开关。 |
| `auto_poll_frames` | 轮询间隔（帧，2~360，默认 15）。 |
| `prefer_large` | 补货/学习时优先大号药水。 |
| `debug_logs` | 诊断日志开关——平时保持 `false`。 |
| `[column1]`..`[column4]` | 各列 `consumables` 列表 + `locked` 锁定。 |

药水代码：`hp5..hp1`、`mp5..mp1`、`rvl`、`rvs`；另有 `vps`、`yps`、
`wms`、`tsc`、`isc`。mod 自定义消耗品直接填代码即可。

## 控制台命令

用 `` Ctrl + ` `` 打开控制台：

| 命令 | 作用 |
| --- | --- |
| `belt-fill` | 按各列记忆补满腰带所有空位。 |
| `belt-place` | 诊断：把一件背包物品移入腰带。 |
| `belt-policy` | 只读：各列记忆 + 当前腰带内容。 |
| `belt-config` | 只读：显示解析后的配置。 |
| `belt-snapshot` | 按各列当前热键行重新记录记忆。 |
| `belt-reset` | 清空已学习的记忆；下次补货重新记录。 |
| `belt-scan` | 只读：输出腰带 + 背包布局。 |
| `belt-watch` | 只读：距上次调用为止的挂钩统计。 |
| `belt-verify` | 只读：校验挂接的 RVA 是否匹配当前游戏版本。 |

## 从源码构建

- Visual Studio 2022+（MSVC、C++20）、CMake >= 3.28，以及 D2RLoader 的
  **PluginSDK**（用 `AUTOBELTREFILL_SDK_DIR` 指向其目录）。

```bat
cmake -S . -B build -D AUTOBELTREFILL_SDK_DIR="D:/path/to/PluginSDK"
cmake --build build --config Release
```

## 注意事项与已知限制

- 游戏按**家族**把药水路由到最左边的匹配列。锁定的列若在同家族列的
  左边会拦截路由；被它拒绝的药水会被游戏丢弃，到不了右边的列。
  需要让小号药水到达右侧列时，请保持锁定列有货，或解除锁定。
- 记忆按会话保存：进新游戏时各列重新学习（toml 配置的列表会重新载入）。
