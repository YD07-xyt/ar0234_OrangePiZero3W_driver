# AR0234 调参与取图

板子：Orange Pi Zero 3W（A733），相机接 CAM1。驱动：`ar0234_mipi.ko`。编译和安装见 [build.md](build.md)。

## 1. 能输出什么

| 项 | 取值 |
| --- | --- |
| 分辨率 | 1920×1200（驱动只做了这一种） |
| 格式 | RAW10 Bayer GRBG，V4L2 `BA10`；每像素 2 字节小端，低 10 位有效（0–1023）；一帧 4608000 字节 |
| 帧率 | 2 lane 60 fps；4 lane 120 fps（都已实测） |
| 彩色 | 是。没有深度（RGBD 需要另外的深度相机） |

ISP 没开（设备树 `isp_used=0`）。`/dev/video0` 上列出的 YUV/RGB 格式，以及 brightness/contrast/white_balance 等控件，都要经过 ISP，对 RAW 无效。去马赛克、白平衡、伽马要在主机上做（见第 5 节的脚本）。

## 2. lane 数（2 或 4）

lane 数在编译时决定，运行时改不了。

**板子上当前装的是 4 lane 版本**，默认 60 fps。旧的 2 lane 模块备份在 `extra/ar0234_mipi.ko.bak_2lane`。

| | 2 lane（`make` 的默认值） | 4 lane（当前安装） |
| --- | --- | --- |
| 编译 | `make` | `make LANES=4` |
| 像素时钟 | 45 MHz | 90 MHz |
| 最高帧率 | 60.22 fps（实测） | 120.44 fps（实测） |
| 1 行时间 | 612 / 45 MHz = 13.6 µs | 612 / 90 MHz = 6.8 µs |
| 实测丢帧 | 300 帧 lost 0、error 0 | 600 帧 lost 0、error 0 |

**注意：** 同样的 `exposure` 行数，4 lane 下的实际曝光时间只有 2 lane 的一半。驱动的默认曝光已经按 lane 数换算好了（2 lane 是 512 行，4 lane 是 1024 行，都约 7 ms），但自己设 `exposure` 时要按时间来算行数。

**加载时的默认帧率**由模块参数 `fps` 决定，默认 60，2 lane 和 4 lane 都适用。参数大于等于最高帧率时按最高帧率运行：

```sh
sudo rmmod vin_v4l2 ar0234_mipi
sudo modprobe ar0234_mipi fps=120      # 4 lane 跑满 120 fps；也可以设 30、15 等
sudo modprobe vin_v4l2
cat /sys/module/ar0234_mipi/parameters/fps
```

想开机就用别的帧率，可以写一行 `options ar0234_mipi fps=120` 到 `/etc/modprobe.d/ar0234.conf`。运行时仍然可以用 `vertical_blanking` 改帧率。

**切换方法**（编译时 `VIN_DIR`、`BSP_INC` 的路径见 build.md 第 1 节）：

```sh
cd ~/ar0234
make clean && make LANES=4 VIN_DIR=$HOME/vin_hdr/vin BSP_INC=$HOME/vin_hdr/bsp_include
# 临时测试，不装进系统：
sudo rmmod vin_v4l2 ar0234_mipi; sudo insmod ./ar0234_mipi.ko; sudo modprobe vin_v4l2
sudo dmesg | grep "lanes ="          # 开一次流后能看到 lanes = 4
# 确认没问题后再装（开机自动加载用的就是这个）：
sudo cp ar0234_mipi.ko /lib/modules/$(uname -r)/extra/ && sudo depmod -a
```

**回退到 2 lane：** 在 `~/ar0234` 下执行 `make clean && make VIN_DIR=... BSP_INC=...`，然后安装。也可以直接用备份：`sudo cp /lib/modules/$(uname -r)/extra/ar0234_mipi.ko.bak_2lane /lib/modules/$(uname -r)/extra/ar0234_mipi.ko && sudo depmod -a`。注意这个备份是加 `fps` 参数之前的版本，没有 `fps` 参数。临时测试时，`rmmod` 之后再 `modprobe ar0234_mipi`，加载的就是 `extra/` 里已经装好的那个版本。

**丢帧检查：** 下面的命令要在不写盘的情况下测。如果同时用 `--stream-to` 写 SD 卡，120 fps 时 SD 卡写不过来，会出现 lost_cnt，这不是链路的问题。

```sh
v4l2-ctl -d /dev/video0 -i 0 --set-fmt-video=width=1920,height=1200,pixelformat=BA10 \
  --stream-mmap --stream-count=600 >/dev/null
sudo cat /sys/kernel/debug/mpp/vi | sed -n "/vi0:/,/CSI Band/p"   # 看 frame => cnt / lost_cnt / error_cnt
```

## 3. 曝光、增益、帧率

控件挂在传感器子设备上，不在 `/dev/video0` 上。子设备的编号可能会变，先找到它：

```sh
S=/dev/$(basename $(grep -l ar0234 /sys/class/video4linux/v4l-subdev*/name | xargs dirname))
v4l2-ctl -d $S -l          # 列出控件和当前值
```

| 控件 | 单位 | 范围 | 默认 | 对应寄存器 |
| --- | --- | --- | --- | --- |
| `exposure` | 行 × 16 | 32 … 1048464（2 … 65529 行） | 约 7 ms：2 lane 8192（512 行），4 lane 16384（1024 行） | R0x3012 |
| `gain` | 16 = 1× | 16 … 256（1× … 16×，模拟增益） | 28（约 1.75×） | R0x3060 + R0x30BA |
| `vertical_blanking` | 行 | 16 … 64335 | 由 `fps` 参数决定：fps=60 时，2 lane 为 16，4 lane 为 1232 | R0x300A = 1200 + vblank |

- 开流前或开流中都能设置，下次开流时保持。
- 驱动重新加载后恢复默认值。
- 设备开着时（流还在跑）可以直接改，大约 1–2 帧后生效。

### 3.1 换算公式

```
行时间 t_line = 612 / pclk                  2 lane: 13.6 µs   4 lane: 6.8 µs
曝光时间       = 曝光行数 × t_line
帧长 (行)      = max(1200 + vblank, 曝光行数 + 5)
帧率           = pclk / (612 × 帧长)
exposure 控件值 = 曝光行数 × 16
```

### 3.2 常用设置

帧率（曝光不超过一帧长度时）：

| 目标 fps | 2 lane vblank | 2 lane 实测 | 4 lane vblank | 4 lane 实测 |
| --- | --- | --- | --- | --- |
| 120 | — | — | 16 | 120.44 |
| 60 | 16 | 60.22 | 1232 | 60.34 |
| 30 | 1232 | 30.17 | 3664 | 30.20 |
| 15 | 3664 | 15.10 | 8528 | 未测（按公式计算） |

曝光时间换成控件值：

| 曝光时间 | 2 lane 行数 → 控件值 | 4 lane 行数 → 控件值 |
| --- | --- | --- |
| 1 ms | 74 → 1184 | 147 → 2352 |
| 5 ms | 368 → 5888 | 735 → 11760 |
| 10 ms | 735 → 11760 | 1471 → 23536 |
| 16 ms | 1176 → 18816 | 2353 → 37648 |

如果曝光行数加 5 超过了帧长，驱动会自动拉长帧，帧率就会下降。例如 2 lane 下曝光 2400 行，实测帧率掉到 30.5 fps。要保持 60 fps，2 lane 的曝光不能超过 1211 行（约 16.5 ms）；要保持 120 fps，4 lane 也不能超过 1211 行（约 8.2 ms）。

### 3.3 调参顺序

1. **先定帧率**，用 `vertical_blanking`。
2. **再调曝光**：在不降帧的前提下尽量拉长曝光。运动画面要防拖影，就把曝光缩短到 1–5 ms。
3. **最后加增益**：曝光已经到上限还嫌暗时再加。增益会同时放大噪声。AND9820 建议增益 ≥ 1.684×（控件值 ≥ 27），低于这个值暗部噪声可能变差，这一点没有实测过。
4. **看统计值**：用 `ar0234_view.py stats` 看 mean 和 sat（饱和像素比例），一般让 sat < 1%、mean 在 150–500 之间。

实测（2 lane，同一个室内场景，平均码值）：

| 设置 | mean |
| --- | --- |
| 默认（512 行，28） | 111.8 |
| 200 行 | 76.5 |
| 1100 行 | 172.3 |
| 1100 行 + gain 64（4×） | 297.8 |

### 3.4 示例

```sh
# 4 lane，120 fps，曝光 5 ms，增益 2×
v4l2-ctl -d $S -c vertical_blanking=16 -c exposure=11760 -c gain=32

# 2 lane，30 fps，曝光 30 ms（2206 行，不超过 30 fps 的帧长 2432 行）
v4l2-ctl -d $S -c vertical_blanking=1232 -c exposure=$((2206*16)) -c gain=28

# 恢复默认（4 lane，60 fps）
v4l2-ctl -d $S -c vertical_blanking=1232 -c exposure=16384 -c gain=28
```

### 3.5 测试图样

用来排查链路问题，判断传感器到 SoC 这一段有没有错位、丢 lane：

```sh
sudo rmmod vin_v4l2 ar0234_mipi
sudo modprobe ar0234_mipi test_pattern=2   # 0 关  1 纯色  2 彩条  3 渐变灰  256 walking 1s
sudo modprobe vin_v4l2
```

用完再执行一次 `rmmod` 和 `modprobe ar0234_mipi`（不带参数），恢复正常图像。

### 3.6 还不支持的

hflip/vflip、1080p/720p 裁剪、2×2 binning、数字增益、RAW8 输出。需要的话要改驱动。

## 4. 在板子上取图

每条命令都必须带 `-i 0`。不带的话设置格式会失败，报 `Invalid argument`，内核还会打 WARN。

```sh
F="-d /dev/video0 -i 0 --set-fmt-video=width=1920,height=1200,pixelformat=BA10 --stream-mmap"

# 单帧（跳过前 10 帧，等曝光稳定）
v4l2-ctl $F --stream-skip=10 --stream-count=1 --stream-to=ar0234.raw     # 4608000 字节

# 连续 N 帧写到一个文件里（写 /tmp 或 SSD；120 fps 时 SD 卡写不过来会丢帧）
v4l2-ctl $F --stream-skip=10 --stream-count=100 --stream-to=/tmp/seq.raw

# 只测帧率，不存盘
v4l2-ctl $F --stream-count=600
```

文件格式：裸帧依次拼接，没有文件头，每帧 1920×1200×2 字节。

## 5. 在主机上看图

脚本是 [tools/ar0234_view.py](../tools/ar0234_view.py)，在主机上运行，依赖 `python3-numpy` 和 `python3-opencv`。

### 5.1 转 PNG

```sh
scp orangepi@192.168.64.10:~/ar0234.raw .
python3 tools/ar0234_view.py stats ar0234.raw        # mean / median / p99 / 饱和比例
python3 tools/ar0234_view.py png ar0234.raw --wb     # -> ar0234_000.png，--wb 灰度世界白平衡
```

### 5.2 快速实时预览（推荐，约 15–18 fps）

板子上先把图像缩成 960×600、8 位 RGB，再用不加密的 TCP 发给主机。每帧只有 1.7 MB，原始帧是 4.6 MB。

板子端脚本是 [tools/ar0234_board_stream.py](../tools/ar0234_board_stream.py)，需要 `python3-numpy`（已装）。先把它拷到板子上：`scp tools/ar0234_board_stream.py orangepi@192.168.64.10:~/`。

```sh
# 1) 主机先开监听
nc -l 5000 | python3 tools/ar0234_view.py live --fmt rgb8 --size 960x600 --wb --stats

# 2) 板子上发送（可以用 ssh 远程启动；视频走 nc，不经过 SSH 加密）
ssh orangepi@192.168.64.10 'v4l2-ctl -d /dev/video0 -i 0 \
  --set-fmt-video=width=1920,height=1200,pixelformat=BA10 \
  --stream-mmap --stream-count=0 --stream-to=- 2>/dev/null \
  | python3 ~/ar0234_board_stream.py | nc 192.168.64.66 5000'
```

- 主机的 IP 是 192.168.64.66。换网络后要改成新地址，主机防火墙要放行 5000 端口。
- `--bin 2` 会输出 480×300，主机端对应加 `--size 480x300`。
- 板子端的转换是隔点取样（每个 2×2 Bayer 单元取一个 R、一个 G、一个 B），没有插值，所以边缘会比全分辨率稍硬。这个画面只适合预览和调参，不能用作最终图像。
- 主机端会扣黑电平，并做灰度世界白平衡和 gamma。

### 5.3 全分辨率 RAW 预览（慢，约 2–4 fps）

```sh
ssh orangepi@192.168.64.10 'v4l2-ctl -d /dev/video0 -i 0 \
  --set-fmt-video=width=1920,height=1200,pixelformat=BA10 \
  --stream-mmap --stream-count=0 --stream-to=- 2>/dev/null' \
  | python3 tools/ar0234_view.py live --wb --stats
```

- 两种预览都可以按 `q` 或 `Esc` 退出，按 `s` 保存当前帧（RAW 预览时同时保存 PNG 和 RAW）。
- `--stats` 会在画面上显示实际 fps、mean 和饱和比例。可以另开一个终端，用第 3 节的命令边改边看。
- `-i 0` 会往 stdout 打印一行 `Video input set to 0 ...`，两个脚本都会自动跳过这一行。

### 5.4 为什么慢（实测）

| 环节 | 实测 |
| --- | --- |
| WiFi 链路速率 | 板子 600 Mbit/s，主机 1200 Mbit/s |
| SSH 传输（默认加密） | 4–9 MB/s，原始帧 1–2 fps |
| 普通 TCP（nc） | 约 16–30 MB/s，波动大 |
| 板子上 `v4l2-ctl` 往管道输出 | 约 57 fps（300 帧 5.2 s） |
| 板子上 numpy 缩图 | 每帧约 14 ms |
| 端到端（板子缩图 + nc + 主机显示） | 17.8 fps |

瓶颈主要有两处：
- **SSH 加密：** 板子 CPU 加密跟不上，吞吐只有 4–9 MB/s。
- **WiFi 实际吞吐：** 链路速率很高，但实际传输远达不到。

主机端每帧处理约 23 ms，不是瓶颈。要更快，可以：
- 换有线网口；
- 在板子上用 jpegenc 压成 JPEG 再传。板子上有 GStreamer 的 jpegenc，这个方案还没做。

### 5.5 画面发灰（雾感）的原因

传感器输出带有约 42 码的黑电平（R0x301E = 0x002A，板上读回），所以全黑像素读出来不是 0，而是 42 左右。旧版脚本没有扣掉这部分，经过 gamma 之后，最暗的像素也有 56/255，整个画面就像蒙了一层雾。现在两个脚本都默认扣除 42（可以用 `--black` 调整），最暗的像素回到 0。

如果扣了黑电平还是觉得糊，可以按下面顺序排查：
1. **对焦：** 镜头是手动对焦的，转动镜头调一下。
2. **运动模糊：** 曝光太长，把 `exposure` 调短。
3. **预览缩小了：** 5.2 节的快速预览只有一半分辨率。要看清细节，用 5.1 节抓全分辨率 RAW 转 PNG 再看。

### 5.6 颜色

V4L2 的 GRBG 对应 OpenCV 的 `COLOR_BayerGB2BGR`，不是 `BayerGR`。我用人工构造的图样验证过：OpenCV 的命名是从第 2 行第 2 列开始数的。脚本里已经用的是正确的参数。

## 6. 排查

| 现象 | 原因 / 处理 |
| --- | --- |
| `read chip id failed, ret = -22` | I2C 没有应答。先确认排线插在 **CAM1**：用 `sudo i2ctransfer -y 11 w2@0x10 0x30 0x00 r2` 测 TWI11，应该返回 `0x0a 0x56`。如果是 `-y 9` 能读到，说明插在 CAM2 上了 |
| 设置格式报 `Invalid argument` | 命令里没加 `-i 0` |
| 改了控件画面没变化 | 改到了 `/dev/video0` 上，应该改传感器子设备 `$S`；或者加载的还是旧驱动，用 `modinfo -n ar0234_mipi` 看路径，用 `v4l2-ctl -d $S -l` 看有没有 `vertical_blanking` 控件 |
| 有 lost_cnt | 先排除写盘速度的影响，按第 2 节不写盘再测一次 |
| `/dev/v4l-subdevN` 编号变了 | 按第 3 节用 name 去查 |
| `scaler get_selection error`、`video0 has already stream off`、`imx219_2 ... No such device` | VIN 框架自己打的日志，或者 CAM2 上没接设备，不影响出图 |

调试的时候，开流过程中可以直接读寄存器：`sudo i2ctransfer -f -y 11 w2@0x10 0x30 0x12 r2`（读曝光）。`-f` 表示强制访问已经被驱动占用的地址。尽量不要直接写寄存器，写进去的值和驱动里保存的状态会不一致。
