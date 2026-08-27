# NovaOS

一个简洁的 32 位 C 语言操作系统内核，带 **多用户登录**（开机认证 + 密码哈希持久化）、**Ring3 用户态**（分页 + TSS + IDT + int 0x80 syscall）、交互式 shell `NovaSh`、**NovaFS**（256 inode / 多级目录 / 递归删除）、**运行时 ACPI/AML 解析关机**、**Bochs VBE 图形驱动（1024x768x32 真彩控制台）**、**Windows BSOD 风格红屏崩溃页** 与美化启动画面，可用 QEMU 直接启动测试。（建议使用 QEMU 6.2，支持最好）

## 目录结构

```
NovaOS/
├── boot/
│   ├── stage1.asm          # MBR 引导扇区 (512B, LBA 加载；内核分两段读避开 64K 边界)
│   ├── stage2.asm          # Stage2: A20/GDT/进入保护模式/拷贝内核/跳转
│   └── kernel_entry.inc    # 内核布局常量 (加载地址 0x100000)
├── kernel/
│   ├── entry.asm           # 32位内核入口 stub (设栈/段, 调用 kmain)
│   ├── kernel.c            # 内核主体: VGA/键盘/串口/RTC + 登录/多用户 + NovaSh shell
│   ├── ata.c / ata.h       # 磁盘 PIO ATA 驱动 (LBA 读写 512B 扇区)
│   ├── novafs.c / novafs.h   # NovaFS 文件系统: 多级目录 + 递归删除
│   ├── acpi.c / acpi.h     # 最小 ACPI/AML 解析器 (RSDP→FADT→DSDT, 解析 \_S5 关机)
│   ├── gfx.c / gfx.h       # Bochs VBE 图形驱动: 1024x768x32 真彩控制台 (128x48)
│   ├── stdint.h            # 最小 stdint
│   └── link.ld             # 链接脚本 (段紧凑于 0x100000)
├── build.ps1  build.bat    # 构建脚本 (NASM + gcc + ld + objcopy -> disk.img)
├── run.ps1    run.bat      # QEMU 启动脚本 (isa-debug-exit + 串口日志)
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
- `disk.img` —— 可启动的 1 MiB 原始磁盘镜像

磁盘布局（LBA，512 B / sector）：

| LBA 范围     | 内容                    |
|--------------|-------------------------|
| 0            | Stage1 (MBR)            |
| 1 – 2        | Stage2                  |
| 3 – 128      | Kernel（预算 126 扇区） |
| 129          | NovaFS superblock        |
| 130 – 161    | Inode 表 (256 × 64 B)   |
| 162 – 169    | Block 位图              |
| 170+         | 数据块                  |

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
4. 横幅 → 启动自检（cpu/video/com1/novafs/acpi/input/ring3）→ **登录提示** → NovaSh

提示：
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
- 改动会立即经 ATA 写回磁盘，重启不丢；`format` 会清空账户（root/nova 仍可登录）
- 用户名上限 23 字符（NovaFS 文件名限制）；密码上限 31 字符、不允许空密码
- `logout` 回到登录界面可换账户登录；目前所有用户权限等价（无 root 专属命令限制）
- 登录提示与输入同时回显到串口，方便无显示调试

## NovaSh 命令

### 账户

| 命令          | 说明                                        |
|---------------|---------------------------------------------|
| `whoami`      | 显示当前登录用户                            |
| `passwd`      | 改当前用户密码（验旧密码 → 新密码输两遍）   |
| `useradd 名`  | 创建新用户（提示设置密码）                  |
| `userdel 名`  | 删除用户（root 不可删）                     |
| `logout`      | 注销，返回登录界面                          |

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
| `fsinfo`   | NovaFS 概览 + 当前目录条目数          |
| `format`   | 强制重新格式化 NovaFS                 |

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
| `run  名`    | 执行 .nxp 程序                                           |
| `mkdemo`     | 生成示例程序 demo.nxp（然后 `run demo.nxp`）             |

注：单个文件大小上限 6 块 = 3072 B（直接块 NDIRECT=6）。

## Ring3 用户态

NovaOS 实现完整的 Ring 0 ↔ Ring 3 切换，所有 `.nxp` 用户程序都在受限的 Ring 3 下运行。
内核做了三件事：

1. **Identity Paging + 独立用户页**：页目录（PDE）0–8MB 全带 `US=1` 标志；0x400000–0x500000 范围（syscall trampoline + 用户栈）的 PTE 也设置 US，其余物理内存保持 Supervisor only。
2. **GDT / TSS / IDT**：GDT 追加 `CS_R3=0x1B`、`DS_R3=0x23`（DPL=3）；TSS 填好 `SS0=0x10 / ESP0`，Ring3 触发中断时 CPU 自动切回内核栈；32 个异常向量 + 0x80 syscall trap gate 全部由 entry.asm 宏生成。
3. **Syscall 跳板页（0x00400000）**：用户程序调用 API 时，`call [0x400Fxx]` 跳转到该页的 `int 0x80` stub；内核在进入用户程序前把 syscall stub + API 表拷贝到 trampoline。

Ring3 规则：
- 用户态 `DS/ES/FS/GS` 必须是 0x23（RPL=3），iretd 后由 kernel stub 预装载
- 用户态 CS/RPL=3 发生异常时，`fault_dispatch` 走 "user fault" 分支：打印 `eip/err/cr2` → `ring3_leave()` 杀程序 → 返回 shell
- 内核态异常：**触发 BSOD 红屏** + `cli/hlt` 永久停机（见下一节）

## .nxp 程序格式

- **文件**：4 字节魔数 `NXP\x01` + 平坦 32 位保护模式机器码
- **加载**：固定地址 `0x00300000`（MinGW `ld --image-base 0x00300000` 保证 .data/.bss 不越界），入口 = 加载地址 + 4；独立栈 0x00500000 向下，总大小上限 3 KB
- **Syscall**：通过 trampoline（0x400000）的 `int 0x80` 进行，ABI 与 cdecl 完全一致
- **入口约定**：`EAX` = 用户可见 API 表指针（0x00400F80，结构体见 kernel.c 的 `nxp_api_t`）：

```
+0x00 magic 'NXP1'   +0x04 version=1
+0x08 putc(c)        +0x0C puts(s)         +0x10 getchar()    +0x14 exit()
+0x18 scr_w          +0x1C scr_h           （无图形时为 0）
+0x20 pixel(x,y,rgb) +0x24 fill_rect(x,y,w,h,rgb) +0x28 text(x,y,s,rgb)
+0x2C getkey()       ← 非阻塞读键：无键返回 -1（交互程序用）
+0x30 mouse(&dx,&dy,&btns)  ← 非阻塞读鼠标：返回积累的包数（0=无输入），
                              输出自上次调用以来的位移增量（屏幕坐标 +y 向下）
                              与当前按键位；读后清零。按键：NXP_BTN_L/R/M = 1/2/4
+0x34 get_pixel(x,y) ← 读一个像素。做 XOR 软件光标用（画两次自动还原，
                      任何背景上都可见且不留痕迹），paint.nxp 的十字光标即此实现
```

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

用户态程序崩溃**不会**触发 BSOD，只会打印 `[nxp] user fault: <异常名> eip=… err=… cr2=…` 然后安全返回 shell（`ringbad.nxp` 就是测这个用的）。

- 所有回调为 cdecl；`ret` 或调用 `exit()` 均可安全返回 shell
- `.nxp` 程序统一由 `programs/nxp_entry.c` 提供入口包装：压参数→调 `nxp_main(argc, argv)`→调 `exit(ret)`
- `mkdemo` 生成的 demo.nxp 即按此 ABI 手写机器码，可作参考

### 宿主机开发工作流（programs\ 目录）

把 C 源文件放进 `programs\`（如 `hello.c`），运行 `build.ps1` 即自动：

1. 编译链接为 `.nxp`（`nxp_entry.c` 提供入口跳板，链接在最前；`nxp.h` 是 API 头）
2. 直接写入 disk.img 的 NovaFS 区域（宿主机生成超级块/inode/位图/数据块）
3. 开机后 `NovaFS mounted`，直接 `run 文件名.nxp` 执行

写法参考：
- `programs/hello.c` — 静态演示，putc/puts 输出
- `programs/ringok.c`  — **最小 Ring3 健全性测试**：打印 `[user] ring3 alive (ringok)` 然后 `exit(0x42)`；用它验证 Ring3 进入/返回正常
- `programs/ringbad.c` — **用户态故障测试**：故意 `*(volatile uint32_t*)0x100000 = 1` 写只读内核页，期待触发 "user fault: #PF …" 然后安全回 shell（**不触发 BSOD**）
- `programs/paint.c` — **交互式画板**：鼠标移动画笔，左键画、右键擦、方向键/WASD 移动、1-8 换色、c 清屏、q 退出（非阻塞 `getkey` + `mouse`）

单程序上限 3 KB。注意 MinGW 链接 `.nxp` 必须加 `--image-base 0x00300000 --section-alignment 16`：前者防止 .data/.bss 被 ld 放到 0x400000+ 覆盖 syscall trampoline，后者避免 PE 4K 对齐把程序撑到十几 KB。

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
| .nxp 用户程序       | 0x00300000    | Ring3 R/X  | 固定 image base，上限 3 KB                |
| Syscall Trampoline  | 0x00400000    | Ring3 R/X  | `int 0x80` stub + 0xF80 起的 API 表       |
| Ring3 用户栈顶      | 0x00500000    | Ring3 R/W  | 向下增长                                  |
| Identity 页目录/表  | 0x001FF000    | 内核 R/W   | 1 PDE + 2 PT（0–8MB，identity map）       |

## 键盘

- PS/2 scancode set 1
- `Shift` / `CapsLock` 正常切换大小写
- 退格键删除上一个字符（限制 128 字符命令行缓冲）
- 目前不支持 `Tab`、方向键、历史命令

## 鼠标

- PS/2 辅助口（8042 aux）轮询驱动：`0xA8` 开口、清配置位 0x20、`0xF6` 默认值、`0xF4` 开流式上报（校验 `0xFA` ACK）
- 键盘和鼠标共用端口 `0x60`，靠状态寄存器 bit5（aux 标志）区分字节归属，统一由 `ps2_drain()` 分发
- 3 字节包 `[flags dx dy]`，bit3 作包同步位，9 位符号扩展；PS/2 的 +y 向上已换算为屏幕 +y 向下
- 初始化期间的命令 ACK / 配置字节**不是**包数据，只消费不进包组装器（否则会错位一整个包）

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

注意！在Win7上很有可能出现“已停止运行”，不影响！