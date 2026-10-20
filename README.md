# 武汉科技大学 校园网自动认证工具（wust-auth）

一个用 **C99** 写的命令行小工具：自动检测网络 → 没联网就用你的学号密码完成校园网认证 → 再复检一遍确认能上网。
程序**不包含任何人的学号密码**，账号信息只保存在你自己电脑上的 `wust_config.ini` 里，可以放心打包分发给同学。

- 纯 C99 + 系统自带的 socket，**零第三方依赖**（不需要 libcurl、不需要额外 DLL），拷贝即用
- 编译要求：`gcc -std=c99 -Wall -Wextra -pedantic`，**零警告**
- 支持 `-a` 静默启动、`-q` 后台静默（无窗口 + 写日志），可以配置成开机自启
- Windows 额外提供 `wust-authw.exe`（GUI 子系统），开机自启**完全不弹窗**

> 📄 **只想发给同学看？** 用 [使用指南.md](使用指南.md)（一页纸，不含技术细节）。
> 本 README 是完整文档，包含编译、原理和排错。

**目录**

1. [文件说明](#1-文件说明)
2. [编译方法](#2-编译方法)
3. [怎么用](#3-怎么用)（首次运行 / 菜单 / 参数 / 配置 / 改密码 / 排错 / 静默与日志）
4. [配置开机自启](#4-配置开机自启)（Windows / Linux）
5. [分发流程](#5-分发流程给同学的一步步说明)
6. [工作原理](#6-工作原理技术细节)
7. [安全提醒](#7-安全提醒)
8. [开发者自测](#8-开发者自测可选)

---

## 1. 文件说明

| 文件 | 作用 | 是否需要分发给同学 |
| --- | --- | --- |
| `main.c` | 全部源代码 | 只给想自己编译的同学 |
| `Makefile` | 编译脚本 | 同上 |
| `wust-auth.exe`（Windows）/ `wust-auth`（Linux） | 编译好的程序（控制台版，手动使用） | **是**，直接发这个 |
| `wust-authw.exe` | 开机自启专用：Windows GUI 子系统版，**永不弹窗**、默认静默自动登录 | Windows 用户建议一起发 |
| `wust-auth.log` | 静默运行时自动生成的日志（排查用） | 否（运行时自动生成） |
| `README.md` | 本说明（完整文档：编译、原理、排错） | 可发，但同学更该看下一行 |
| `使用指南.md` | **一页纸的简明使用说明**，可直接发给同学 | **是**（推荐和 exe 一起发） |
| `wust_config.ini` | 你自己学号密码的配置 | **绝对不要发！** 里面有你的密码 |
| `mock_server.py` | 开发者自测用的假认证服务器 | 否（仅开发调试用） |

> ⚠️ 分发前请务必删掉自己电脑上的 `wust_config.ini`，否则等于把自己的密码发给别人。
> `wust-auth.log` 里只有登录过程记录，不含密码，但也建议清掉再发。

**运行环境说明**：这两个 exe 依赖 Windows 自带的 UCRT（`api-ms-win-crt-*.dll`）
和 `WS2_32.dll`、`USER32.dll`、`KERNEL32.dll`。Windows 10 / 11 全部自带；
Windows 7 / 8 若提示缺少 `api-ms-win-crt-runtime-l1-1-0.dll`，装一下系统的
「通用 C 运行库」更新（KB2999226）即可。不需要 MinGW、不需要额外 DLL。

---

## 2. 编译方法

### Windows（MinGW-w64）

1. 安装 [MinGW-w64](https://www.mingw-w64.org/downloads/)（或者用 MSYS2 装 `mingw-w64-x86_64-gcc`），确保 `gcc` 在 PATH 里。
2. 在本目录打开 PowerShell / CMD：

```bat
:: 方式一：用 Makefile（MinGW 自带的 make 叫 mingw32-make）—— 一次生成两个 exe
mingw32-make

:: 方式二：直接手敲命令
gcc -std=c99 -Wall -Wextra -pedantic -O2 -o wust-auth.exe main.c -lws2_32

:: 开机自启专用的 GUI 版（不弹控制台窗口，永久静默）
gcc -std=c99 -Wall -Wextra -pedantic -O2 -DWUST_FORCE_QUIET -mwindows -o wust-authw.exe main.c -lws2_32
```

产物：

- `wust-auth.exe` —— 控制台版，手动使用（录入、改密码、登录）
- `wust-authw.exe` —— **GUI 子系统版**，开机自启用；不带参数就是静默自动登录

Windows 下必须链接 `-lws2_32`，Makefile 已按平台自动加上。

### Linux

```bash
make                       # 或者：gcc -std=c99 -Wall -Wextra -pedantic -O2 -o wust-auth main.c
make check                 # 只做严格告警检查（-fsyntax-only），确认零警告
make clean                 # 清理
```

Linux 上没有 `-mwindows`，只生成 `wust-auth`（开机自启用 `-a` 或 `-q`，systemd 本来就没有窗口）。
编译完成后 `./wust-auth` 即可运行。Linux 上配置文件权限会自动设为 `600`（只有你自己能读）。

---

## 3. 怎么用

### 3.1 首次运行：录入学号和密码

直接双击 `wust-auth.exe`（或在终端里 `./wust-auth`）：

```
==================================================
   武汉科技大学 校园网自动认证工具  v1.2
==================================================

未检测到可用的账号配置，请先录入你的学号和密码。
（账号只保存在本机的 wust_config.ini 中，程序不会上传到任何第三方）
请输入学号：2023xxxxxxxx
请输入密码：********          ← 输入时不会显示，正常现象
配置已保存到：C:\wust-auth\wust_config.ini

---------------- 主菜单 ----------------
  1. 自动检测网络并登录（普通登录）
  2. 修改账号和密码
  3. 退出程序
----------------------------------------
请选择 [1-3]：
```

没有配置文件时程序会**自动跳到录入流程**，录完以后再显示菜单。

> 密码输入时不回显，输完直接回车。密码里带 `&`、`%`、`=`、空格等特殊字符也没问题。

### 3.2 菜单

| 选项 | 说明 |
| --- | --- |
| `1` 自动检测网络并登录 | 先探测外网；已在线就直接告诉你，没在线才发认证请求；认证成功后再复检一次 |
| `2` 修改账号和密码 | 只输要改的那一项，另一项直接回车保持不变 |
| `3` 退出程序 | |

一次完整的成功登录长这样：

```
[1/3] 正在检测网络是否已经连通 ...
      当前未联网，开始认证。

[2/3] 正在向认证服务器提交登录请求（学号 2023xxxxxxxx）...
      服务器返回：code=0，authMsg=已登录成功
      登录成功！

[3/3] 正在复检网络连通性 ...
      网络已连通，可以正常上网。
```

### 3.3 命令行参数

```
wust-auth.exe [选项]

  -a, --auto      静默模式：直接读配置完成“检测并登录”，不显示菜单
  -q, --quiet     后台静默：隐藏窗口 + 把输出写进 wust-auth.log，不弹窗、不等输入
  -v, --verbose   额外打印探测结果和服务器原始响应，排查问题用
  -p, --where     只做配置诊断：显示实际使用的配置文件路径和读取结果，然后退出
  -t, --tries N   最多尝试认证几次（默认 3，-q 时 12）
  -d, --delay N   重试间隔秒数（默认 5，-q 时 10）
  -h, --help      显示帮助
```

参数写法很宽容：`-a`、`--auto`、`-auto`、`/a`、`'-a'`、`"-a"` 都能识别
（任务计划里手误加引号是很常见的情况，程序会自动去掉两端成对的引号）。

例：

```bat
wust-auth.exe -a            :: 后台静默认证一次（会显示窗口）
wust-auth.exe -q            :: 后台静默认证一次，不显示窗口，结果写日志
wust-auth.exe -a -v         :: 静默认证，并打印服务器原始返回
wust-auth.exe -p            :: 只做配置诊断：读的哪个文件、为什么读不到
```

**退出码**（脚本判断用）：`0`=已在线或认证成功 ｜ `1`=认证失败 ｜ `2`=配置缺失/不完整 ｜ `3`=参数错误。

> **重试策略**：只有“连不上认证服务器／探测失败”这类网络原因才会重试；账号密码错误不重试。
> 默认最多 3 次、间隔 5 秒；`-q` 静默模式为了照顾开机场景，默认放宽到 12 次、间隔 10 秒
> （可用 `-t` / `-d` 或在 `-q` 之前显式指定来覆盖）。
> 数值参数支持 `-t 5`、`-t=5`、`--tries=5` 三种写法。

### 3.4 配置文件

**查找顺序**（先找到先用）：

1. 环境变量 `WUST_CONFIG` 指定的路径
2. **当前目录**下已存在的 `wust_config.ini`
3. **程序所在目录**下已存在的 `wust_config.ini`
4. **每个用户自己的配置目录**下已存在的 `wust_config.ini`
   - Windows：`%APPDATA%\wust-auth\wust_config.ini`（一般是 `C:\Users\你的用户名\AppData\Roaming\wust-auth\`）
   - Linux：`$XDG_CONFIG_HOME/wust-auth/wust_config.ini` 或 `~/.config/wust-auth/wust_config.ini`

**如果四处都没有**（第一次运行），程序会自动挑一个**能写进去**的位置：

- 程序所在目录可写 → 就写在那儿（最直观，推荐把程序放在 `D:\wust-auth\` 这种普通目录）；
- 程序所在目录**不可写** → 自动改存到上面第 4 条的用户配置目录，并**自动创建该目录**。

> 这一点很重要：如果把 exe 放在 `C:\` 根目录、`C:\Program Files\` 或只读盘里，
> 普通用户是写不进去的。程序不会反复报错，而是自动把配置存到 `%APPDATA%\wust-auth\`，
> 下次运行也能自动找回来。（早期版本会一直提示“保存配置失败”，现在不会了。）

内容格式（程序自动生成，一般不用手改）：

```ini
# 武汉科技大学校园网自动认证工具 —— 个人配置
# 这里保存着你的学号和密码，请不要把这个文件发给别人。
username=2023xxxxxxxx
password=你的密码
```

- 编码：UTF-8（带不带 BOM 都行）、UTF-16 LE/BE（带 BOM，即记事本里的“Unicode”）都能自动识别
- 行尾 `\r\n` / `\n` 都行；键名也接受 `user=` / `pass=` 这两种简写
- 值两边的空白会被忽略；**密码首尾的空格不会被保留**
- 唯一不支持的是“无 BOM 的 UTF-16”，遇到时程序会明确提示你另存为 UTF-8

### 3.5 修改密码

运行程序 → 选 `2` → 提示「直接回车表示该项保持不变」：

```
—— 修改账号和密码 ——
当前学号：2023xxxxxxxx
提示：直接回车表示该项保持不变。
请输入新学号：            ← 直接回车，学号不变
请输入密码：********      ← 输入新密码
配置已更新并保存到：C:\wust-auth\wust_config.ini
```

也可以直接删掉 `wust_config.ini` 再运行程序，会重新走一遍首次录入流程。

### 3.6 提示「读不到配置文件」怎么办

先跑一次诊断，它会告诉你**程序到底在用哪个文件、以及为什么读不到**：

```bat
wust-auth.exe -p
```

```
=== 配置诊断 ===
当前工作目录  ：D:\wust-auth
程序所在目录  ：D:\wust-auth（可写：是）
用户配置目录  ：C:\Users\你\AppData\Roaming\wust-auth（尚未创建，首次保存时会自动创建）
使用的配置文件：D:\wust-auth\wust_config.ini
读取结果      ：失败 —— 文件不存在
提示：第一次使用请不带参数运行本程序，按提示录入学号和密码。
```

对照「读取结果」里的原因处理：

| 读取结果 | 原因 | 怎么办 |
| --- | --- | --- |
| `文件不存在` | 还没录过账号（第一次运行），或配置被放到了别的目录 | 直接**不带参数**运行一次 `wust-auth.exe`，按提示录入学号密码 |
| `文件是空的` | 建了空文件 / 编辑时内容被清空 | 不带参数运行一次重新录入，或选菜单 `2` |
| `缺少 username=（学号）一行…` | 少写了一行，或等号后面是空的 | 补上，或选菜单 `2` 重新录入 |
| `缺少 password=（密码）一行…` | 同上 | 同上 |
| `文件里既没有 username= 也没有 password=…` | 键名写错了（例如写成了 `user_name=`、`pwd=`、`账号=`） | 只能是 `username=` 和 `password=`（也接受 `user=` / `pass=`） |
| `文件内容里含 0 字节，像是“无 BOM 的 UTF-16”…` | 用别的编辑器存成了 UTF-16 | 用记事本打开 → 另存为 → 编码选 **UTF-8** |
| `无法打开（错误码 5…）` | 没有读取权限 / 文件被占用 / 路径其实是目录 | 换个目录放配置，或用 `WUST_CONFIG` 指到你有权限的位置 |
| `正常，学号 = xxx` | 配置没问题 | 那问题不在配置文件，用 `-a -v` 看网络和认证过程 |

诊断输出里还会列出**程序所在目录**和**用户配置目录**各自是否可写，例如把程序放在 `C:\` 根目录时：

```
=== 配置诊断 ===
当前工作目录  ：C:\
程序所在目录  ：C:\（可写：否）
用户配置目录  ：C:\Users\你\AppData\Roaming\wust-auth（可写：是）
使用的配置文件：C:\Users\你\AppData\Roaming\wust-auth\wust_config.ini
读取结果      ：正常，学号 = 2026xxxxxxxx
```
看到「程序所在目录 可写：否」就是这种情况——程序已经把配置自动存到用户配置目录了，
不必做任何处理；只是别忘了**配置在那个目录里**，换电脑/重装系统后要重新录入。

想让它固定存在程序旁边，就把 exe 挪到可写目录（如 `D:\wust-auth\`）后重跑一次。

如果「使用的配置文件」不是你以为的那个路径，说明出现了**目录不一致**（很常见）：

- 程序的查找顺序是：`WUST_CONFIG` 环境变量 → **当前目录**下已有的 `wust_config.ini` → **exe 所在目录**下已有的 `wust_config.ini` → **用户配置目录**下已有的 `wust_config.ini`（见 3.4 节的四步）。
- 所以把 exe 换目录、或者从别的目录启动程序，读到的配置就可能不是你上次录的那个。
- 想固定下来，就设一个环境变量（写进「系统属性 → 环境变量」即可长期生效）：

```bat
setx WUST_CONFIG "D:\wust-auth\wust_config.ini"
```

```bash
export WUST_CONFIG="$HOME/.config/wust-auth.ini"
```

- 排查目录问题时，用 `-p` 输出的「当前工作目录」和「使用的配置文件」两行对照一下就清楚了。

### 3.7 后台静默运行与日志

用 `-q`（或直接用 `wust-authw.exe`）时，程序**不显示任何窗口**，所有过程信息写进日志文件：

- 默认位置：**exe 所在目录**下的 `wust-auth.log`（exe 目录不可写时自动写到 `%APPDATA%\wust-auth\`）。
  注意它跟的是 **exe 的位置**，不是配置文件的位置——所以即使用 `WUST_CONFIG` 把配置放到别处，
  日志仍在 exe 旁边。
- 也可以用 `WUST_LOG` 环境变量指定，例如 `set WUST_LOG=D:\wust-auth\run.log`
- **追加写**，每次运行都会留一段带时间戳的记录，方便回看“今天开机到底认证了没有”

```bat
type D:\wust-auth\wust-auth.log
```

里面会写清楚：程序路径、用的哪个配置、探测判定结果、服务器返回的 code、最终退出码。
日志里**不会**出现密码。

---

## 4. 配置开机自启

### 4.1 Windows（任务计划程序）

#### 先弄清楚两个 exe

| 文件 | 子系统 | 会不会弹黑窗口 | 建议用途 |
| --- | --- | --- | --- |
| `wust-auth.exe` | 控制台 | 会（可用 `-q` 隐藏） | 手动用：双击/命令行录入、改密码、登录 |
| **`wust-authw.exe`** | **Windows GUI** | **永远不会**（PE 头里就没有控制台） | **开机自启专用**：自动读取配置、静默登录、写日志 |

`wust-authw.exe` 不带参数时**默认就是静默自动登录**，不显示任何窗口、不等待输入，
运行结果写进 `wust-auth.log`（和 exe 同目录；不可写则写进 `%APPDATA%\wust-auth\`）。

> 只想要一个 exe 也可以：`wust-auth.exe -q` 同样隐藏窗口 + 写日志。
> 区别是控制台程序被计划任务拉起时可能闪一下黑框，GUI 版的 `wust-authw.exe` 连闪都不会闪。

#### 为什么“任务计划里显示在运行，但没有登录”

最常见的两个原因，都能从日志和“上次运行结果”看出来：

1. **参数被写成了带引号的 `'-a'`**
   Windows 命令行里**单引号不是引号字符**，会原样传给程序。
   任务计划里写成 `'-a'` 时，程序收到的是 5 个字符 `'-a'`，认不出来 → 打印帮助并以**退出码 3**结束
   （在任务计划的「上次运行结果」里就是 `3`），于是“任务跑过了，但什么都没做”。
   → **v1.2 起程序会自动去掉两端成对的 `'` 或 `"`，`'-a'`、`/a`、`--auto` 都能正确识别。**
   正确的写法就是 `-a`，**不要加任何引号**。
2. **干脆没填参数**
   不带参数运行会进入交互菜单，等你在窗口里输入。被计划任务拉起时你没有窗口可点，
   它就一直挂在“正在运行”状态。→ 自启用 `wust-authw.exe`（不用参数就是静默自动），
   或者给 `wust-auth.exe` 加上 `-q`。

#### 推荐做法（零弹窗 + 可查日志）

1. 把 `wust-auth.exe`、`wust-authw.exe` 放到固定目录，例如 `D:\wust-auth\`（**别放 `C:\` 根目录或 Program Files**，那里普通用户不可写）。
2. 先手动运行 `wust-auth.exe`，录入学号密码，确认菜单 `1` 能登录成功。
3. 管理员 CMD 执行：

```bat
:: 登录时自动认证（零窗口），30 秒延迟等网络就绪
schtasks /create /tn "WUST校园网自动认证" /tr "D:\wust-auth\wust-authw.exe" /sc onlogon /delay 0000:30 /f

:: 断网重连保活：每 10 分钟检查一次（已在线会立刻退出，几乎无消耗）
schtasks /create /tn "WUST校园网自动认证-保活" /tr "D:\wust-auth\wust-authw.exe" /sc minute /mo 10 /f
```

4. 立刻测试：

```bat
schtasks /run /tn "WUST校园网自动认证"
```

5. 看结果（这是关键，静默运行不会有任何窗口提示）：

```bat
type D:\wust-auth\wust-auth.log
```

正常应该是：

```
控制台：无（GUI 子系统，从未创建）

===== 2026-10-19 18:17:46 | wust-auth v1.2 | 静默启动（开机自启） =====
程序目录：D:\wust-auth
配置文件：D:\wust-auth\wust_config.ini

[1/3] 正在检测网络是否已经连通 ...
      当前未联网，开始认证。
...
      登录成功！
[3/3] 正在复检网络连通性 ...
      网络已连通，可以正常上网。
===== 2026-10-19 18:17:49 | 结束：已在线或登录成功（退出码 0）=====
```

补充命令：

```bat
schtasks /query /tn "WUST校园网自动认证" /v /fo list    :: 看上次运行结果
schtasks /delete /tn "WUST校园网自动认证" /f            :: 删掉旧任务重建
```

#### 图形界面做法

1. `Win + R` → `taskschd.msc` → 右侧「创建任务」（**不要**用“创建基本任务”）。
2. **常规**：名称 `WUST校园网自动认证`；选「只在用户登录时运行」（不需要存密码）。
3. **触发器** → 新建 → 「开始任务」选 **登录时** → 「延迟任务时间」填 `30 秒`。
   想断网重连就再加一个触发器：「按预定计划」→ 重复间隔 `10 分钟`、持续时间 `无限期`。
4. **操作** → 新建 → 程序或脚本填 **`D:\wust-auth\wust-authw.exe`**，
   「添加参数」**留空**（GUI 版默认静默自动），「起始于」留空。
5. **条件**：取消勾选「只有在计算机使用交流电源时才启动此任务」。
6. **设置**：勾上「如果任务失败，按以下频率重新启动：1 分钟」。
7. 保存 → 右键任务「运行」→ 然后 `type D:\wust-auth\wust-auth.log` 确认。

> 旧版任务里如果已经把参数写成了 `'-a'`，**不必改**：v1.2 会自动兼容。
> 但建议换成 `wust-authw.exe` 以获得真正的零窗口。


### 4.2 Linux（systemd）

> Linux 下 systemd 本来就没有窗口，用 `-a` 即可，日志进 journal：
> `journalctl -u wust-auth.service -n 50`。
> 如果想统一写进文件，把 `ExecStart` 改成 `/usr/local/bin/wust-auth -q`，
> 并把 `Environment=WUST_LOG=/var/log/wust-auth.log` 一起加上。

把程序装到系统路径并测试：

```bash
gcc -std=c99 -Wall -Wextra -pedantic -O2 -o wust-auth main.c
sudo install -m 755 wust-auth /usr/local/bin/wust-auth
/usr/local/bin/wust-auth          # 先交互式录入一次学号密码
```

> 配置会落在哪：以普通用户身份运行且 `/usr/local/bin` 不可写时，自动落到
> `~/.config/wust-auth/wust_config.ini`；用 `sudo` 运行时则会写进 `/usr/local/bin/`。
> **建议显式指定**，避免和程序目录权限纠缠：
> `WUST_CONFIG=$HOME/.config/wust-auth.ini wust-auth` 录入一次，
> 之后在 service 里也设置同一个 `WUST_CONFIG`。

**方案 A：用户级 service（推荐给个人电脑）**

`~/.config/systemd/user/wust-auth.service`：

```ini
[Unit]
Description=WUST campus network auto login
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
Environment=WUST_CONFIG=%h/.config/wust-auth.ini
ExecStart=/usr/local/bin/wust-auth -a
Restart=on-failure
RestartSec=30

[Install]
WantedBy=default.target
```

```bash
systemctl --user daemon-reload
systemctl --user enable --now wust-auth.service
systemctl --user status wust-auth.service          # 看结果
loginctl enable-linger $USER                       # 没登录时也能跑
journalctl --user -u wust-auth.service -n 50       # 看日志（printf 输出会进 journal）
```

**方案 B：系统级 service（公用机器 / 一直开机）**

`/etc/systemd/system/wust-auth.service`：

```ini
[Unit]
Description=WUST campus network auto login
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
Environment=WUST_CONFIG=/etc/wust-auth.ini
ExecStart=/usr/local/bin/wust-auth -a
Restart=on-failure
RestartSec=30

[Install]
WantedBy=multi-user.target
```

```bash
sudo install -m 600 /dev/null /etc/wust-auth.ini       # 先建好空文件
sudo WUST_CONFIG=/etc/wust-auth.ini /usr/local/bin/wust-auth   # 交互式录入（root 身份）
sudo systemctl daemon-reload
sudo systemctl enable --now wust-auth.service
sudo systemctl status wust-auth.service
journalctl -u wust-auth.service -n 50
```

**方案 C：断网重连最灵敏的做法 —— NetworkManager 事件钩子**

`/etc/NetworkManager/dispatcher.d/90-wust-auth`（`chmod 755`，属主 root）：

```sh
#!/bin/sh
# $1=接口名  $2=动作
case "$2" in
  up|dhcp4-change|connectivity-change)
    WUST_CONFIG=/etc/wust-auth.ini /usr/local/bin/wust-auth -a >/dev/null 2>&1 &
    ;;
esac
```

```bash
sudo chmod 755 /etc/NetworkManager/dispatcher.d/90-wust-auth
```

**方案 D：最简单的定时保活（cron）**

```bash
crontab -e
# 每 5 分钟检查一次，没在线就自动认证
*/5 * * * * WUST_CONFIG=$HOME/.config/wust-auth.ini /usr/local/bin/wust-auth -a >/dev/null 2>&1
```

> Linux 上建议用登录用户自己的身份运行（不要把别人/自己的密码文件放到公用目录），
> `chmod 600` 保证只有属主能读。

---

## 5. 分发流程（给同学的一步步说明）

1. **你自己**：编译出 `wust-auth.exe` 和 `wust-authw.exe`，先在自己电脑上删掉
   `wust_config.ini` 和 `wust-auth.log`，或者干脆从干净目录打包。
2. 打包这些文件：`wust-auth.exe` + `wust-authw.exe` + `使用指南.md`
   （可选 `wust_config.ini` 的**空模板**，但绝不要放你的真实配置）。
3. 发给同学，让同学：
   - 解压到一个**固定目录**（例如 `D:\wust-auth\`，别放桌面又随手删）；
   - 双击 `wust-auth.exe`，按提示输入**自己的**学号和密码（密码不回显，输完回车）；
   - 看到菜单后选 `1`，确认显示「网络已连通，可以正常上网」；
   - 想改密码随时选 `2`；
   - 想开机自启就按 4.1 节配置，程序填 `wust-authw.exe`（路径要换成他解压后的真实路径）；
   - 之后想知道自启有没有生效，看同目录下的 `wust-auth.log`。
4. 同学之间**不要互相传 `wust_config.ini`**，那是别人的密码。

> **同学第一次运行时大概率会遇到「Windows 已保护你的电脑」**（SmartScreen）。
> 这是未签名程序的常规提示，不是被杀毒报毒：点「更多信息」→「仍要运行」即可，
> 或者右键 exe → 属性 → 勾选「解除锁定」。
> 如果希望彻底避免，只能买代码签名证书，对校园小工具没有必要。

**同学常问的问题**

| 问题 | 回答 |
| --- | --- |
| 双击 `wust-authw.exe` 什么都没发生 | **正常**：它是 GUI 子系统的静默版，只在后台跑一次登录然后退出，不显示任何窗口。手动录入/改密码请用 `wust-auth.exe`；想看它干了什么就看 `wust-auth.log` |
| 双击 `wust-auth.exe` 窗口一闪就没了 | 说明它有任务在身（比如带参数启动了 `-a`），或者参数写错直接报了错。请开 CMD/PowerShell，`cd` 到目录后执行 `wust-auth.exe` 看完整输出 |
| 开机后任务计划里显示“正在运行”，但没登录、也没窗口 | 大多是**参数被写成了 `'-a'`**（单引号在 Windows 命令行里不是引号），程序认不出参数就以退出码 3 结束。v1.2 已自动兼容；也建议直接用 `wust-authw.exe`（不用参数）。详见 4.1 节 |
| 开机自启到底跑没跑？ | 静默运行时不会弹任何窗口，去看 `wust-auth.log`：每次运行都有一段带时间戳的记录和退出码 |
| 放在 `C:\` 根目录或 Program Files 里，提示保存配置失败 | 那里普通用户不可写。新版本会自动改存到 `%APPDATA%\wust-auth\`；也可以把 exe 挪到 `D:\wust-auth\` 这类目录 |
| 中文是乱码 | 旧版控制台不支持 UTF-8，先执行 `chcp 65001` 再运行；建议用 Windows Terminal |
| 提示「账号或密码不正确」 | 选菜单 `2` 重新录入；确认密码没输错、账号没被锁定 |
| 提示「连不上认证服务器」 | 先连上校园 WiFi/网线再运行；也可能校园网临时调整了认证服务器 |
| 提示读不到 / 找不到配置文件 | 先执行 `wust-auth.exe -p` 看具体原因，对照第 3.6 节的表处理 |
| 退出登录后网页都打不开，程序却说“已在线” | 旧版本的误判：门户把 `url=http://www.baidu.com` 回填到跳转地址里，被错当成“能上百度”。**用新版程序**即可（判定规则已重写，见第 6 节） |
| 想确认程序为什么这么判 | 加 `-v` 运行，探测那一行会直接写出判定依据，例如“被跳转到了别的站点（疑似校园网门户劫持）” |
| 换了 `WUST_PROBE_URL` 后总是说未联网 | 非百度地址需要用 `WUST_PROBE_MARKER` 指定正文特征串，否则无法确认真实站点，程序会保守判为未联网 |
| 提示与百度不通但能上网 | 探测用的地址被拦了，可以临时换一个：`set WUST_PROBE_URL=http://www.bing.com` 再运行 |

---

## 6. 工作原理（技术细节）

抓包确认的认证流程：

| 项目 | 值 |
| --- | --- |
| 请求地址 | `http://59.68.177.9/api/account/login` |
| 请求方式 | `POST` |
| `Content-Type` | `application/x-www-form-urlencoded;charset=UTF-8` |
| 表单参数 | `username=<学号>`、`password=<密码，已 URL 编码>`、`nasId=1` |
| 成功标志 | 返回 JSON 中 `"code": 0`（同时会带 `"authMsg": "已登录成功"`） |

程序做的事：

1. `GET http://www.baidu.com` 探测是否已经在线，判定规则如下（**原则：拿不准就算未联网**）：
   - `3xx`：只有跳转目标**属于同一个站点**才算通 —— 同域或互为子域，或同一个注册域
     （例如 `www.baidu.com` → `https://www.baidu.com`、`www.bing.com` → `cn.bing.com` 都是正常的站内跳转）；
     跳到别的站点（校园网门户的典型表现，例如跳到 `59.68.177.9`）判为未联网；
   - `204`：判为通；
   - `200`：正文里必须出现**真实站点的特征**才判为通。默认探测百度时要求出现
     `<!--STATUS OK-->`（百度首页自带的特征注释），或者出现“百度一下”且响应体足够大；
   - 未认证时门户会返回 `302` 跳自己、并把原地址当 `url=` 参数回填，或者给一个
     含有 baidu 字样/跳转脚本的 `200` 门户页 —— 这些**都不会**被当成已联网。
2. 已在线 → 打印「当前网络已经连通（已在线），无需重复认证」并退出，不发任何认证请求。
3. 未在线 → `POST` 上面的接口（密码先做 `application/x-www-form-urlencoded` 编码，
   所以密码里有 `&`、`%`、`=`、空格也能正确提交），解析返回 JSON 里的 `code`：
   `code == 0` → 登录成功，否则打印服务器给的 `authMsg` 并报登录失败。
4. 认证成功后再 `GET` 一次百度复检，通过才输出「网络已连通，可以正常上网」。
   如果认证服务器已经明确回了 `code: 0`、但复检探测不到外网，只会提示一句
   “探测站点可能不可达”，**不会**当成认证失败（认证本身已经成功）。
5. 认证成功后**不需要**再模拟点击“自服务登录”或请求任何展示页面。

**为什么不用 libcurl**：认证接口是明文 HTTP，用系统自带 socket 就能完成；
不引第三方库可以让 exe 拷贝即用、不用带 DLL，也不需要处理“忽略证书错误”的问题（本来就没有 TLS）。

**如果以后校园网换了认证服务器地址或 `nasId`**：用浏览器 F12 → Network 重新抓一次包，
然后改 `main.c` 顶部的 `DEFAULT_LOGIN_URL` 重新编译；
调试时也可以临时用环境变量覆盖，不必重新编译：

```bat
set WUST_LOGIN_URL=http://新地址/api/account/login
wust-auth.exe -a -v
```

| 环境变量 | 作用 |
| --- | --- |
| `WUST_CONFIG` | 指定配置文件路径 |
| `WUST_LOGIN_URL` | 覆盖认证接口地址 |
| `WUST_PROBE_URL` | 覆盖联网探测地址（默认 `http://www.baidu.com`） |
| `WUST_PROBE_MARKER` | 自定义探测时，正文里必须出现的特征串；出现了才算“已联网” |
| `WUST_LOG` | 静默运行（`-q`）时日志文件的路径 |

---

## 7. 安全提醒

- `wust_config.ini` 里是**明文密码**（校园网认证接口本身就是明文 HTTP，客户端加密没有意义）。
  请不要把这个文件发给任何人、不要提交到 Git、不要放公共网盘。
- Linux 下程序会把配置权限设成 `600`；Windows 下请自己注意别把文件夹共享出去。
- 不要把密码写在代码里或命令行参数里（命令行参数会出现在进程列表和历史记录中），
  本程序只从配置文件读取。

---

## 8. 开发者自测（可选）

`mock_server.py` 是一个模拟深澜接口的本地服务器，用来在不连校园网的情况下验证完整流程：

```bash
python mock_server.py 18080
```

它模拟：未认证时 `/probe` 返回 `302` 跳门户，认证成功后才返回真正的百度首页；
`POST /api/account/login` 校验 `username`/`password`/`nasId` 并返回 `{"code":0,...}` 或 `{"code":1,...}`。

> ⚠️ 这个假服务器只认一组写死的账号，用来验证登录流程：
> **学号 `2023000001`，密码 `MyP@ss&w=rd 1`**（故意带 `@ & =` 和空格，用来验证 URL 编码）。
> 用别的账号密码它会返回 `code: 1`，看起来像“密码错误”，其实是对的。

另外还提供了几个专门用来测“联网判定”的地址（不需要登录状态）：

| 地址 | 模拟的情况 | 期望判定 |
| --- | --- | --- |
| `/probe` | 未认证 302 跳门户，认证后放行 | 未联网 → 登录 → 已联网 |
| `/portal302-baidu` | 门户 302，且把 `url=http://www.baidu.com/` 回填 | 未联网 |
| `/portal200-baidu` | 门户 200 页面，正文含 baidu 与 JS 跳转 | 未联网 |
| `/portal200-clean` | 门户 200 页面，无 srun 关键字 | 未联网 |
| `/redir-same` | 同站点跳转（http 跳 https 那种） | 已联网 |
| `/redir-other` | 跳到别的站点 | 未联网 |
| `/real` | 真实百度首页 | 已联网（配合 `WUST_PROBE_MARKER`） |
| `/empty204` | 204 探测端点 | 已联网 |

然后用环境变量把程序指过去测试：

```bat
set WUST_PROBE_URL=http://127.0.0.1:18080/probe
set WUST_LOGIN_URL=http://127.0.0.1:18080/api/account/login
wust-auth.exe -a -v
```

```bash
WUST_PROBE_URL=http://127.0.0.1:18080/probe \
WUST_LOGIN_URL=http://127.0.0.1:18080/api/account/login \
./wust-auth -a -v
```

`GET http://127.0.0.1:18080/state` 可以看服务器记录到的表单内容（用来核对 URL 编码是否正确），
`GET /reset` 复位登录状态。
