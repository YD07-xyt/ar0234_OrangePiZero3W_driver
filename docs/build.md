# ar0234_mipi.ko 编译与上板

在已烧录的系统上直接加模块，不重编镜像。板子内核 6.6，`linux-headers-current-sun60iw2` 已安装。

## 1. 准备 VIN 头文件

驱动依赖 VIN 的私有头文件（`camera.h`、`sensor_helper.h`、`vin_log.h` 等）和 `bsp/include`。headers 包里不一定带这些文件，从源码拷到板子上：

```sh
# 主机上，在 a733 目录执行（不是板子上）
ssh orangepi@192.168.64.10 'mkdir -p ~/vin_hdr/vin ~/vin_hdr/bsp_include ~/ar0234'
rsync -a --include='*/' --include='*.h' --exclude='*' \
  linux-orangepi/bsp/drivers/vin/ orangepi@192.168.64.10:~/vin_hdr/vin/
rsync -a linux-orangepi/bsp/include/ orangepi@192.168.64.10:~/vin_hdr/bsp_include/
rsync -a ar0234_OrangePiZero3W_driver/ orangepi@192.168.64.10:~/ar0234/ --exclude docs
```

拷完在板子上检查，四个文件都要存在：

```sh
ls ~/vin_hdr/vin/utility/vin_log.h ~/vin_hdr/vin/modules/sensor/camera.h \
   ~/vin_hdr/bsp_include/sunxi-log.h ~/vin_hdr/bsp_include/media/sunxi_camera_v2.h
```

如果板子上 `/lib/modules/$(uname -r)/build/bsp/drivers/vin/modules/sensor/camera.h` 已经存在，可以跳过前两条，Makefile 默认就用那里。

## 2. 编译（板子上）

```sh
cd ~/ar0234
make VIN_DIR=$HOME/vin_hdr/vin BSP_INC=$HOME/vin_hdr/bsp_include          # 2 lane
# 出图后：make clean && make LANES=4 VIN_DIR=... BSP_INC=...
```

产物 `ar0234_mipi.ko`。用 `modinfo ar0234_mipi.ko | grep vermagic` 确认和 `uname -r` 一致。

如果报 `Unknown symbol`（`sensor_write_array`、`cci_dev_probe_helper` 等），说明 `vin_io.ko` 没先加载（这些符号都由 `vin_io` 导出），先 `sudo modprobe vin_io`。编译时出现 `WARNING: ... undefined!`，查 `grep cci_dev_probe_helper /lib/modules/$(uname -r)/build/Module.symvers`，没有就把板上 `/lib/modules/$(uname -r)/build/Module.symvers` 换成内核构建产物里的那份。

## 3. 设备树

需要改 `sensor0`（CAM1），见 [驱动构建思路.md](<../../docs/驱动构建思路.md>) 第 4 节：

```dts
sensor0_mname = "ar0234";
sensor0_twi_addr = <0x20>;
sensor0_isp_used = <0>;       /* 首次出图先抓 RAW */
/* sensor0_pwdn、sensor0_reset 删除或注释掉，模组没有这两个脚 */
```

只改设备树也不需要重编镜像。改的是 `/boot` 下 U-Boot 加载的 dtb 文件，下面的命令都在板子上执行。

**不能用 overlay**：overlay 只能新增或覆盖属性，不能删除 `sensor0_pwdn`。所以直接改 dtb 文件。

### 3.1 找到当前 dtb 和 sensor0 节点路径

```sh
sudo apt install -y device-tree-compiler        # 提供 dtc / fdtget / fdtput

grep fdtfile /boot/orangepiEnv.txt              # 有就用它；没有看 boot.cmd 里的默认值
ls /boot/dtb/allwinner/ | grep -i zero3w
DTB=/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb   # 按上面结果改

# 节点完整路径（fdtput 要用）。dtc 反编译输出里的 "sensor0 = ..." 来自 __symbols__（标签表），不是 /aliases
NODE=/soc@3000000/vind@5800800/sensor@5812000
fdtget "$DTB" /__symbols__ sensor0              # 应输出同一路径
fdtget -p "$DTB" "$NODE"                        # 列出该节点所有属性
fdtget "$DTB" "$NODE" sensor0_mname             # 改之前应为 imx219
```

确认系统运行时确实用的是这个 dtb（改了文件但启动用的是别的 dtb，改动不会生效）：

```sh
ls -ld /boot/dtb                                       # 指向 dtb-<内核版本> 的软链接，正常
ls /proc/device-tree/soc@3000000/vind@5800800/ | grep sensor
cat /proc/device-tree$NODE/sensor0_mname; echo         # 应为 imx219，与文件一致
```

如果最后两条没有输出，说明运行中的设备树不是 `$DTB`，先查清楚再改。

### 3.2 备份

```sh
sudo cp -n "$DTB" "$DTB.imx219.bak"
```

`-n` 不覆盖已有备份，多次执行也安全。

### 3.3 修改（方法一，推荐：fdtput 原地改）

```sh
sudo fdtput -t s "$DTB" "$NODE" sensor0_mname ar0234
sudo fdtput -t i "$DTB" "$NODE" sensor0_twi_addr 0x20
sudo fdtput -t i "$DTB" "$NODE" sensor0_isp_used 0
sudo fdtput -d   "$DTB" "$NODE" sensor0_pwdn
fdtget -p "$DTB" "$NODE" | grep -q sensor0_reset && sudo fdtput -d "$DTB" "$NODE" sensor0_reset
```

源码里 `sensor0_reset` 是注释掉的，一般不存在，所以最后一行先判断再删除。

### 3.4 修改（方法二：dtc 反编译后手改）

```sh
dtc -I dtb -O dts -o /tmp/zero3w.dts "$DTB" 2>/dev/null
nano /tmp/zero3w.dts        # 搜 sensor@5812000，按上面的 dts 片段改，删掉 sensor0_pwdn 一行
dtc -I dts -O dtb -o /tmp/zero3w.dtb /tmp/zero3w.dts
sudo cp /tmp/zero3w.dtb "$DTB"
```

反编译后的 phandle 会变成数字，不影响使用。

### 3.5 检查并重启

```sh
fdtget "$DTB" "$NODE" sensor0_mname        # ar0234
fdtget "$DTB" "$NODE" sensor0_isp_used     # 0
fdtget "$DTB" "$NODE" sensor0_pwdn         # 应报错 FDT_ERR_NOTFOUND
sudo reboot
```

重启后确认生效：

```sh
NODE=/soc@3000000/vind@5800800/sensor@5812000    # 重启后变量会丢，重新设
cat "/proc/device-tree$NODE/sensor0_mname"; echo     # ar0234
ls "/proc/device-tree$NODE/" | grep pwdn             # 无输出
```

### 3.6 恢复

起不来或要换回 imx219 时：

```sh
sudo cp "$DTB.imx219.bak" "$DTB" && sudo reboot
```

如果系统完全起不来，把 TF 卡插到电脑上，在 `boot` 分区的 `dtb/allwinner/` 里用备份覆盖原文件。

双摄时 CAM2 改 `sensor@5812020`（`sensor2`）节点，方法相同：`sensor2_mname` 设为 `ar0234_2`，删除 `sensor2_pwdn`（PE10）。

## 4. 加载

VIN 按 `sensor0_mname` 调 `v4l2_i2c_new_subdev()`，内核会 `request_module("i2c:ar0234")` 自动加载同名模块。所以推荐先把模块装进系统并 `depmod`，重启后自动加载：

```sh
sudo mkdir -p /lib/modules/$(uname -r)/extra
sudo cp ar0234_mipi.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a
modinfo ar0234_mipi | grep alias        # 应有 i2c:ar0234 和 i2c:ar0234_2
```

手动加载（不重启调试时），sensor 模块要在 `vin_v4l2` 之前加载：

```sh
sudo modprobe -r vin_v4l2 imx219 2>/dev/null
sudo modprobe vin_io                       # 提供 cci/sensor_helper 符号
sudo insmod ./ar0234_mipi.ko               # 测试图：insmod ./ar0234_mipi.ko test_pattern=2
sudo modprobe vin_v4l2
dmesg | grep -i -E "ar0234|vin|cci" | tail -40
```

成功标志：`find AR0234, chip id 0x0a56`。

重新编译后要再执行一次上面的 `cp` 和 `depmod -a`，否则加载的还是旧模块。

## 5. 抓图

必须先 `-i 0`（VIDIOC_S_INPUT）再设格式，否则 `vin_pipeline_try_format` 报 WARN、S_FMT 返回 Invalid argument。

```sh
v4l2-ctl -d /dev/video0 -i 0 --set-fmt-video=width=1920,height=1200,pixelformat=BA10 \
  --stream-mmap --stream-skip=10 --stream-count=1 --stream-to=ar0234.raw   # 4608000 字节
v4l2-ctl -d /dev/video0 -i 0 --set-fmt-video=width=1920,height=1200,pixelformat=BA10 \
  --stream-mmap --stream-count=300                                         # 看实测 fps
```

实测：2 lane 60.22 fps，4 lane（`make LANES=4`）120.44 fps。调参、取图、主机预览见 [tuning.md](tuning.md)。

ISP 未启用（`isp_used=0`），只有 RAW10（`BA10`，GRBG）可用；`--list-formats` 里的 YUV/RGB 和 `/dev/video0` 上的 brightness/contrast/white_balance 等控件走 ISP，对 RAW 无效。

## 6. 曝光 / 增益 / 帧率

控件在传感器子设备上（名字为 `ar0234` 的 `/dev/v4l-subdev*`），开流前或开流中都能改，下次开流保持：

```sh
S=/dev/$(basename $(grep -l ar0234 /sys/class/video4linux/v4l-subdev*/name | xargs dirname))
v4l2-ctl -d $S -l
v4l2-ctl -d $S -c exposure=$((1100*16))      # 单位：行 ×16；1 行 = 612/45 MHz ≈ 13.6 µs（2 lane）
v4l2-ctl -d $S -c gain=64                     # 16 = 1×，256 = 16× 模拟增益
v4l2-ctl -d $S -c vertical_blanking=1232      # 帧长 = 1200 + vblank 行
```

| 控件 | 范围 | 默认 | 说明 |
| --- | --- | --- | --- |
| `exposure` | 32 … 1048464（2 … 65529 行） | 8192（512 行 ≈ 7 ms） | 曝光行数 + 5 超过帧长时自动拉长帧，帧率下降 |
| `gain` | 16 … 256 | 28（≈1.75×） | AND9820 建议 ≥ 1.684× |
| `vertical_blanking` | 16 … 64335 | 16 | fps = 45 MHz / (612 × (1200 + vblank))（2 lane） |

常用帧率（2 lane）：

| fps | vertical_blanking | 实测 |
| --- | --- | --- |
| 60 | 16 | 60.22 |
| 30 | 1232 | 30.17 |
| 15 | 3664 | 15.10 |

## 驱动参数

| 项 | 取值 | 来源 |
| --- | --- | --- |
| I2C | 0x20（7 位 0x10），16 位地址 / 16 位数据 | 数据手册 |
| 芯片 ID | R0x3000 = 0x0A56（黑白版 0x1A56） | rpi 驱动 |
| 时钟 | 模组 24 MHz 晶振，PLL 按 24 MHz → 450 MHz 链路，RAW10 | rpi `ar0234_pll_config_24_450_10bit` |
| 格式 | `MEDIA_BUS_FMT_SGRBG10_1X10`，1920×1200 | rpi 驱动 |
| hts / vts | 612 / 1216 | rpi `LINE_LENGTH_PCK_DEF`、`VBLANK_MIN` |
| pclk | 2 lane 45 MHz，4 lane 90 MHz | rpi `ar0234_freq_pixclk` |
| mipi_bps | 900 Mbps/lane | 450 MHz DDR |
| 复位 | 软件复位 R0x301A：0x0001 → 200 ms → 0x2058 | rpi `ar0234_soft_reset` |
| 出流 / 停流 | R0x301A = 0x205C / 0x2058 | 数据手册 R0x301A[2] |
| 增益 | VIN Q4 → R0x3060（粗 2^s × 细），同步写 R0x30BA | AND9820 式 43–46、表 24 |
