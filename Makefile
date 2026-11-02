# Out-of-tree build of ar0234_mipi.ko for Allwinner VIN (A733, kernel 6.6).
#
#   make                      # 2 lane (bring-up)
#   make LANES=4              # 4 lane
#   make KDIR=... VIN_DIR=... BSP_INC=...
#
# KDIR    : kernel build tree (on board: /lib/modules/$(uname -r)/build)
# VIN_DIR : copy of linux-orangepi/bsp/drivers/vin (only headers needed)
# BSP_INC : copy of linux-orangepi/bsp/include (sunxi-log.h, media/sunxi_camera_v2.h)

obj-m := ar0234_mipi.o

LANES   ?= 2
KDIR    ?= /lib/modules/$(shell uname -r)/build
VIN_DIR ?= $(KDIR)/bsp/drivers/vin
BSP_INC ?= $(KDIR)/bsp/include

ccflags-y += -I$(VIN_DIR) -I$(VIN_DIR)/modules/sensor -I$(BSP_INC)
ccflags-y += -DAR0234_LANES=$(LANES)

all:
	@test -f $(VIN_DIR)/utility/vin_log.h || { echo "missing $(VIN_DIR)/utility/vin_log.h, copy bsp/drivers/vin headers first (docs/build.md 1)"; exit 1; }
	@test -f $(BSP_INC)/sunxi-log.h || { echo "missing $(BSP_INC)/sunxi-log.h, copy bsp/include first (docs/build.md 1)"; exit 1; }
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
