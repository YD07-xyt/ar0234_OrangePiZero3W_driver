// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AR0234 MIPI RAW sensor driver for Allwinner VIN (A733 / Orange Pi Zero 3W).
 *
 * VIN glue follows linux-orangepi bsp/drivers/vin/modules/sensor/imx219.c.
 * Register sequences come from 3rd/ar0234-rpi-driver/ar0234.c
 * (24 MHz EXTCLK, 450 MHz link, 10-bit).
 *
 * Module facts (docs/驱动构建思路.md section 2.2):
 *  - 24 MHz crystal on the module, board MCLK is not needed.
 *  - No RESET / PWDN pins: software reset through R0x301A.
 *  - I2C 8-bit address 0x20 (7-bit 0x10), 16-bit address / 16-bit data.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/videodev2.h>
#include <linux/clk.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mediabus.h>
#include <linux/io.h>
#include "utility/vin_log.h"
#include "camera.h"
#include "sensor_helper.h"

MODULE_DESCRIPTION("A low-level driver for AR0234 sensors (Allwinner VIN)");
MODULE_LICENSE("GPL");

/* MIPI lane count: 2 for bring-up, 4 after the image is stable (make LANES=4). */
#ifndef AR0234_LANES
#define AR0234_LANES 2
#endif
#if AR0234_LANES != 2 && AR0234_LANES != 4
#error "AR0234_LANES must be 2 or 4"
#endif

#define MCLK              (24*1000*1000)
#define V4L2_IDENT_SENSOR 0x0A56

#define I2C_ADDR      0x20
#define SENSOR_NUM    0x2
#define SENSOR_NAME   "ar0234"
#define SENSOR_NAME_2 "ar0234_2"

/* Registers */
#define AR0234_REG_CHIP_ID             0x3000
#define AR0234_REG_FRAME_LENGTH_LINES  0x300A
#define AR0234_REG_COARSE_INTEG_TIME   0x3012
#define AR0234_REG_RESET               0x301A
#define AR0234_REG_ANALOG_GAIN         0x3060
#define AR0234_REG_TEST_PATTERN_MODE   0x3070
#define AR0234_REG_MFR_30BA            0x30BA
#define AR0234_REG_SERIAL_FORMAT       0x31AE

#define AR0234_CHIP_ID      0x0A56
#define AR0234_CHIP_ID_MONO 0x1A56

#define AR0234_RESET_DEFAULT 0x2058
#define AR0234_RESET_STREAM  0x0004	/* R0x301A[2] stream */

#define AR0234_MFR_30BA_GAIN_BITS(v) (0x7620 | (v))

/*
 * Timing. line_length_pck keeps its reset default 612 (rpi
 * AR0234_LINE_LENGTH_PCK_DEF). frame_length_lines = 1200 + 21 - 5 = 1216
 * (rpi VBLANK_MIN / FLL_OVERHEAD).
 * fps = pclk / (hts * vts): 2 lane 45 MHz -> ~60 fps, 4 lane 90 MHz -> ~121 fps.
 */
#define AR0234_HTS          612
#define AR0234_VTS_1200     1216
#define AR0234_FLL_OVERHEAD 5
#define AR0234_EXP_MIN      2
#if AR0234_LANES == 2
#define AR0234_PCLK         (45 * 1000 * 1000)
#define SENSOR_FRAME_RATE   60
#else
#define AR0234_PCLK         (90 * 1000 * 1000)
#define SENSOR_FRAME_RATE   120
#endif
/* 450 MHz DDR link clock -> 900 Mbps per lane */
#define AR0234_MIPI_BPS     (900 * 1000 * 1000)

static int test_pattern;
module_param(test_pattern, int, 0644);
MODULE_PARM_DESC(test_pattern,
		 "R0x3070 test pattern: 0 off, 1 solid, 2 color bars, 3 fade to grey, 256 walking 1s");

static int ar0234_sensor_vts = AR0234_VTS_1200;

/* VBLANK control range: frame_length_lines = height + vblank */
#define AR0234_VBLANK_MIN (AR0234_VTS_1200 - 1200)
#define AR0234_VBLANK_MAX (0xFFFF - 1200)

/*
 * Default frame rate at load time (vertical_blanking can still change it).
 * 60 for both lane counts; 4 lane 120 fps needs fps=120.
 */
static int fps = 60;
module_param(fps, int, 0444);
MODULE_PARM_DESC(fps, "default frame rate (1..max), sets default vertical_blanking");

/* Default exposure ~7 ms (512 lines at 2 lane, same as the old table) */
#define AR0234_EXP_DEF_LINES (512 * (AR0234_PCLK / (45 * 1000 * 1000)))

static unsigned int ar0234_default_vblank(void)
{
	unsigned int vts;

	if (fps <= 0 || fps >= SENSOR_FRAME_RATE)
		return AR0234_VBLANK_MIN;
	/*
	 * Scale from the measured max rate (VTS 1216 -> 60.22 / 120.44 fps)
	 * instead of pclk/(hts*fps): the latter gave 59.90 fps for fps=60.
	 */
	vts = AR0234_VTS_1200 * SENSOR_FRAME_RATE / fps;
	if (vts > 0xFFFF)
		vts = 0xFFFF;
	return vts - 1200 > AR0234_VBLANK_MIN ? vts - 1200 : AR0234_VBLANK_MIN;
}

/*
 * Driver state wraps sensor_info (first member, so to_state() and the
 * kfree(to_state(sd)) in sensor_remove still work).
 */
struct ar0234_state {
	struct sensor_info info;
	unsigned int vblank;
	struct v4l2_ctrl *exp_ctrl;
	struct v4l2_ctrl *gain_ctrl;
	struct v4l2_ctrl *vblank_ctrl;
};

static struct ar0234_state *to_ar0234(struct sensor_info *info)
{
	return container_of(info, struct ar0234_state, info);
}

/*
 * Default registers: soft reset, PLL/MIPI (24 MHz in, 450 MHz link, 10-bit),
 * lane count, common init, pixclk-specific settings.
 */
static struct regval_list sensor_default_regs[] = {
	/* soft reset (rpi ar0234_soft_reset) */
	{AR0234_REG_RESET, 0x0001},
	{REG_DLY, 200},
	{AR0234_REG_RESET, AR0234_RESET_DEFAULT},
	{REG_DLY, 1},

	/* ar0234_pll_config_24_450_10bit */
	{0x302A, 0x0005},	/* vt_pix_clk_div */
	{0x302C, 0x0001},	/* vt_sys_clk_div */
	{0x302E, 0x0008},	/* pre_pll_clk_div */
	{0x3030, 0x0096},	/* pll_multiplier */
	{0x3036, 0x000A},	/* op_pix_clk_div */
	{0x3038, 0x0001},	/* op_sys_clk_div */
	{0x31B0, 0x0082},	/* frame_preamble */
	{0x31B2, 0x005C},	/* line_preamble */
	{0x31B4, 0x4248},	/* mipi_timing_0 */
	{0x31B6, 0x4258},	/* mipi_timing_1 */
	{0x31B8, 0x904B},	/* mipi_timing_2 */
	{0x31BA, 0x030B},	/* mipi_timing_3 */
	{0x31BC, 0x0D89},	/* mipi_timing_4 */
	{0x3354, 0x002B},	/* mipi_cntrl: RAW10 */
	{0x31AC, 0x0A0A},	/* data_format_bits: 10-bit in/out */

	/* serial_format: MIPI + lane count (rpi ar0234_enable_streams) */
	{AR0234_REG_SERIAL_FORMAT, 0x0200 | AR0234_LANES},

	/* common_init */
	{0x30B0, 0x0028},
	{0x306E, 0x9010},
	{0x3082, 0x0003},
	{0x31D0, 0x0000},
	{0x3088, 0x8050},
	{0x3086, 0x9237},
	{0x3096, 0x0280},
	{0x31E0, 0x0003},
	{0x3F4C, 0x121F},
	{0x3F4E, 0x121F},
	{0x3F50, 0x0B81},
	{0x3ED2, 0xFA96},
	{0x3180, 0x824F},
	{0x3ECC, 0x0C42},
	{0x3ECC, 0x0C42},
	{0x30F0, 0x2283},
	{0x3102, 0x5000},
	{0x30B4, 0x0011},
	{0x3064, 0x1982},

#if AR0234_LANES == 2
	/* pixclk_45mhz_mfr_settings + R0x30BA for 45 MHz (rpi ar0234_pixclk_config) */
	{0x3088, 0x81BA},
	{0x3086, 0x3D02},
	{AR0234_REG_MFR_30BA, AR0234_MFR_30BA_GAIN_BITS(6)},
#endif
};

/* 1920x1200 full array (rpi ar0234_1920x1200_config) */
static struct regval_list sensor_1920x1200_regs[] = {
	{0x3002, 0x0008},	/* y_addr_start */
	{0x3004, 0x0008},	/* x_addr_start */
	{0x3006, 0x04B7},	/* y_addr_end */
	{0x3008, 0x0787},	/* x_addr_end */
	{0x30A2, 0x0001},	/* x_odd_inc */
	{0x30A6, 0x0001},	/* y_odd_inc */
	{AR0234_REG_FRAME_LENGTH_LINES, AR0234_VTS_1200},
	{AR0234_REG_COARSE_INTEG_TIME, 0x0200},
	{AR0234_REG_ANALOG_GAIN, 0x000E},	/* 1.78x, AND9820 recommends >= 1.684x */
};

static struct regval_list sensor_fmt_raw[] = {
};

/* R0x30BA lookup, same as rpi ar0234_set_analog_gain() */
static u16 ar0234_mfr_30ba(u16 ana)
{
#if AR0234_LANES == 2
	return AR0234_MFR_30BA_GAIN_BITS(ana < 0x36 ? 6 : 0);
#else
	if (ana < 0x20)
		return AR0234_MFR_30BA_GAIN_BITS(2);
	if (ana < 0x3A)
		return AR0234_MFR_30BA_GAIN_BITS(1);
	return AR0234_MFR_30BA_GAIN_BITS(0);
#endif
}

/*
 * VIN gain is Q4 (16 = 1x). Convert to R0x3060 (AND9820 eq. 43-46,
 * Table 24): coarse s gives 2^s; fine t gives 32/(32-t) for s = 0, 2 and
 * 16/(16-t/2) for s = 1, 3 (even t only); s = 4 is 16x only.
 */
static u16 ar0234_gain_to_reg(int gain_val)
{
	int s, base, t;

	if (gain_val < 16)
		gain_val = 16;
	if (gain_val >= 16 * 16)
		return 0x40;

	for (s = 0, base = 1; s < 3 && gain_val >= 16 * base * 2; s++)
		base *= 2;

	if (s == 0 || s == 2) {
		t = 32 - (32 * 16 * base) / gain_val;
		t = clamp(t, 0, 15);
	} else {
		t = 16 - (16 * 16 * base) / gain_val;
		t = clamp(t, 0, 7) * 2;
	}

	return (s << 4) | t;
}

/* Registers are only written while streaming; sensor_reg_init replays them. */
static bool ar0234_streaming(struct sensor_info *info)
{
	return info->stream_count > 0;
}

static void ar0234_write_gain(struct v4l2_subdev *sd, int gain_val)
{
	u16 ana = ar0234_gain_to_reg(gain_val);

	sensor_write(sd, AR0234_REG_MFR_30BA, ar0234_mfr_30ba(ana));
	sensor_write(sd, AR0234_REG_ANALOG_GAIN, ana);
}

static int sensor_s_gain(struct v4l2_subdev *sd, int gain_val)
{
	struct sensor_info *info = to_state(sd);

	info->gain = gain_val;
	if (ar0234_streaming(info))
		ar0234_write_gain(sd, gain_val);
	return 0;
}

/*
 * exp_val is in lines, Q4. frame_length_lines = height + vblank, and is
 * stretched when the exposure needs more lines (lower fps).
 */
static void ar0234_write_exp(struct v4l2_subdev *sd, unsigned int exp_val)
{
	struct sensor_info *info = to_state(sd);
	unsigned int shutter = exp_val >> 4;
	unsigned int frame_length = info->current_wins ?
		info->current_wins->height + to_ar0234(info)->vblank :
		ar0234_sensor_vts;

	if (shutter < AR0234_EXP_MIN)
		shutter = AR0234_EXP_MIN;
	if (shutter > 0xFFFF - AR0234_FLL_OVERHEAD - 1)
		shutter = 0xFFFF - AR0234_FLL_OVERHEAD - 1;
	if (shutter + AR0234_FLL_OVERHEAD > frame_length)
		frame_length = shutter + AR0234_FLL_OVERHEAD;

	sensor_write(sd, AR0234_REG_FRAME_LENGTH_LINES, frame_length);
	sensor_write(sd, AR0234_REG_COARSE_INTEG_TIME, shutter);
}

static int sensor_s_exp(struct v4l2_subdev *sd, unsigned int exp_val)
{
	struct sensor_info *info = to_state(sd);

	info->exp = exp_val;
	if (ar0234_streaming(info))
		ar0234_write_exp(sd, exp_val);
	return 0;
}

static int sensor_s_vblank(struct v4l2_subdev *sd, unsigned int vblank)
{
	struct sensor_info *info = to_state(sd);

	to_ar0234(info)->vblank = vblank;
	if (ar0234_streaming(info))
		ar0234_write_exp(sd, info->exp);
	return 0;
}

static int sensor_s_exp_gain(struct v4l2_subdev *sd,
			     struct sensor_exp_gain *exp_gain)
{
	sensor_s_exp(sd, exp_gain->exp_val);
	sensor_s_gain(sd, exp_gain->gain_val);
	return 0;
}

/*
 * The module has no RESET/PWDN pins and its own 24 MHz crystal.
 * Board MCLK (PE5) lands on a module NC pin; enabling it is harmless and
 * keeps the VIN flow the same as other sensors. No vin_gpio_* on PWDN/RESET.
 */
static int sensor_power(struct v4l2_subdev *sd, int on)
{
	switch (on) {
	case STBY_ON:
		sensor_dbg("STBY_ON!\n");
		vin_set_mclk(sd, OFF);
		break;
	case STBY_OFF:
		sensor_dbg("STBY_OFF!\n");
		cci_lock(sd);
		vin_set_mclk_freq(sd, MCLK);
		vin_set_mclk(sd, ON);
		usleep_range(10000, 12000);
		cci_unlock(sd);
		break;
	case PWR_ON:
		sensor_dbg("PWR_ON!\n");
		cci_lock(sd);
		vin_set_mclk_freq(sd, MCLK);
		vin_set_mclk(sd, ON);
		vin_gpio_write(sd, POWER_EN, CSI_GPIO_HIGH);
		vin_set_pmu_channel(sd, IOVDD, ON);
		vin_set_pmu_channel(sd, AVDD, ON);
		vin_set_pmu_channel(sd, DVDD, ON);
		/* >= 6.2 ms before first I2C access (rpi AR0234_RESET_DELAY_MIN_US) */
		usleep_range(10000, 12000);
		cci_unlock(sd);
		break;
	case PWR_OFF:
		sensor_dbg("PWR_OFF!\n");
		cci_lock(sd);
		vin_set_mclk(sd, OFF);
		vin_gpio_write(sd, POWER_EN, CSI_GPIO_LOW);
		vin_set_pmu_channel(sd, DVDD, OFF);
		vin_set_pmu_channel(sd, AVDD, OFF);
		vin_set_pmu_channel(sd, IOVDD, OFF);
		cci_unlock(sd);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/* No reset pin. Reset is done by software in sensor_default_regs. */
static int sensor_reset(struct v4l2_subdev *sd, u32 val)
{
	return 0;
}

static int sensor_detect(struct v4l2_subdev *sd)
{
	data_type rdval = 0;
	int ret;

	ret = sensor_read(sd, AR0234_REG_CHIP_ID, &rdval);
	if (ret < 0) {
		sensor_err("read chip id failed, ret = %d\n", ret);
		return -ENODEV;
	}
	if (rdval == AR0234_CHIP_ID_MONO) {
		sensor_print("find AR0234 mono, chip id 0x%04x\n", rdval);
		return 0;
	}
	if (rdval != AR0234_CHIP_ID) {
		sensor_err("invalid chip id 0x%04x, expect 0x%04x\n",
			   rdval, AR0234_CHIP_ID);
		return -ENODEV;
	}
	sensor_print("find AR0234, chip id 0x%04x\n", rdval);
	return 0;
}

static int sensor_init(struct v4l2_subdev *sd, u32 val)
{
	int ret;
	struct sensor_info *info = to_state(sd);

	ret = sensor_detect(sd);
	if (ret) {
		sensor_err("chip found is not an target chip.\n");
		return ret;
	}

	info->focus_status = 0;
	info->low_speed = 0;
	info->width = 1920;
	info->height = 1200;
	info->hflip = 0;
	info->vflip = 0;
	/* keep info->exp / info->gain: VIN calls init on every S_INPUT */

	info->tpf.numerator = 1;
	info->tpf.denominator = (fps > 0 && fps < SENSOR_FRAME_RATE) ?
				fps : SENSOR_FRAME_RATE;

	info->preview_first_flag = 1;

	return 0;
}

static long sensor_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	int ret = 0;
	struct sensor_info *info = to_state(sd);

	switch (cmd) {
	case GET_CURRENT_WIN_CFG:
		if (info->current_wins != NULL) {
			memcpy(arg, info->current_wins,
			       sizeof(*info->current_wins));
			ret = 0;
		} else {
			sensor_err("empty wins!\n");
			ret = -1;
		}
		break;
	case SET_FPS:
		break;
	case VIDIOC_VIN_SENSOR_EXP_GAIN:
		sensor_s_exp_gain(sd, (struct sensor_exp_gain *)arg);
		break;
	case VIDIOC_VIN_SENSOR_CFG_REQ:
		sensor_cfg_req(sd, (struct sensor_config *)arg);
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

/* Color AR0234 outputs GRBG bayer (rpi ar0234_pll_configs). */
static struct sensor_format_struct sensor_formats[] = {
	{
		.desc = "Raw RGB Bayer",
		.mbus_code = MEDIA_BUS_FMT_SGRBG10_1X10,
		.regs = sensor_fmt_raw,
		.regs_size = ARRAY_SIZE(sensor_fmt_raw),
		.bpp = 1
	},
};
#define N_FMTS ARRAY_SIZE(sensor_formats)

static struct sensor_win_size sensor_win_sizes[] = {
	{
	 .width = 1920,
	 .height = 1200,
	 .hoffset = 0,
	 .voffset = 0,
	 .hts = AR0234_HTS,
	 .vts = AR0234_VTS_1200,
	 .pclk = AR0234_PCLK,
	 .mipi_bps = AR0234_MIPI_BPS,
	 .fps_fixed = 1,
	 .bin_factor = 1,
	 .intg_min = AR0234_EXP_MIN << 4,
	 .intg_max = (AR0234_VTS_1200 - 1) << 4,
	 .gain_min = 1 << 4,
	 .gain_max = 16 << 4,
	 .regs = sensor_1920x1200_regs,
	 .regs_size = ARRAY_SIZE(sensor_1920x1200_regs),
	 .set_size = NULL,
	 },
};
#define N_WIN_SIZES (ARRAY_SIZE(sensor_win_sizes))

static int sensor_reg_init(struct sensor_info *info)
{
	int ret;
	struct v4l2_subdev *sd = &info->sd;
	struct sensor_format_struct *sensor_fmt = info->fmt;
	struct sensor_win_size *wsize = info->current_wins;

	ret = sensor_write_array(sd, sensor_default_regs,
				 ARRAY_SIZE(sensor_default_regs));
	if (ret < 0) {
		sensor_err("write sensor_default_regs error\n");
		return ret;
	}

	sensor_write_array(sd, sensor_fmt->regs, sensor_fmt->regs_size);

	if (wsize->regs) {
		ret = sensor_write_array(sd, wsize->regs, wsize->regs_size);
		if (ret < 0) {
			sensor_err("write win regs error\n");
			return ret;
		}
	}

	if (wsize->set_size)
		wsize->set_size(sd);

	info->width = wsize->width;
	info->height = wsize->height;
	ar0234_sensor_vts = wsize->vts;

	/* replay exposure / gain / frame length set while not streaming */
	ar0234_write_exp(sd, info->exp);
	ar0234_write_gain(sd, info->gain);

	if (test_pattern)
		sensor_write(sd, AR0234_REG_TEST_PATTERN_MODE, test_pattern);

	/* stream on */
	ret = sensor_write(sd, AR0234_REG_RESET,
			   AR0234_RESET_DEFAULT | AR0234_RESET_STREAM);

	sensor_print("s_fmt set width = %d, height = %d, lanes = %d\n",
		     wsize->width, wsize->height, AR0234_LANES);

	return ret;
}

static int sensor_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct sensor_info *info = to_state(sd);

	sensor_print("%s on = %d, %d*%d %x\n", __func__, enable,
		     info->current_wins->width,
		     info->current_wins->height, info->fmt->mbus_code);

	if (!enable)
		return sensor_write(sd, AR0234_REG_RESET, AR0234_RESET_DEFAULT);
	return sensor_reg_init(info);
}

static int sensor_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
				struct v4l2_mbus_config *cfg)
{
	unsigned int lanes = (AR0234_LANES == 4) ?
			     V4L2_MBUS_CSI2_4_LANE : V4L2_MBUS_CSI2_2_LANE;

	cfg->type = V4L2_MBUS_CSI2_DPHY;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	cfg->bus.mipi_csi2.num_data_lanes = lanes | V4L2_MBUS_CSI2_CHANNEL_0;
#else
	cfg->flags = lanes | V4L2_MBUS_CSI2_CHANNEL_0;
#endif
	return 0;
}

/*
 * Controls are not VOLATILE: v4l2 core skips s_ctrl for volatile controls
 * (cluster_changed()), so a volatile exposure/gain was never written.
 */
static int sensor_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sensor_info *info =
			container_of(ctrl->handler, struct sensor_info, handler);
	struct v4l2_subdev *sd = &info->sd;

	switch (ctrl->id) {
	case V4L2_CID_GAIN:
		return sensor_s_gain(sd, ctrl->val);
	case V4L2_CID_EXPOSURE:
		return sensor_s_exp(sd, ctrl->val);
	case V4L2_CID_VBLANK:
		return sensor_s_vblank(sd, ctrl->val);
	}
	return -EINVAL;
}

/* ----------------------------------------------------------------------- */

static const struct v4l2_ctrl_ops sensor_ctrl_ops = {
	.s_ctrl = sensor_s_ctrl,
};

static const struct v4l2_subdev_core_ops sensor_core_ops = {
	.reset = sensor_reset,
	.init = sensor_init,
	.s_power = sensor_power,
	.ioctl = sensor_ioctl,
#if IS_ENABLED(CONFIG_COMPAT)
	.compat_ioctl32 = sensor_compat_ioctl32,
#endif
};

static const struct v4l2_subdev_video_ops sensor_video_ops = {
	.s_stream = sensor_s_stream,
};

static const struct v4l2_subdev_pad_ops sensor_pad_ops = {
	.enum_mbus_code = sensor_enum_mbus_code,
	.enum_frame_size = sensor_enum_frame_size,
	.get_fmt = sensor_get_fmt,
	.set_fmt = sensor_set_fmt,
	.get_mbus_config = sensor_g_mbus_config,
};

static const struct v4l2_subdev_ops sensor_ops = {
	.core = &sensor_core_ops,
	.video = &sensor_video_ops,
	.pad = &sensor_pad_ops,
};

/* ----------------------------------------------------------------------- */
/* AR0234: 16-bit register address, 16-bit data */
static struct cci_driver cci_drv[] = {
	{
		.name = SENSOR_NAME,
		.addr_width = CCI_BITS_16,
		.data_width = CCI_BITS_16,
	}, {
		.name = SENSOR_NAME_2,
		.addr_width = CCI_BITS_16,
		.data_width = CCI_BITS_16,
	}
};

static int sensor_init_controls(struct v4l2_subdev *sd, const struct v4l2_ctrl_ops *ops)
{
	struct sensor_info *info = to_state(sd);
	struct ar0234_state *st = to_ar0234(info);
	struct v4l2_ctrl_handler *handler = &info->handler;
	int ret = 0;

	unsigned int vblank_def = ar0234_default_vblank();

	v4l2_ctrl_handler_init(handler, 3);

	/* exposure: lines, Q4; default ~7 ms (512 lines at 2 lane) */
	st->exp_ctrl = v4l2_ctrl_new_std(handler, ops, V4L2_CID_EXPOSURE,
					 AR0234_EXP_MIN << 4,
					 (0xFFFF - AR0234_FLL_OVERHEAD - 1) << 4,
					 1, AR0234_EXP_DEF_LINES << 4);
	/* analog gain: Q4, 1x..16x; default 28 = 1.75x (~R0x3060 0x0E) */
	st->gain_ctrl = v4l2_ctrl_new_std(handler, ops, V4L2_CID_GAIN,
					  1 * 16, 16 * 16, 1, 28);
	/* vblank in lines: fps = pclk / (hts * (1200 + vblank)) */
	st->vblank_ctrl = v4l2_ctrl_new_std(handler, ops, V4L2_CID_VBLANK,
					    AR0234_VBLANK_MIN,
					    AR0234_VBLANK_MAX, 1, vblank_def);

	/* defaults until the first s_ctrl */
	info->exp = AR0234_EXP_DEF_LINES << 4;
	info->gain = 28;
	st->vblank = vblank_def;

	if (handler->error) {
		ret = handler->error;
		v4l2_ctrl_handler_free(handler);
	}

	sd->ctrl_handler = handler;

	return ret;
}

static int sensor_dev_id;

static int ar0234_cci_index(struct i2c_client *client)
{
	int i;

	for (i = 0; i < SENSOR_NUM; i++) {
		if (!strcmp(cci_drv[i].name, client->name))
			return i;
	}
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
static int sensor_probe(struct i2c_client *client)
#else
static int sensor_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
#endif
{
	struct v4l2_subdev *sd;
	struct ar0234_state *st;
	struct sensor_info *info;

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (st == NULL)
		return -ENOMEM;
	info = &st->info;
	sd = &info->sd;

	if (client)
		cci_dev_probe_helper(sd, client, &sensor_ops,
				     &cci_drv[ar0234_cci_index(client)]);
	else
		cci_dev_probe_helper(sd, client, &sensor_ops,
				     &cci_drv[sensor_dev_id++]);

	sensor_init_controls(sd, &sensor_ctrl_ops);
	mutex_init(&info->lock);

#if IS_ENABLED(CONFIG_SAME_I2C)
	info->sensor_i2c_addr = I2C_ADDR >> 1;
#endif
	info->fmt = &sensor_formats[0];
	info->fmt_pt = &sensor_formats[0];
	info->win_pt = &sensor_win_sizes[0];
	info->fmt_num = N_FMTS;
	info->win_size_num = N_WIN_SIZES;
	info->sensor_field = V4L2_FIELD_NONE;
	info->af_first_flag = 1;

	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
static int sensor_remove(struct i2c_client *client)
#else
static void sensor_remove(struct i2c_client *client)
#endif
{
	struct v4l2_subdev *sd;

	if (client)
		sd = cci_dev_remove_helper(client,
					   &cci_drv[ar0234_cci_index(client)]);
	else
		sd = cci_dev_remove_helper(client, &cci_drv[sensor_dev_id++]);
	kfree(to_state(sd));
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	return 0;
#endif
}

static const struct i2c_device_id sensor_id[] = {
	{SENSOR_NAME, 0},
	{}
};

static const struct i2c_device_id sensor_id_2[] = {
	{SENSOR_NAME_2, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, sensor_id);
MODULE_DEVICE_TABLE(i2c, sensor_id_2);

static struct i2c_driver sensor_driver[] = {
	{
		.driver = {
			   .owner = THIS_MODULE,
			   .name = SENSOR_NAME,
			   },
		.probe = sensor_probe,
		.remove = sensor_remove,
		.id_table = sensor_id,
	}, {
		.driver = {
			   .owner = THIS_MODULE,
			   .name = SENSOR_NAME_2,
			   },
		.probe = sensor_probe,
		.remove = sensor_remove,
		.id_table = sensor_id_2,
	},
};

static __init int init_sensor(void)
{
	int i, ret = 0;

	sensor_dev_id = 0;

	for (i = 0; i < SENSOR_NUM; i++)
		ret = cci_dev_init_helper(&sensor_driver[i]);

	return ret;
}

static __exit void exit_sensor(void)
{
	int i;

	sensor_dev_id = 0;

	for (i = 0; i < SENSOR_NUM; i++)
		cci_dev_exit_helper(&sensor_driver[i]);
}

VIN_INIT_DRIVERS(init_sensor);
module_exit(exit_sensor);
