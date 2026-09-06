# NovaOS

一个简洁的 32 位 C 语言操作系统内核，带 **多用户登录**（开机认证 + 密码哈希 + 文件属主权限）、**Ring3 用户态**（分页 + TSS + IDT + int 0x80 syscall）、**抢占式多进程**（PIT 100Hz + Ring3-only 抢占 + 4 进程槽）、**.nsh 批处理脚本**（.bat 兼容语法，内核/Ring3 双引擎）、交互式 shell `NovaSh`（方向键行内编辑）、**NovaFS**（256 inode / 多级目录 / 间接块 67KB 文件 / 递归删除 / 磁盘用量统计）、**运行时 ACPI/AML 解析关机**、**Bochs VBE 图形驱动（1024x768x32 真彩控制台）**、**e1000 网卡 + TCP/IP 网络栈**（DHCP / ICMP ping / DNS / UDP echo / **TCP + wget**）、**全屏 TUI**（`tui.nxp`：文件浏览器 / 任务监视 / 系统信息三面板）、**贪吃蛇**（第一个原生游戏）、**Windows BSOD 风格红屏崩溃页** 与美化启动画面，可用 QEMU 直接启动测试。（建议使用 QEMU 6.2，支持最好）

## 目录结构

```
NovaOS/
├── boot/
│   ├── stage1.asm          # MBR 引导扇区 (512B, LBA 加载；内核分两段读避开 64K 边界)
│   ├── stage2.asm          # Stage2: A20/GDT/进入保护模式/拷贝内核/跳转
│   └── kernel_entry.inc    # 内核布局常量 (加载地址 0x100000)
├── kernel/
│   ├── entry.asm           # 32位内核入口 stub (设栈/段, 调用 kmain) + syscall/fault/IRQ0 门
│   ├── kernel.c            # 内核主体: VGA/键盘/串口/RTC + 登录/多用户 + NovaSh + 进程命令
│   ├── paging.c / paging.h # Ring3: 分页/GDT/TSS/IDT/syscall + PIC/PIT + 多进程调度器
│   ├── ata.c / ata.h       # 磁盘 PIO ATA 驱动 (LBA 读写 512B 扇区)
│   ├── novafs.c / novafs.h   # NovaFS 文件系统: 多级目录 + 递归删除 + 磁盘用量统计
│   ├── acpi.c / acpi.h     # 最小 ACPI/AML 解析器 (RSDP→FADT→DSDT, 解析 \_S5 关机)
│   ├── gfx.c / gfx.h       # Bochs VBE 图形驱动: 1024x768x32 真彩控制台 (128x48)
│   ├── e1000.c / e1000.h   # Intel 82540EM (QEMU e1000) 网卡驱动: PCI 扫描 + 轮询收发环
│   ├── net.c / net.h       # 精简 TCP/IP 栈: ARP/IPv4/ICMP/UDP + DHCP/DNS/echo 命令
│   ├── stdint.h            # 最小 stdint
│   └── link.ld             # 链接脚本 (段紧凑于 0x100000)
├── programs/               # Ring3 用户程序源码 (build 自动编译注入)
│   ├── nxp.h / nxp_entry.c # .nxp API 头 (30 项) + 入口跳板 (+ no-op __chkstk_ms)
│   ├── hello.c             # 静态演示 (彩条)
│   ├── ringok.c / ringbad.c  # Ring3 健全性 / 用户态故障测试
│   ├── paint.c             # 鼠标画板
│   ├── snake.c             # 贪吃蛇 (第一个原生游戏)
│   ├── tui.c               # tui.nxp: 全屏 TUI (文件浏览 / 任务 / 系统三面板)
│   └── nsh.c               # nsh.nxp: 完整 NovaSh + .nsh 脚本引擎
├── scripts/                # .nsh 示例脚本 (build 注入 data-seed.img)
│   ├── hello.nsh           # 第一个脚本：变量 / 回显 / pause
│   └── count.nsh           # if/goto 循环倒数 3-2-1
├── build.ps1  build.bat    # 构建脚本 (NASM + gcc + ld + objcopy -> disk.img + data-seed.img)
├── run.ps1    run.bat      # QEMU 启动脚本 (双盘 + e1000 user 网络 + 7777 hostfwd + 监视器 4444 + 串口日志)
└── README.md
```

## 工具链

- **NASM** (汇编器)
- **MinGW-w64 gcc** (32 位 C 编译, `-m32 -ffreestanding -fno-pie`)
- **ld / objcopy** (链接 PE → 转换 flat binary)
- **QEMU** (`qemu-system-i386`)

构建脚本会自动查找以上工具。

## 构建

```powershell
.\build.ps1          # build.bat 效果相同
```

产物：
- `build\kernel.bin` —— flat binary（预算 126 扇区 ≈ 63 KB，`build.ps1` 超限会报错）
- `disk.img` —— 可启动的引导盘（只含 Stage1/2 + 内核）
- `data-seed.img` —— NovaFS 数据盘模板（含内置 .nxp 程序）

**双盘架构**：用户数据（账户 `/passwd`、家目录、文件）放在独立的**数据盘** `data.img`
（IDE 主通道从盘）上。`run.ps1` 第一次启动时从 `data-seed.img` 复制生成 `data.img`，
之后**永不覆盖** —— 重新编译内核不会丢账户和文件；想彻底重置就删掉 `data.img` 再运行。

引导盘 `disk.img` 布局（LBA，512 B / sector）：

| LBA 范围     | 内容                    |
|--------------|-------------------------|
| 0            | Stage1 (MBR)            |
| 1 – 2        | Stage2                  |
| 3 – 128      | Kernel（预算 126 扇区） |

数据盘 `data.img` 布局（LBA，512 B / sector）：

| LBA 范围     | 内容                    |
|--------------|-------------------------|
| 129          | NovaFS superblock        |
| 130 – 161    | Inode 表 (256 × 64 B)   |
| 162 – 169    | Block 位图              |
| 170+         | 数据块（共 32768 块）   |

> 内核加载缓冲在实模式 `0000:8400`，BIOS INT 13h 的缓冲不能跨 64K 边界，
> 所以 stage1 把内核**分两段**读：前 62 扇区 → `0000:8400`（至 0xFFFF），
> 后 64 扇区 → `1000:0000`（线性 0x10000，两段无缝相接），stage2 拷贝逻辑不变。
> 改动磁盘布局时必须同步 `kernel/novafs.h` 的 `FS_*_LBA` 常量与 `build.ps1` 的注入偏移。

首次启动时若 superblock magic 不符，NovaFS 会**自动初始化**，不用手打 `format`。

## 运行

```powershell
.\run.ps1            # run.bat 效果相同
```

QEMU 启动后：
1. Stage1 (0x7C00) INT 13h LBA 分两段加载 Stage2 + 内核（避开 64K 边界）
2. Stage2 开 A20 / 设 GDT / 切保护模式，把内核从 0x8400 拷贝到 0x00100000，`jmp 0x08:0x100000`
3. `entry.asm` 设置 SS/DS/ES/FS/GS = 0x10，栈顶 0x00200000，调用 C 的 `kmain`
4. 横幅 → 启动自检（cpu/video/com1/novafs/acpi/input/ring3/net）→ **登录提示** → NovaSh

提示：
- 用户数据在独立的 `data.img`（IDE 从盘）上：重新 build 不丢账户/文件；删掉 `data.img` 再运行即从模板重置（内置程序也会回来）
- 串口（COM1）输出被主机 `run.ps1` 接管，`shutdown` 通过内核内置的最小 ACPI/AML 解析器运行时定位 PM1_CNT 与 `\_S5`（不再硬编码 0x604），优雅关机；`reboot` 通过 8042 键盘控制器复位。
- 屏幕 + 串口双路输出，日志落到项目根目录 `serial.log`（含登录提示与各阶段 `[gfxdbg]` 状态探针）。
- 历史问题（已解决）：此前 `D:\qemu` 里混有早年手动拷入的旧版 GTK/glib/SDL2 DLL，QEMU 每次退出都在 ntdll 崩一次（c000000d，弹"已停止工作"）。用 Geek 强制卸载清空后重装官方 6.2.0 即根治，实测 9/9 次关机退出全部干净。若弹窗复现，先查事件查看器 Id=1000，与内核无关。

## 登录与多用户

开机自检完成后进入登录界面，认证通过才进 shell：

```
NovaOS login: root
Password: ****          ← 输入回显为 *，支持退格
Welcome, root. Type 'help' for commands.
```

- **默认账户**：`root` / 密码 `nova`（root 隐含存在；`/passwd` 里没有 root 行时用默认密码）
- **账户库**：NovaFS 根目录的 `/passwd`，每行 `用户名:FFFFFFFF`（8 位十六进制 FNV-1a 哈希，**不存明文**）
- **家目录**：`useradd` 自动建 `/home/<名字>`；非 root 用户登录（或 `su` 过去）后直接落在自己家里，root 落在 `/`
- **特权分离**：仅 root 可 `useradd` / `userdel` / `format`；`su` 时 root 切换免密、普通用户要输目标密码
- 改动会立即经 ATA 写回数据盘，重启、重新编译都不丢；`format`（仅 root）会清空账户（root/nova 仍可登录）
- 用户名上限 23 字符（NovaFS 文件名限制）；密码上限 31 字符、不允许空密码
- `logout` 回到登录界面可换账户登录
- **文件属主**：每个文件/目录记录创建者的数字 uid（存于 inode 的 `pad[0]` 字节，root=0；
  uid 由用户名哈希导出，旧数据盘上的文件 pad[0]=0 天然归 root）。非 root 用户
  **不能覆盖/删除别人创建**的文件和目录（`rm`/`rmdir`/`rd`/`write` 会得到
  permission denied）；`/passwd` 豁免 —— 改密码走内核的旧密码验证流程
- 读权限暂不设限（任何用户可读所有文件）；rwx 三位权限组尚未实现
- 登录提示与输入同时回显到串口，方便无显示调试

## NovaSh 命令

### 账户

| 命令          | 说明                                        |
|---------------|---------------------------------------------|
| `whoami`      | 显示当前登录用户                            |
| `passwd`      | 改当前用户密码（验旧密码 → 新密码输两遍）   |
| `useradd 名`  | 创建用户并建家目录 `/home/名`（**仅 root**）|
| `userdel 名`  | 删除用户及家目录（**仅 root**，root 不可删）|
| `su 名`       | 切换用户：root 免密，其他用户需输目标密码；切换后落到对方家目录 |
| `logout`      | 注销，返回登录界面                          |

特权模型：只有 **root** 能执行 `useradd` / `userdel` / `format`，
其他用户执行会得到 `permission denied`；文件**写入/删除**按属主检查（见「登录与多用户」），
读暂不设限，rwx 三位权限组尚未实现。

### 通用

| 命令       | 说明                                 |
|------------|--------------------------------------|
| `help`     | 列出所有命令                         |
| `ver`      | 版本 + 构建日期                      |
| `about`    | 关于 NovaOS                           |
| `echo X`   | 回显 X                               |
| `cls`      | 清屏                                 |
| `date`     | 日期 / 时间（CMOS RTC，带星期）      |
| `mem`      | 静态内存布局                         |
| `acpi`     | ACPI 表信息 + `\_S5` 解析结果        |
| `reboot`   | 重启（QEMU CPU reset）               |
| `shutdown` | 关机（运行时解析 RSDP→FADT→DSDT，经 `\_S5`/PM1_CNT 断电） |
| `halt`     | CLI 停机                             |
| `fsinfo`   | NovaFS **磁盘用量**（已用/空闲块、已用字节、inode）+ 当前目录条目数 |
| `format`   | 强制重新格式化 NovaFS                 |
| `run  名`  | `名.nsh` = 运行脚本；`名.nxp` = 启动进程并挂起 shell（见「多进程」） |
| `procs`    | 列出进程（`ps` 同义）；`s`=挂起 `r`=就绪 |
| `fg`       | 恢复全部挂起的进程                    |
| `kill N`   | 结束进程 N（不带 N = 结束全部）       |

### 网络

| 命令        | 说明                                                     |
|-------------|----------------------------------------------------------|
| `netinfo`   | NIC / MAC / IP / 掩码 / 网关 / DNS；未配置时自动跑 DHCP  |
| `dhcp`      | （重新）获取 DHCP 租约                                   |
| `ping IP`   | 发 4 个 ICMP echo，逐包显示时延                          |
| `dns NAME`  | DNS A 记录查询（SLIRP 转发给宿主机解析器）               |
| `wget H[/P]`| HTTP GET（TCP），响应体按路径名存入 NovaFS，可 `cat`     |
| `udpecho`   | UDP echo 服务器（端口 7777），按 `q` 停止                |

### 目录操作

| 命令        | 说明                                        |
|-------------|---------------------------------------------|
| `ls`        | 列当前目录，目录标 `[DIR]`、文件标 `[FILE]` + 大小 |
| `cd 路径`   | 切目录；支持 `..`（父）、`/`（根）            |
| `mkdir 名`  | 在 cwd 下建子目录                            |
| `rmdir 名`  | 删**空**目录                                 |
| `rd 名`     | **强制删除整个目录树**（≈ `rm -rf / rd /s/q`）<br>保护：不能删 cwd 本身或其祖先，需先 `cd ..` |

Shell prompt 显示 当前用户 和当前工作目录：`root@novaos:/docs/sub#`

### 文件操作

| 命令         | 说明                                                     |
|--------------|----------------------------------------------------------|
| `cat  名`    | 显示文件内容                                             |
| `write 名`   | 新建/覆盖文件。逐行输入，**单独一行输入 `.` 结束**        |
| `rm  名`     | 删除文件（目录请用 `rmdir` / `rd`）                      |
| `mkdemo`     | 生成示例程序 demo.nxp（然后 `run demo.nxp`）             |

注：单个文件大小上限 = 6 直接块 + 1 间接块（128 项）= **68608 B**；3072 B 以内只用直接块（旧版完全兼容）。

## 多进程

NovaOS 拥有**抢占式多任务**：PIC 重映射 + PIT 定时器 100Hz 产生 IRQ0，调度器只在
**打断用户态（Ring 3）时切换任务**，内核态永不抢占 —— 从结构上杜绝内核重入问题。

### 进程槽位

| 槽位 | 链接基址    | 用户栈顶    | 程序文件名       |
|------|-------------|-------------|------------------|
| 0    | 0x300000    | 0x500000    | `名.nxp`         |
| 1    | 0x320000    | 0x4C0000    | `名.1.nxp`       |
| 2    | 0x340000    | 0x480000    | `名.2.nxp`       |
| 3    | 0x360000    | 0x440000    | `名.3.nxp`       |

- `.nxp` 是按固定地址链接的平坦二进制，无法重定位，所以 **build.ps1 为每个程序按
  4 个槽位基址各链接一份**（`paint.nxp` / `paint.1.nxp` / …）。同一程序想开两份？
  `run paint.nxp` + `run paint.1.nxp` 即可
- 单一地址空间、**无 CR3 切换**：任务切换只保存/恢复 13 字寄存器帧（pushad +
  eip/cs/eflags/esp/ss），TSS ESP0 栈每次系统调用完整展开，天然无残留
- 最多 4 个并发进程；`procs`（或 `ps`）查看，pid = 槽位 + 1

### 使用方式

```
root@novaos:/# run nsh.nxp            ← shell 挂起自身，进入调度器
[proc] started nsh.nxp pid=1  (F11 suspend / F12 kill)
  （nsh 内）run paint.1.nxp           ← 进程内用 API->spawn 再拉起进程，自己继续跑
pid 2
  （nsh 内）procs                     ← 1 r nsh.nxp / 2 r paint.1.nxp
  按 F11                              ← 全部冻结，回到内核 shell
[proc] suspended - 'fg' resumes, 'kill' ends
root@novaos:/# procs                  ← 1 s nsh.nxp / 2 s paint.1.nxp
root@novaos:/# fg                     ← 解冻，重新进入调度器
root@novaos:/# （F12 = 不挂起直接全杀回 shell）
[proc] all processes exited
```

- `run` 会**挂起 shell**：只要有进程活着就回不到提示符；全部退出（或 F12/F11）才返回
- 用户程序崩溃只死它自己，其余进程继续跑；`fg` 从保存的寄存器帧精确恢复
- `getchar()` 已改为**非阻塞**（无键返回 -1），交互程序在自己的时间片里轮询

### 进程隔离与已知限制

- **内存隔离**：系统调用只能访问调用者自己槽位的镜像（+BSS）、自己的栈窗
  （32KB）和 trampoline 页 —— 虽是共享地址空间，进程 A 再也改写不了进程 B 的内存
- **独立当前目录**：每个进程有自己的 NovaFS cwd（spawn 时继承），系统调用期间
  内核自动切换并在返回前恢复；目录被其它进程删掉则自动回落到根
- **控制台自动清屏**：最后一个进程退出/被杀（含 F11/F12）时内核自动清屏，
  shell 总是拿到干净的画面；还有别的进程存活时不清（保护同伴的画面）
- 仍共享：键盘（轮询者各自抓键）、用户身份（所有进程都以当前登录用户的 uid 执行）
- F11 是硬挂起，若进程正卡在磁盘写入的系统调用中会丢弃该操作
- 最多 4 进程；无优先级，纯轮转；无 fork / IPC

## .nsh 脚本

`nsh.nxp` 内置 **.bat 兼容语法的脚本引擎**。脚本是纯文本文件（内核 NovaSh 用 `write
名字.nsh` 创建，或预置在 `scripts\` 由 build 注入数据盘），在 nsh 里**直接输入文件名**
或 `run 名字.nsh` 执行。

**内核 NovaSh 也能直接跑 .nsh 脚本**（`run 名字.nsh`，无需进入 nsh.nxp）—— 内核侧
引擎把脚本里的未知命令委托给 shell 命令分发器，因此脚本可以使用**全部 NovaSh 命令**
（`mkdir` / `write` / `useradd` / `ls` / `run` …），适合系统管理类脚本；两处引擎语法
保持一致。脚本里 `run X.nxp` 会挂起 shell 进入多任务，全部进程退出后脚本继续往下走。

```bat
@echo off                        ← 关闭命令回显（@cmd = 单行不回显）
rem 这是注释（:: 也可以）
echo Hello, %who%!               ← set who=world 后用 %who% 引用
if not %who%==world goto bad     ← 字符串比较（引号可选，比较串内不能有空格）
:loop                            ← 标签
echo count = %n%
if %n%==1 goto end
goto loop
pause                            ← 按任意键继续
exit                             ← 结束脚本（交互态则是退出 nsh）
```

支持项与限制：变量 8 个（名字 ≤11 字符、值 ≤31 字符，未定义的 `%名%` 保持原样）；
标签区分大小写（`goto :L` 与 `goto L` 等价）；比较串内不能有空格（两边同样加引号会
自动抵消）；不支持嵌套脚本、算术 `set /a`、`for` 循环。示例见 `scripts\hello.nsh`
（变量/pause）与 `scripts\count.nsh`（if/goto 循环倒数）。

另外：在 nsh 里输入**未知的词**会先尝试当作 `.nsh` 脚本执行（batch 习惯），输入
`名.nxp` 不会自动执行，请用 `run 名.nxp`（那会 spawn 一个新进程）。

## Ring3 用户态

NovaOS 实现完整的 Ring 0 ↔ Ring 3 切换，所有 `.nxp` 用户程序都在受限的 Ring 3 下运行。
内核做了三件事：

1. **Identity Paging + 独立用户页**：页目录（PDE）0–8MB 全带 `US=1` 标志；0x400000–0x500000 范围（syscall trampoline + 用户栈）的 PTE 也设置 US，其余物理内存保持 Supervisor only。
2. **GDT / TSS / IDT**：GDT 追加 `CS_R3=0x1B`、`DS_R3=0x23`（DPL=3）；TSS 填好 `SS0=0x10 / ESP0`，Ring3 触发中断时 CPU 自动切回内核栈；32 个异常向量 + 0x80 syscall 门 + IRQ0 抢占门（PIC 重映射 + PIT 100Hz）全部由 entry.asm / paging.c 生成。
3. **Syscall 跳板页（0x00400000）**：用户程序调用 API 时，`call [0x400Fxx]` 跳转到该页的 `int 0x80` stub；内核在进入用户程序前把 syscall stub + API 表拷贝到 trampoline。

Ring3 规则：
- 用户态 `DS/ES/FS/GS` 必须是 0x23（RPL=3），iretd 后由 kernel stub 预装载
- 用户态 CS/RPL=3 发生异常时，`fault_dispatch` 走 "user fault" 分支：打印 `eip/err/cr2` → `ring3_leave()` 杀程序 → 返回 shell
- 内核态异常：**触发 BSOD 红屏** + `cli/hlt` 永久停机（见下一节）

## .nxp 程序格式

- **文件**：4 字节魔数 `NXP\x01` + 平坦 32 位保护模式机器码
- **加载**：**每个槽位一个链接基址**（0x300000 / 0x320000 / 0x340000 / 0x360000，
  见「多进程」；MinGW `ld --image-base <基址>` 保证 .data/.bss 不越界），
  入口 = 加载地址 + 4；每槽独立栈（0x500000 / 0x4C0000 / 0x480000 / 0x440000 向下），
  程序上限 68608 B（间接块），槽内 BSS 可用至 ~124 KB
- **Syscall**：通过 trampoline（0x400000）的 `int 0x80` 进行，ABI 与 cdecl 完全一致
- **入口约定**：`EAX` = 用户可见 API 表指针（0x00400F80，结构体见 kernel.c 的 `nxp_api_t`）：

```
+0x00 magic 'NXP1'   +0x04 version=1
+0x08 putc(c)        +0x0C puts(s)         +0x10 getchar()    +0x14 exit()
                      ↑ getchar 为**非阻塞**轮询：无键返回 -1（多任务下进程在
                        自己的时间片里轮询，交互循环写法 `while((c=getchar())<0);`）
+0x18 scr_w          +0x1C scr_h           （无图形时为 0）
+0x20 pixel(x,y,rgb) +0x24 fill_rect(x,y,w,h,rgb) +0x28 text(x,y,s,rgb)
+0x2C getkey()       ← 非阻塞读键：无键返回 -1（交互程序用）
+0x30 mouse(&dx,&dy,&btns)  ← 非阻塞读鼠标：返回积累的包数（0=无输入），
                              输出自上次调用以来的位移增量（屏幕坐标 +y 向下）
                              与当前按键位；读后清零。按键：NXP_BTN_L/R/M = 1/2/4
+0x34 get_pixel(x,y) ← 读一个像素。做 XOR 软件光标用（画两次自动还原，
                      任何背景上都可见且不留痕迹），paint.nxp 的十字光标即此实现
+0x38 cls()           ← 清屏 + 光标归位
+0x3C set_color(fg)   ← 设 VGA 属性前景色，取值见 nxp.h 的 NXP_COLOR_* (0x00–0x0F)
+0x40 getuser(buf,max) ← 把当前登录用户名拷进 buf，返回长度（Ring3 无需读 /passwd）
+0x44 getdate(buf,max) ← 把格式化 RTC 日期行 "Date: …  Time: …" 拷进 buf，返回长度
+0x48 readfile(name,buf,max) ← 读 NovaFS 当前目录下的文件进 buf，返回实际大小（-1=错）
+0x4C spawn(name)      ← 把 name.nxp 启动为**新进程**（名.1/2/3.nxp 选槽位），
                         返回 pid（1–4），-1 = 槽位忙/文件错；调用者继续运行
+0x50 procs(buf,max)   ← 把存活进程列表（每行 "pid 状态 名字"）拷进 buf，返回条数
+0x54 writefile(name,data,len) ← 新建/覆盖 NovaFS 文件（属主检查生效），
                         返回写入长度，-1 = 错误/权限不足。配合 readfile，
                         用户程序第一次拥有了完整的文件读写能力
+0x58 ticks()          ← 系统启动以来的 10 ms 计数（IRQ0 累加），
                         游戏节拍/动画定时的基准时钟
+0x5C putcell(x,y,ch,attr)  ← TUI 原语：写一个字符单元（VGA attr: fg|bg<<4）
+0x60 cellfill(x,y,w,h,chattr) ← TUI 原语：填矩形，chattr = ch<<8|attr
+0x64 cputs(x,y,s,attr)     ← TUI 原语：在单元坐标打印字符串
+0x68 cursor(on)       ← TUI 原语：隐藏/恢复文本光标
```

TUI 原语绕过滚动控制台，直接按单元寻址：LFB 控制台由 `gfx_cell()` 逐单元渲染
（每单元独立前景/背景色，无光标/滚动副作用），VGA 文本后备直写 0xB8000。
`programs/nxp.h` 提供 `NXP_ATTR(fg,bg)` 与 CP437 制表符常量（╔ ═ ║ 等，
VGA BIOS 字体自带）。注意 syscall 的用户指针只接受**调用者槽位镜像前 8KB
或自身 32KB 栈窗** —— 传给 listdir/procs/readfile/getdate/getuser 的大缓冲
必须放栈上（tui.c 即此写法）。

方向键扩展码：`NXP_KEY_LEFT/RIGHT/UP/DOWN` = 0x11/0x12/0x13/0x14（nxp.h 有定义）。

## BSOD（红屏崩溃页）

内核态异常（任何 0–31 号 CPU 向量，如 #GP / #PF / #DF）会立即触发 Windows BSOD 风格的 **纯亮红色崩溃屏幕**，调试用：

```
:(

NovaOS ran into a problem and needs to halt.
We're collecting fault info, then stopping for you.

#GP General Protection
EIP: 0x00102034   ERR: 0x00000000   CR2: 0x00000000
CS:  0x00000008   EFL: 0x00010002

Registers:
EAX / EBX / ECX / EDX / ESI / EDI / EBP / ESP

Halted. Power off to restart.
```

- 背景色：纯亮红 `0x00FF0000`（不经过 VGA 调色板，直接写 RGB framebuffer）
- 前景色：白 `0xFFFFFF`
- 异常名称表含全部 32 个向量（`#DE / #DB / NMI / #BP / #OF / #BR / #UD / #NM / #DF / #TS / #NP / #SS / #GP / #PF / #MF / #AC / #MC / #XM / #VE / #CP …`）
- 串口同步输出同样内容，调试时直接看 `serial.log` 不用截图

用户态程序崩溃**不会**触发 BSOD：肇事进程被结束（其余进程继续跑，多进程下调度器
直接切入下一个；一个都不剩则回 shell），并打印 `[nxp] user fault: <异常名> eip=… err=… cr2=…`（`ringbad.nxp` 就是测这个用的）。

- 所有回调为 cdecl；`ret` 或调用 `exit()` 均可安全返回 shell
- `.nxp` 程序统一由 `programs/nxp_entry.c` 提供入口包装：压参数→调 `nxp_main(argc, argv)`→调 `exit(ret)`
- `mkdemo` 生成的 demo.nxp 即按此 ABI 手写机器码，可作参考

### 宿主机开发工作流（programs\ 与 scripts\ 目录）

把 C 源文件放进 `programs\`（如 `hello.c`），运行 `build.ps1` 即自动：

1. 编译链接为 `.nxp`（`nxp_entry.c` 提供入口跳板，链接在最前；`nxp.h` 是 API 头），
   **每个程序按 4 个槽位基址各链接一份**，文件名 `名.nxp` / `名.1.nxp` / `名.2.nxp` /
   `名.3.nxp`（多进程无需运行时重定位的关键）
2. 直接写入数据盘模板 `data-seed.img` 的 NovaFS 区域（宿主机生成超级块/inode/位图/数据块）；
   `scripts\*.nsh` 也会作为纯文本注入根目录
3. 开机后 `NovaFS mounted`，直接 `run 文件名.nxp` 执行

**重要**：已存在的 `data.img` 不会被覆盖 —— 改了 `programs\` 或 `kernel\` 之后想让
guest 用上新版，**删掉 `data.img` 让它重新播种**（用户数据会一并重置，注意备份）。
旧程序配新内核可能出现诡异行为（例如旧版 nsh 配非阻塞 getchar 会刷屏）。

写法参考：
- `programs/hello.c` — 静态演示，putc/puts 输出
- `programs/ringok.c`  — **最小 Ring3 健全性测试**：打印 `[user] ring3 alive (ringok)` 然后 `exit(0x42)`；用它验证 Ring3 进入/返回正常
- `programs/ringbad.c` — **用户态故障测试**：故意 `*(volatile uint32_t*)0x100000 = 1` 写只读内核页，期待触发 "user fault: #PF …" 然后安全回 shell（**不触发 BSOD**）
- `programs/paint.c` — **交互式画板**：鼠标移动画笔，左键画、右键擦、方向键/WASD 移动、1-8 换色、c 清屏、q 退出（非阻塞 `getkey` + `mouse`）
- `programs/snake.c` — **贪吃蛇**：方向键/WASD 转向，吃红色食物变长加速，撞墙/咬到自己结束；SPACE 重开、ESC/q 回 shell。节拍来自 `ticks()` 系统调用（IRQ0 的 10ms 计数），任何宿主机速度一致；和 paint 同时 `run` 可以真的边聊天边玩
- `programs/nsh.c` — **完整 NovaSh 移植版（Ring3）**：内核 shell 的全部命令
  （ls/cd/mkdir/write/cat/rm/format/fsinfo/useradd/su/passwd/acpi/reboot/shutdown…）
  通过 `listdir`/`fsop`/`sysop` 三个系统调用复用内核实现，文件属主与进程 cwd 规则
  与内核完全一致；再加上 `.bat` 兼容的 .nsh 脚本引擎、方向键行内编辑、
  `spawn` 多进程、`procs`/`fg`/`kill`。从 NovaSh `run nsh.nxp` 进入，
  提示符 `用户@novaos:nsh#`，`exit`/`logout` 回内核 shell

单程序上限 68608 B（6 直接块 + 间接块）。注意 MinGW 链接 `.nxp` 必须加
`--image-base <槽位基址> --section-alignment 16`：前者防止 .data/.bss 被 ld 放到
0x400000+ 覆盖 syscall trampoline，后者避免 PE 4K 对齐把程序撑到十几 KB。
超过 3072 B 的部分经 inode `pad[0..3]` 指向的**间接块**（128 个块号）寻址，
`build.ps1` 与内核的读写/释放路径均已实现。

## NovaFS 设计

- **超级简单**，没有间接块、没有目录块：目录结构直接编码为 inode 的 `uint16_t parent` 字段
- **超级块必须住满整个扇区**：ATA PIO 按扇区（512 B）整块传输，内核里的 `super_t` 只有 20 B，
  若直接读进结构体会把 492 B 磁盘数据泼到相邻 .bss 变量上（曾把 gfx 控制台状态整个清零、
  屏幕冻结在 com1 —— 已用 `sb_sec[512]` + 宏修复）。同理，所有 `ata_read/ata_write` 的
  缓冲区都必须是 512 的整数倍
- inode 共 256 个，64 B / 个：

```
typedef struct {
    uint8_t  type;          // T_FREE=0 / T_FILE=1 / T_DIR=2
    uint8_t  flags;
    uint16_t parent;        // 父目录 inode 编号（root=0）
    char     name[24];      // 文件名，0 终止
    uint32_t size;          // 字节数
    uint32_t block[6];      // 直接块（数据块编号，0 = 空）
    uint8_t  pad[6];
} inode_t;                   // = 64 B
```

- 数据块 512 B / 块，位图 8 sectors = 32768 bits，支持最多 32768 块
- **单间接块**：inode `pad[0..3]` 复用为间接块号（128 个块指针/块），
  文件上限 = 6 直接块 + 128 间接块 = 68608 B；pad 历史上恒为 0，旧盘向后兼容
- inode 的 `pad[0]` 在引入间接块**之前**曾是属主 uid —— 现属主 uid 移到 `flags` 字节
  （0 = root），登录/`su` 时内核调 `fs_setuid()` 切换当前身份，新建的文件/目录打上
  创建者的 uid；非 root 对他人对象的写入/删除操作返回权限错误（`/passwd` 豁免）
- 每次 inode / bitmap 变动都通过 ATA PIO `ata_write()` 写回磁盘，**持久化**
- 账户库 `/passwd` 就是根目录下的普通文件（多行 `用户名:哈希`），随 NovaFS 持久化

## 内存布局

| 区域                | 地址          | 权限       | 说明                                      |
|---------------------|---------------|------------|-------------------------------------------|
| Stage1 加载点       | 0x00007C00    | 实模式     | BIOS 加载 MBR 扇区                        |
| Stage2 加载点       | 0x00007E00    | 实模式     | Stage2 代码（开 A20 / 保护模式）           |
| 内核临时加载点      | 0x00008400    | 实模式     | Stage2 暂存 kernel.bin 的缓冲区            |
| VGA 文本缓冲        | 0x000B8000    | 内核 R/W   | 80×25 文本模式 fallback                    |
| VBE LFB             | 0xE0000000    | 内核 R/W   | Bochs VBE 线性帧缓冲（1024×768×32，3 MB） |
| 内核镜像            | 0x00100000    | 内核 R/X   | kernel.bin 加载地址（identity paging）    |
| Ring0 栈顶          | 0x00200000    | 内核 R/W   | 向下增长                                  |
| .nxp 槽0 程序       | 0x00300000    | Ring3 R/X  | 进程槽0；槽1/2/3 = 0x320000/0x340000/0x360000 |
| Syscall Trampoline  | 0x00400000    | Ring3 R/X  | `int 0x80` stub + 0xF80 起的 API 表       |
| 进程栈 槽0–3        | 0x500000 / 0x4C0000 / 0x480000 / 0x440000 | Ring3 R/W | 各自向下增长          |
| Identity 页目录/表  | 0x001FF000    | 内核 R/W   | 1 PDE + 2 PT（0–8MB，identity map）       |
| NIC MMIO (e1000)    | PCI BAR0 运行时探测 | 内核 R/W | `page_map_device()` 4MB 大页恒等映射  |

## 键盘

- PS/2 scancode set 1
- `Shift` / `CapsLock` 正常切换大小写
- **行内编辑**（NovaSh 与 nsh）：`←`/`→` 移动光标，光标中间可插入/删除，
  右侧内容自动重绘（实现：`putc('\v')` = 光标左移不擦除，编辑后重印尾部再退回）
- 退格键删除光标前一个字符（限制 128 字符命令行缓冲；单行超出屏幕宽度后编辑不可靠）
- F11 / F12 为调度器热键（挂起 / 全杀，见「多进程」），普通程序不会收到
- 目前不支持 `Tab` 补全、`↑↓` 历史命令

## 鼠标

- PS/2 辅助口（8042 aux）轮询驱动：`0xA8` 开口、清配置位 0x20、`0xF6` 默认值、`0xF4` 开流式上报（校验 `0xFA` ACK）
- 键盘和鼠标共用端口 `0x60`，靠状态寄存器 bit5（aux 标志）区分字节归属，统一由 `ps2_drain()` 分发
- 3 字节包 `[flags dx dy]`，bit3 作包同步位，9 位符号扩展；PS/2 的 +y 向上已换算为屏幕 +y 向下
- 初始化期间的命令 ACK / 配置字节**不是**包数据，只消费不进包组装器（否则会错位一整个包）

## 网络

NovaOS 内置 **e1000 网卡驱动 + 精简 TCP/IP 栈**（`kernel/e1000.c` + `kernel/net.c`），
全部轮询驱动：无中断（IMS=0）、无线程，每个等待型命令在自己的循环里调 `net_poll()`
收帧、以 `proc_ticks()`（IRQ0 的 10ms 计数）做超时。在 QEMU 用户态网络下开箱即用：

- **驱动**：PCI 总线 0 扫描 `8086:100E`（QEMU 默认网卡，00:03.0），开 MEM + Bus
  Master；MMIO BAR 由 `paging.c` 新增的 `page_map_device()` 用 **4MB 大页恒等映射**
  （P|RW|PCD|PS，VA==PA，描述符地址直接喂 DMA）。legacy 收发环：TX 8 项 / RX 16 项
  × 2KB 缓冲。MAC 先试 EERD（QEMU 语义：地址<<2、DONE=bit1），失败回落 RA0
- **协议栈**：ARP（8 项缓存 + 应答别人的请求）→ IPv4 → ICMP echo / UDP
  （DHCP、DNS、echo 服务）/ **TCP**（精简客户端，见下）。**仅支持链路层直达
  目标** —— QEMU user 网络就是一个 /24，10.0.2.x 全部直达，所以日常够用；
  没有经网关的路由转发
- **DHCP**：标准 DISCOVER→OFFER→REQUEST→ACK。细节坑：拿到 OFFER 后**立即**
  用租约 IP 应答 ARP（`g_ip` 先行、`g_ip_set` 仍为 0）—— SLIRP 对已知客户端的
  ACK/再次 OFFER 都要**先 ARP 再单投**，不应答 ARP 就永远收不到包
- **ICMP**：内核会回应 echo request（宿主机可 ping 10.0.2.15），`ping` 命令发 4 个
  echo 并按 id/seq 匹配回复
- **TCP**：单连接客户端状态机（SYN 握手 / 顺序收流 / 累积 ACK / FIN 关闭，
  每段 500ms 重传）。接收环 16KB，**只 ACK 实际装入环的字节**，装不下的靠对端
  重传（首次实现无条件推进 ACK 序号，12KB 响应丢了 3.8KB——TCP 的背压就是这么
  来的）；收发窗口固定 4096，SLIRP 永远追不上环。`wget` 是它唯一的用户
- **UDP echo 服务器**：7777 端口，配合 `run.ps1` 的 `hostfwd=udp::7777-:7777`，
  宿主机往 `127.0.0.1:7777` 发 UDP 会被 guest 原样回显（跨机链路验证）

`nsh.nxp` 里同名命令经 `sysop` 操作码 12–17（netinfo/ping/dhcp/dns/udpecho/wget）
走同一份内核实现，Ring3 与内核行为完全一致。

### QEMU 侧接线（run.ps1 已配好）

- `-nic user,model=e1000,hostfwd=udp::7777-:7777`：用户态网络 + 7777 端口前送
- `-monitor tcp:127.0.0.1:4444,server,nowait`：宿主机可连 QEMU 监视器（自动化用）
- `tests\nettest.ps1`：无头启动 + 监视器 `sendkey` 驱动登录 → netinfo → ping →
  dns → udpecho + 宿主机 UDP 回环，读 `serial-nettest.log` 逐项判定；用临时数据盘
  `data-nettest.img`，**不碰 data.img**。`tests\nshtest.ps1` 额外验证 nsh.nxp 路径，
  `tests\wgettext.ps1`（配 `tests\httphost.ps1` 宿主机 HTTP 服务器）端到端验证
  TCP/wget

### 已知限制

- TCP 为单连接客户端（无并发、无服务端、无 IP 分片重组）；无网关路由（目标必须在链路上）
- UDP 校验和恒为 0（IPv4 允许）；DNS 只取第一条 A 记录（CNAME 跳过）
- `net_poll` 只在 ping/dhcp/dns/wget/udpecho 等命令的等待循环里被调用，
  没有后台收包；RX 环 16 帧，burst 超过会丢

## TUI

`run tui.nxp` 进入全屏字符界面（Ring3 进程，shell 挂起，F11/F12 照常可用），
现代暗色主题：近黑背景、暗灰细边框、青色点缀、胶囊页签、图标化列表
（`►` 目录 / `·` 文件）、右对齐暗灰尺寸列、`░/█` 滚动条、暗灰选中行。
四个面板，`1/2/3/4` 或 `←/→` 切换，`q`/ESC 退出：

| 面板        | 内容与按键                                                          |
|-------------|----------------------------------------------------------------------|
| `1 Files`   | NovaFS 浏览器 + **实时预览面板**（右侧，选中即显示带行号的文本预览；NXP 二进制识别后显示摘要和运行提示；屏幕 <100 列时自动隐藏）。`↑/↓` 选择，`Enter` 进目录 / 全屏查看文件（`↑/↓` 滚动，任意键返回），`Backspace` 回上级 |
| `2 Tasks`   | 进程列表每秒刷新（`●` 状态点：绿=就绪、黄=挂起）；`↑/↓` 选择，`k` 结束选中进程（拒绝杀 TUI 自己）    |
| `3 System`  | 当前用户 / RTC 日期时间 / 开机时长 / 屏幕规格（128x48 LFB 或 80x25 VGA）|
| `4 Settings`| **全局设置**（写入 NovaFS 根目录 `nova.cfg`，内核 + TUI 共同消费，见「全局设置」）：强调色（青/绿/黄/品红/白，shell 提示符/帮助/登录界面 + TUI 全部即时跟随）、Quiet boot（静默启动，下次启动生效）、预览面板、顶栏时钟、大小格式、选中样式。`↑/↓` 选择、`←/→` 修改，即时生效并自动保存；TUI 保存后经 sysop 18 通知内核热重载 |

两个控制台后端都支持：VBE LFB（128x48 双栏）与 80x25 VGA 文本后备（单栏），
尺寸自适应（`scr_w=0` 即按 80x25）。

### 全局设置（nova.cfg）

NovaFS 根目录的 `nova.cfg` 是系统级配置（键值对文本），内核在文件系统挂载后
（横幅之前）立即读取，TUI Settings 面板保存后经 sysop 18 触发热重载：

| 键 | 值 | 消费者 | 说明 |
|----|----|--------|------|
| `accent` | cyan/green/yellow/magenta/white | 内核 + TUI | shell 提示符、帮助表头、登录界面和 TUI 的强调色，改动即时生效 |
| `quiet` | on/off | 内核 | 静默启动：跳过横幅和全部自检输出，直接到登录提示（下次启动生效） |
| `preview` / `clock` / `sizes` / `selection` | 见 Settings 表 | TUI | TUI 私有偏好 |

TUI 启动时会 `cd /` 保证 nova.cfg 落在内核读取的位置；文件不存在时全部用默认值。

实现要点（想写自己的 TUI 程序照抄即可）：
- 内核只提供 4 个编元寻址 syscall（putcell / cellfill / cputs / cursor，见
  「.nxp 程序格式」），控制台路由在 `kernel.c` 的 `con_put/con_fill/con_cursor`，
  LFB 渲染在 `gfx.c` 的 `gfx_cell()`
- `tui.nxp` 约 5.9 KB，由 `build.ps1` 自动按 4 个槽位链接注入，开机即
  `run tui.nxp` 可用
- 坑 1：`.nxp` 入口 = 镜像第一个字节 —— `nxp_entry.c` 里**文件级 `__asm__`
  会被 GCC 提到所有函数之前**，把入口顶掉（表现为程序一进 Ring3 就直接退
  出），所以 no-op `__chkstk_ms` 必须写成 C 函数放在 `nxp_entry` 之后
- 坑 2：MinGW 对 >4KB 栈帧插入 `__chkstk_ms` 探针；用户栈全部实体映射、
  无 guard page，`ret` 空实现即可（或像 tui.c 一样把栈帧压在 4KB 内）
- 坑 3：syscall 缓冲的 uptr 窗口是**槽位镜像前 32KB**（tui 的 BSS 在镜像后段，
  旧版 8KB 窗口会把 listdir/readfile 的指针静默拒绝——文件名全部画不出来）
- 视觉回归：`tests\tuitest.ps1`（无头启动 + 监视器 sendkey 驱动 + QEMU
  `screendump` 截屏，`tests\ppm2png.ps1` 转 PNG 人工/自动检查）

## 工具与测试脚本

非临时脚本放 `tools\`（实用工具）和 `tests\`（自动化回归）；`build\` 只放
构建产物和测试产生的临时文件，不进 git。

### tools\inject.ps1 —— 向现有 NovaFS 盘非破坏式注入程序

`data.img` 只在第一次从 `data-seed.img` 播种，之后 build 更新了 `programs\`
**旧的 data.img 里不会有新程序**（表现为 `run xxx.nxp` 报 no such file）。
过去只能删 `data.img` 重新播种（账户和文件全丢）；注入器可以原地解决：

```powershell
tools\inject.ps1 -Image data.img -Programs tui,nsh    # 自动备份 -> data.img.bak
```

- 对每个名字注入 4 个槽位二进制（`build\nxp_名0.bin` → `名.nxp` …），
  同名文件原地覆盖（保留 inode 和属主 uid），新名字分配新 inode
- 直接块 + 间接块都会正确释放/重建，不动其他任何文件
- 想彻底重置仍然可以删 `data.img` 让 `run.ps1` 重新播种

### tests\ —— 自动化回归（全部无头运行，不碰 data.img）

| 脚本              | 内容                                                       |
|-------------------|------------------------------------------------------------|
| `nettest.ps1`     | 登录 → netinfo(DHCP) → ping → dns → udpecho + 宿主机 UDP 回环 |
| `nshtest.ps1`     | nsh.nxp 里走 sysop 的同名网络命令                          |
| `wgettext.ps1`    | 配 `httphost.ps1`（宿主机 8080 HTTP 服务器），端到端验证 TCP/wget 并 `cat` 回读 |
| `tuitest.ps1`     | 驱动 TUI 三个面板 + 文件查看器，`screendump` 截屏逐屏检查   |
| `ppm2png.ps1`     | QEMU 截屏 (PPM) 转 PNG 的小工具                            |

测试用临时数据盘 `data-nettest.img`（由 `data-seed.img` 复制），产物
`serial-nettest.log` / `build\shot*.ppm` 均为临时文件。

## 常见流程示例

```
NovaOS login: root
Password: ****
Welcome, root. Type 'help' for commands.

root@novaos:/# useradd alice
New password: ****
Retype new password: ****
User 'alice' created.

root@novaos:/# format
Formatting NovaFS... done. (root ready)

root@novaos:/# mkdir docs
root@novaos:/# mkdir docs/old          ← 不，不能带 "/"，要逐层 cd
root@novaos:/# cd docs
root@novaos:/docs# mkdir old
root@novaos:/docs# write todo.txt
Enter text. End with a single '.' on its own line:
> buy milk
> write os code
> .
Saved XX bytes.

root@novaos:/docs# ls
  [DIR]  old
  [FILE] todo.txt  (XX B)
  2 item(s)

root@novaos:/docs# cat todo.txt
buy milk
write os code

root@novaos:/docs# cd ..
root@novaos:/# rmdir docs
rmdir: directory not empty
root@novaos:/# rd docs
Directory removed.

root@novaos:/# logout                  ← 换 alice 登录
NovaOS login: alice
Password: ****
Welcome, alice. Type 'help' for commands.

alice@novaos:/# whoami
alice
```

### 多进程 + .nsh 脚本示例

```
root@novaos:/# run nsh.nxp
[proc] started nsh.nxp pid=1  (F11 suspend / F12 kill)

root@novaos:nsh# hello.nsh            ← 直接输文件名 = 运行脚本
Hello from a .nsh script!
Hello, world!
Press a key

root@novaos:nsh# run paint.1.nxp      ← 在槽1 拉起第二个进程
pid 2
root@novaos:nsh# run snake.2.nxp      ← 槽2 再来一条蛇，三个进程并发
pid 3
root@novaos:nsh# procs                ← 画板、贪吃蛇和 shell 同屏轮转
1 r nsh.nxp
2 r paint.1.nxp
3 r snake.2.nxp

（按 F11 —— 全部冻结，回到内核 shell）
[proc] suspended - 'fg' resumes, 'kill' ends
root@novaos:/# procs
pid st name
1 s nsh.nxp
2 s paint.1.nxp
root@novaos:/# fsinfo                 ← 磁盘用量
NovaFS disk:
  blocks used : 45 / 32768 (32723 free, 512B each)
  space used  : 20130 B / 16 MB
  inodes used : 23 / 256
  ...
root@novaos:/# fg                     ← 解冻，回到两个进程
[proc] resumed
  （nsh 内）exit → paint 里 q → 全部退出
[proc] all processes exited
root@novaos:/#
```

注意！在Win7上很有可能出现“已停止运行”，不影响！