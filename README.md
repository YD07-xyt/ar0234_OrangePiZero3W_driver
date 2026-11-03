# ar0234_OrangePiZero3W_driver
ar0234相机的orangepi zero3w(a733) liunx驱动

## build
docs/build.md 是构建文档,由ai生成

### 1.clone

```sh
mdkir a733_ws \
cd ./a733_ws \
git clone https://github.com/YD07-xyt/ar0234_OrangePiZero3W_driver.git
```

### 2.准备 VIN 头文件

驱动依赖 VIN 的私有头文件（`camera.h`、`sensor_helper.h`、`vin_log.h` 等）和 `bsp/include`。headers 包里不一定带这些文件，从源码拷到板子上：

```sh
#a733_ws下,与ar0234_OrangePiZero3W_driver同级
git clone --branch <the branch you need> --depth 1 https://github.com/orangepi-xunlong/linux-orangepi.git
```

```sh
ssh orangepi@<your ip> 'mkdir -p ~/vin_hdr/vin ~/vin_hdr/bsp_include ~/ar0234'
rsync -a --include='*/' --include='*.h' --exclude='*' \
  linux-orangepi/bsp/drivers/vin/ orangepi@<your ip>:~/vin_hdr/vin/
rsync -a linux-orangepi/bsp/include/ orangepi@<your ip>:~/vin_hdr/bsp_include/
rsync -a ar0234_OrangePiZero3W_driver/ orangepi@<your ip>:~/ar0234/ --exclude docs
```

拷完在板子上检查，四个文件都要存在：

```sh
ls ~/vin_hdr/vin/utility/vin_log.h ~/vin_hdr/vin/modules/sensor/camera.h \
   ~/vin_hdr/bsp_include/sunxi-log.h ~/vin_hdr/bsp_include/media/sunxi_camera_v2.h
```

如果板子上 `/lib/modules/$(uname -r)/build/bsp/drivers/vin/modules/sensor/camera.h` 已经存在，可以跳过前两条，Makefile 默认就用那里。

### 3.编译

```sh
make VIN_DIR=$HOME/vin_hdr/vin BSP_INC=$HOME/vin_hdr/bsp_include # 2 lane
```


### 4. 改设备树

只改设备树也不需要重编镜像。改的是 `/boot` 下 U-Boot 加载的 dtb 文件


**不能用 overlay**：overlay 只能新增或覆盖属性，不能删除 `sensor0_pwdn`。所以直接改 dtb 文件。

参考[build文档的第3部分](./docs/build.md)

### 5.  AR0234 调参与取图

参考[tuning文档](./docs/tuning.md)