// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX296 CMOS Image Sensor - V4L2 OOT Driver
 *
 * Ported from mainline Linux (drivers/media/i2c/imx296.c, Laurent Pinchart)
 * for L4T 5.15.148-tegra / JetPack 6 on Jetson Orin Nano Super.
 *
 * Sensor: Sony IMX296LQR-C (color) / IMX296LLR-C (mono)
 * Active pixels: 1456 x 1088
 * Interface: MIPI CSI-2, 1 or 2 lanes, RAW10
 * I2C: address 0x1a, 16-bit registers, 8-bit values
 *
 * Register values marked VERIFY should be cross-checked against the mainline
 * driver source at:
 *   https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
 *   File: drivers/media/i2c/imx296.c
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* -------------------------------------------------------------------------
 * Register map (16-bit address, 8-bit value)
 */
#define IMX296_STANDBY          0x3000  /* bit0: 0=operating, 1=standby */
#define IMX296_REGHOLD          0x3008  /* bit0: 1=hold updates, 0=latch */
#define IMX296_XMSTA            0x300a  /* bit0: 0=master, 1=ext trigger */

#define IMX296_CTRL06           0x3006
#define IMX296_CTRL07           0x3007
#define IMX296_CTRL0B           0x300b
#define IMX296_CTRL0C           0x300c
#define IMX296_CTRL0D           0x300d
#define IMX296_CTRL0E           0x300e
#define IMX296_CTRL13           0x3013
#define IMX296_CTRL1D           0x301d
#define IMX296_CTRL1E           0x301e
#define IMX296_CTRL1F           0x301f
#define IMX296_CTRL20           0x3020

/* Analog gain: 10-bit, 0 (0dB) .. 240 (48dB) */
#define IMX296_GAIN             0x3204
#define IMX296_GAIN_MIN         0
#define IMX296_GAIN_MAX         240
#define IMX296_GAIN_STEP        1
#define IMX296_GAIN_DEFAULT     0

/* Black level: 10-bit */
#define IMX296_BLKLEVEL         0x3254

/* ROI / crop window */
#define IMX296_FID0_ROI         0x3300
#define IMX296_FID0_ROIH1ON     BIT(0)
#define IMX296_FID0_ROIV1ON     BIT(1)
/* Vertical active size (2 bytes, little-endian) */
#define IMX296_FID0_VSIZE       0x3308
/* Horizontal active size (2 bytes, little-endian) */
#define IMX296_FID0_HSIZE       0x330c

/* VMAX: total vertical lines per frame (3 bytes) — controls frame rate */
#define IMX296_VMAX             0x3010
/* HMAX: line length in internal clock units — controls horizontal blanking */
#define IMX296_HMAX             0x3014
/* SHS: shutter start line (3 bytes); exposure_lines = VMAX - SHS */
#define IMX296_SHS              0x308d

/* -------------------------------------------------------------------------
 * Sensor geometry
 */
#define IMX296_PIXEL_ARRAY_WIDTH    1456
#define IMX296_PIXEL_ARRAY_HEIGHT   1088

/* VMAX for ~60fps at 37.125 MHz INCK, 2-lane, RAW10 */
#define IMX296_VMAX_DEFAULT         1109

/* -------------------------------------------------------------------------
 * Regmap helpers
 */
struct imx296_reg {
	u16 addr;
	u8  val;
};

#define IMX296_REG_END  { 0xffff, 0x00 }

static int imx296_write_table(struct regmap *rm,
			      const struct imx296_reg *table)
{
	int ret;

	for (; table->addr != 0xffff; table++) {
		ret = regmap_write(rm, table->addr, table->val);
		if (ret)
			return ret;
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * Initialization tables
 *
 * VERIFY: Cross-check these values against the mainline Linux driver at
 *   drivers/media/i2c/imx296.c before deploying in production.
 * The "magic" CTRL registers are manufacturer-reserved initialization values.
 */

/* Common registers written regardless of clock or lane count */
static const struct imx296_reg imx296_common_regs[] = {
	{ 0x3005,    0xf0 },  /* required: activates CSI-2 output */
	{ IMX296_CTRL06,   0x00 },
	{ IMX296_CTRL07,   0x00 },
	{ IMX296_CTRL0B,   0x00 },
	{ IMX296_CTRL0D,   0x00 },  /* VERIFY */
	{ IMX296_CTRL0E,   0x00 },
	{ IMX296_CTRL1D,   0x00 },
	{ IMX296_CTRL1E,   0x00 },
	{ IMX296_CTRL1F,   0x00 },
	{ IMX296_CTRL20,   0x00 },
	{ IMX296_BLKLEVEL, 0x3c },  /* low byte: black level = 0 */
	{ 0x3255,          0x00 },  /* high byte */
	/* Manufacturer-reserved init values — required for valid sensor output */
	{ 0x309e, 0x04 },
	{ 0x30a0, 0x04 },
	{ 0x30a1, 0x3c },
	{ 0x30a4, 0x5f },
	{ 0x30a8, 0x91 },
	{ 0x30ac, 0x28 },
	{ 0x30af, 0x0b },
	{ 0x30df, 0x00 },
	{ 0x3165, 0x00 },
	{ 0x3169, 0x10 },
	{ 0x316a, 0x02 },
	{ 0x31c8, 0xf3 },
	{ 0x31d0, 0xf4 },
	{ 0x321a, 0x00 },
	{ 0x3226, 0x02 },
	{ 0x3256, 0x01 },
	{ 0x3541, 0x72 },
	{ 0x3516, 0x77 },
	{ 0x350b, 0x7f },
	{ 0x3758, 0xa3 },
	{ 0x3759, 0x00 },
	{ 0x375a, 0x85 },
	{ 0x375b, 0x00 },
	{ 0x3832, 0xf5 },
	{ 0x3833, 0x00 },
	{ 0x38a2, 0xf6 },
	{ 0x38a3, 0x00 },
	{ 0x3a00, 0x80 },
	{ 0x3d48, 0xa3 },
	{ 0x3d49, 0x00 },
	{ 0x3d4a, 0x85 },
	{ 0x3d4b, 0x00 },
	{ 0x400e, 0x58 },
	{ 0x4014, 0x1c },
	{ 0x4041, 0x2a },
	{ 0x40a2, 0x06 },
	{ 0x40c1, 0xf6 },
	{ 0x40c7, 0x0f },
	{ 0x40c8, 0x00 },
	{ 0x4174, 0x00 },
	IMX296_REG_END,
};

/* 54 MHz INCK — confirmed from RPi i2c trace on Waveshare IMX296 */
static const struct imx296_reg imx296_54mhz_regs[] = {
	{ 0x3089, 0xb0 },  /* INCKSEL0 — confirmed */
	{ 0x308a, 0x0f },  /* INCKSEL1 — confirmed */
	{ 0x308b, 0xb0 },  /* INCKSEL2 — confirmed */
	{ 0x308c, 0x0c },  /* INCKSEL3 — confirmed */
	{ 0x4114, 0xc5 },  /* GTTABLENUM — confirmed */
	{ 0x418c, 0xa8 },  /* CTRL418C = 168 for 54 MHz — confirmed */
	IMX296_REG_END,
};

/* 2-lane MIPI */
static const struct imx296_reg imx296_2lane_regs[] = {
	{ 0x30c1, 0x00 },  /* 0=2lane VERIFY */
	IMX296_REG_END,
};

/* 1-lane MIPI */
static const struct imx296_reg imx296_1lane_regs[] = {
	{ 0x30c1, 0x01 },  /* 1=1lane VERIFY */
	IMX296_REG_END,
};

/* Start streaming */
static const struct imx296_reg imx296_start_regs[] = {
	{ IMX296_STANDBY, 0x00 },  /* exit standby */
	IMX296_REG_END,
};

static const struct imx296_reg imx296_trigger_regs[] = {
	{ 0x300b, 0x00 },  /* CTRL0B trigger disabled — confirmed */
	{ 0x30ae, 0x00 },  /* LOWLAGTRG — confirmed */
	IMX296_REG_END,
};

static const struct imx296_reg imx296_xmsta_regs[] = {
	{ IMX296_XMSTA,   0x00 },  /* start master-mode output */
	IMX296_REG_END,
};

/* Stop streaming */
static const struct imx296_reg imx296_stop_regs[] = {
	{ IMX296_XMSTA,   0x01 },  /* stop master-mode output */
	{ IMX296_STANDBY, 0x01 },  /* enter standby */
	IMX296_REG_END,
};

/* -------------------------------------------------------------------------
 * Driver state
 */
struct imx296 {
	struct device           *dev;
	struct regmap           *regmap;
	struct clk              *clk;
	struct gpio_desc        *reset;
	struct regulator        *supply;

	struct v4l2_subdev       sd;
	struct media_pad         pad;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl        *gain;
	struct v4l2_ctrl        *exposure;

	struct v4l2_mbus_framefmt fmt;

	u32  clk_freq;
	u32  num_data_lanes;
	u32  vmax;
	bool is_mono;
	bool streaming;
};

static inline struct imx296 *sd_to_imx296(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx296, sd);
}

/* -------------------------------------------------------------------------
 * Exposure / gain helpers
 */
static int imx296_set_vmax(struct imx296 *sensor, u32 vmax)
{
	return regmap_bulk_write(sensor->regmap, IMX296_VMAX,
				 (u8[]){ vmax & 0xff,
					 (vmax >> 8) & 0xff,
					 (vmax >> 16) & 0x0f }, 3);
}

static int imx296_set_exposure(struct imx296 *sensor, u32 exp_lines)
{
	u32 shs;

	exp_lines = clamp(exp_lines, 1u, sensor->vmax - 1);
	shs = sensor->vmax - exp_lines;

	return regmap_bulk_write(sensor->regmap, IMX296_SHS,
				 (u8[]){ shs & 0xff,
					 (shs >> 8) & 0xff,
					 (shs >> 16) & 0x0f }, 3);
}

static int imx296_set_gain(struct imx296 *sensor, u32 gain)
{
	return regmap_bulk_write(sensor->regmap, IMX296_GAIN,
				 (u8[]){ gain & 0xff,
					 (gain >> 8) & 0x03 }, 2);
}

/* -------------------------------------------------------------------------
 * Power management
 */
static int imx296_power_on(struct imx296 *sensor)
{
	int ret;

	if (sensor->supply) {
		ret = regulator_enable(sensor->supply);
		if (ret)
			return ret;
	}

	ret = clk_prepare_enable(sensor->clk);
	if (ret)
		goto disable_reg;

	if (sensor->reset) {
		gpiod_set_value_cansleep(sensor->reset, 1);
		usleep_range(1000, 2000);
		gpiod_set_value_cansleep(sensor->reset, 0);
		usleep_range(5000, 10000);
	}

	/* Sensor needs ~20ms after XCLR release before I2C is ready */
	usleep_range(20000, 25000);
	return 0;

disable_reg:
	if (sensor->supply)
		regulator_disable(sensor->supply);
	return ret;
}

static int imx296_power_off(struct imx296 *sensor)
{
	if (sensor->reset)
		gpiod_set_value_cansleep(sensor->reset, 1);

	clk_disable_unprepare(sensor->clk);

	if (sensor->supply)
		regulator_disable(sensor->supply);

	return 0;
}

static int imx296_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx296 *sensor = sd_to_imx296(sd);

	return imx296_power_on(sensor);
}

static int imx296_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx296 *sensor = sd_to_imx296(sd);

	imx296_power_off(sensor);
	return 0;
}

/* -------------------------------------------------------------------------
 * Sensor initialisation
 */
static int imx296_init_sensor(struct imx296 *sensor)
{
	const struct imx296_reg *clk_regs;
	const struct imx296_reg *lane_regs;
	int ret;

	/* Put sensor in standby while configuring */
	ret = regmap_write(sensor->regmap, IMX296_STANDBY, 0x01);
	if (ret)
		return ret;

	clk_regs  = imx296_54mhz_regs;
	lane_regs = (sensor->num_data_lanes == 2) ?
		imx296_2lane_regs : imx296_1lane_regs;

	ret = imx296_write_table(sensor->regmap, imx296_common_regs);
	if (ret)
		return ret;

	ret = imx296_write_table(sensor->regmap, clk_regs);
	if (ret)
		return ret;
	/* Verify INCKSEL registers landed */
	{
    unsigned int v0, v1, v2, v3;
    regmap_read(sensor->regmap, 0x3089, &v0);
    regmap_read(sensor->regmap, 0x308a, &v1);
    regmap_read(sensor->regmap, 0x308b, &v2);
    regmap_read(sensor->regmap, 0x308c, &v3);
    dev_info(sensor->dev,
             "INCKSEL: %02x %02x %02x %02x\n", v0, v1, v2, v3);
	}

	ret = imx296_write_table(sensor->regmap, lane_regs);
	if (ret)
		return ret;

	/* Full frame — ROI disabled, confirmed from RPi trace */
	ret = regmap_write(sensor->regmap, IMX296_FID0_ROI, 0x00);
	if (ret)
		return ret;

	/* MIPI output area height — confirmed from RPi trace */
	ret = regmap_bulk_write(sensor->regmap, 0x4182,
				(u8[]){ IMX296_PIXEL_ARRAY_HEIGHT & 0xff,
					(IMX296_PIXEL_ARRAY_HEIGHT >> 8) & 0xff }, 2);
	if (ret)
		return ret;

	/*
	 * Hold register updates while writing the interdependent timing
	 * triplet (VMAX, SHS, gain) so they all latch on the same frame edge.
	 */
	regmap_write(sensor->regmap, IMX296_REGHOLD, 0x01);

	ret = imx296_set_vmax(sensor, sensor->vmax);
	if (ret)
		goto release_hold;

	ret = regmap_bulk_write(sensor->regmap, IMX296_HMAX,
				(u8[]){ 1100 & 0xff, (1100 >> 8) & 0xff }, 2);
	if (ret)
		goto release_hold;

	ret = imx296_set_exposure(sensor, sensor->vmax - 1);
	if (ret)
		goto release_hold;

	ret = imx296_set_gain(sensor, 0);
	if (ret)
		goto release_hold;

	ret = regmap_write(sensor->regmap, 0x3212, 0x09); /* GAINDLY 1 frame — confirmed */
	if (ret)
		goto release_hold;

	ret = regmap_write(sensor->regmap, 0x3238, 0x04); /* PGCTRL CLKEN — confirmed */
	if (ret)
		goto release_hold;

	ret = regmap_write(sensor->regmap, 0x3022, 0x01); /* BLKLEVELAUTO on — confirmed */
	if (ret)
		goto release_hold;

	release_hold:


	regmap_write(sensor->regmap, IMX296_REGHOLD, 0x00);
	return ret;
}

/* -------------------------------------------------------------------------
 * V4L2 control ops
 */
static int imx296_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx296 *sensor = container_of(ctrl->handler,
					     struct imx296, ctrls);
	int ret;

	/* Only update hardware when powered and streaming */
	ret = pm_runtime_get_if_in_use(sensor->dev);
	if (!ret || WARN_ON_ONCE(ret < 0))
		return 0;

	regmap_write(sensor->regmap, IMX296_REGHOLD, 0x01);

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = imx296_set_gain(sensor, ctrl->val);
		break;
	case V4L2_CID_EXPOSURE:
		ret = imx296_set_exposure(sensor, ctrl->val);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	regmap_write(sensor->regmap, IMX296_REGHOLD, 0x00);
	pm_runtime_put(sensor->dev);
	return ret;
}

static const struct v4l2_ctrl_ops imx296_ctrl_ops = {
	.s_ctrl = imx296_s_ctrl,
};

/* -------------------------------------------------------------------------
 * V4L2 subdev pad ops
 */
static void imx296_update_pad_format(const struct imx296 *sensor,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->width        = IMX296_PIXEL_ARRAY_WIDTH;
	fmt->height       = IMX296_PIXEL_ARRAY_HEIGHT;
	fmt->code         = sensor->is_mono ? MEDIA_BUS_FMT_Y10_1X10
					    : MEDIA_BUS_FMT_SRGGB10_1X10;
	fmt->field        = V4L2_FIELD_NONE;
	fmt->colorspace   = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc    = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func    = V4L2_XFER_FUNC_DEFAULT;
}

static int imx296_init_cfg(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state)
{
	struct imx296 *sensor = sd_to_imx296(sd);

	imx296_update_pad_format(sensor,
				 v4l2_subdev_get_try_format(sd, state, 0));
	return 0;
}

static int imx296_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx296 *sensor = sd_to_imx296(sd);

	if (code->index != 0)
		return -EINVAL;

	code->code = sensor->is_mono ? MEDIA_BUS_FMT_Y10_1X10
				     : MEDIA_BUS_FMT_SRGGB10_1X10;
	return 0;
}

static int imx296_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx296 *sensor = sd_to_imx296(sd);
	u32 code = sensor->is_mono ? MEDIA_BUS_FMT_Y10_1X10
				   : MEDIA_BUS_FMT_SRGGB10_1X10;

	if (fse->index != 0 || fse->code != code)
		return -EINVAL;

	fse->min_width  = IMX296_PIXEL_ARRAY_WIDTH;
	fse->max_width  = IMX296_PIXEL_ARRAY_WIDTH;
	fse->min_height = IMX296_PIXEL_ARRAY_HEIGHT;
	fse->max_height = IMX296_PIXEL_ARRAY_HEIGHT;
	return 0;
}

static int imx296_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx296 *sensor = sd_to_imx296(sd);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt->format = *v4l2_subdev_get_try_format(sd, state, 0);
	else
		fmt->format = sensor->fmt;

	return 0;
}

static int imx296_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx296 *sensor = sd_to_imx296(sd);

	/* Single fixed mode — clamp to our format regardless of request */
	imx296_update_pad_format(sensor, &fmt->format);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		*v4l2_subdev_get_try_format(sd, state, 0) = fmt->format;
	else
		sensor->fmt = fmt->format;

	return 0;
}

/* -------------------------------------------------------------------------
 * V4L2 subdev video ops
 */
static int imx296_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx296 *sensor = sd_to_imx296(sd);
	int ret = 0;

	if (enable == sensor->streaming)
		return 0;

	if (enable) {
		ret = pm_runtime_get_sync(sensor->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(sensor->dev);
			return ret;
		}

		ret = imx296_init_sensor(sensor);
		if (ret)
			goto err_pm;

		ret = v4l2_ctrl_handler_setup(&sensor->ctrls);
		if (ret)
			goto err_pm;

		ret = imx296_write_table(sensor->regmap, imx296_start_regs);
		if (ret)
			goto err_pm;

		/* Sensor needs ~20 ms after exiting standby before MIPI output is stable. */
		usleep_range(20000, 22000);

		ret = imx296_write_table(sensor->regmap, imx296_trigger_regs);
		if (ret)
			goto err_pm;
		
		ret = imx296_write_table(sensor->regmap, imx296_xmsta_regs);
		if (ret)
			goto err_pm;

		sensor->streaming = true;
	} else {
		ret = imx296_write_table(sensor->regmap, imx296_stop_regs);
		sensor->streaming = false;
		pm_runtime_put(sensor->dev);
	}

	return ret;

err_pm:
	pm_runtime_put(sensor->dev);
	return ret;
}

static const struct v4l2_subdev_video_ops imx296_video_ops = {
	.s_stream = imx296_s_stream,
};

static const struct v4l2_subdev_pad_ops imx296_pad_ops = {
	.init_cfg         = imx296_init_cfg,
	.enum_mbus_code   = imx296_enum_mbus_code,
	.enum_frame_size  = imx296_enum_frame_size,
	.get_fmt          = imx296_get_pad_format,
	.set_fmt          = imx296_set_pad_format,
};

static const struct v4l2_subdev_ops imx296_subdev_ops = {
	.video = &imx296_video_ops,
	.pad   = &imx296_pad_ops,
};

/* -------------------------------------------------------------------------
 * Regmap configuration
 */
static const struct regmap_config imx296_regmap_config = {
	.reg_bits     = 16,
	.val_bits     = 8,
	.max_register = 0x44ff,
};

/* -------------------------------------------------------------------------
 * Probe / remove
 */
static int imx296_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *endpoint;
	struct imx296 *sensor;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->dev = dev;

	/*
	 * sony,imx296    → color (RGGB Bayer, MEDIA_BUS_FMT_SRGGB10_1X10)
	 * sony,imx296llr → mono  (Y10,        MEDIA_BUS_FMT_Y10_1X10)
	 */
	sensor->is_mono = !!(uintptr_t)of_device_get_match_data(dev);

	sensor->vmax = IMX296_VMAX_DEFAULT;

	/* Parse MIPI endpoint from device tree */
	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint) {
		dev_err(dev, "no endpoint in device tree\n");
		return -ENXIO;
	}
	ret = v4l2_fwnode_endpoint_parse(endpoint, &ep);
	fwnode_handle_put(endpoint);
	if (ret) {
		dev_err(dev, "failed to parse endpoint: %d\n", ret);
		return ret;
	}

	sensor->num_data_lanes = ep.bus.mipi_csi2.num_data_lanes;
	if (sensor->num_data_lanes != 1 && sensor->num_data_lanes != 2) {
		dev_err(dev, "unsupported lane count %u\n",
			sensor->num_data_lanes);
		return -EINVAL;
	}

	/* External clock */
	sensor->clk = devm_clk_get(dev, "extclk");
	if (IS_ERR(sensor->clk)) {
		dev_err(dev, "failed to get external clock: %ld\n",
			PTR_ERR(sensor->clk));
		return PTR_ERR(sensor->clk);
	}

	sensor->clk_freq = clk_get_rate(sensor->clk);
	if (sensor->clk_freq != 54000000 && sensor->clk_freq != 51000000) {
		dev_warn(dev, "unsupported clock %u Hz, using 54 MHz tables\n",
		 	sensor->clk_freq);
		sensor->clk_freq = 54000000;
	}

	/* Optional reset GPIO (active high) */
	sensor->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset))
		return PTR_ERR(sensor->reset);

	/* Optional power supply */
	sensor->supply = devm_regulator_get_optional(dev, "vdda");
	if (IS_ERR(sensor->supply)) {
		if (PTR_ERR(sensor->supply) != -ENODEV)
			return PTR_ERR(sensor->supply);
		sensor->supply = NULL;
	}

	/* Regmap */
	sensor->regmap = devm_regmap_init_i2c(client, &imx296_regmap_config);
	if (IS_ERR(sensor->regmap)) {
		dev_err(dev, "failed to init regmap: %ld\n",
			PTR_ERR(sensor->regmap));
		return PTR_ERR(sensor->regmap);
	}
	/* Controls */
	v4l2_ctrl_handler_init(&sensor->ctrls, 2);

	sensor->gain = v4l2_ctrl_new_std(&sensor->ctrls, &imx296_ctrl_ops,
					 V4L2_CID_ANALOGUE_GAIN,
					 IMX296_GAIN_MIN, IMX296_GAIN_MAX,
					 IMX296_GAIN_STEP, IMX296_GAIN_DEFAULT);

	sensor->exposure = v4l2_ctrl_new_std(&sensor->ctrls, &imx296_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     1, sensor->vmax - 1,
					     1, sensor->vmax - 1);

	if (sensor->ctrls.error) {
		ret = sensor->ctrls.error;
		goto err_ctrls;
	}

	/* Subdev */
	v4l2_i2c_subdev_init(&sensor->sd, client, &imx296_subdev_ops);
	sensor->sd.flags  |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags  = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		goto err_ctrls;

	sensor->sd.ctrl_handler = &sensor->ctrls;
	imx296_update_pad_format(sensor, &sensor->fmt);

	/* PM runtime — power on deferred until stream start */
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	ret = v4l2_async_register_subdev(&sensor->sd);
	if (ret) {
		dev_err(dev, "failed to register subdev: %d\n", ret);
		goto err_pm;
	}

	dev_info(dev, "IMX296 probed: %s, %u lanes, %u Hz INCK\n",
		 sensor->is_mono ? "mono" : "color",
		 sensor->num_data_lanes, sensor->clk_freq);
	return 0;

err_pm:
	pm_runtime_disable(dev);
	media_entity_cleanup(&sensor->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&sensor->ctrls);
	return ret;
}

static int imx296_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx296 *sensor = sd_to_imx296(sd);

	v4l2_async_unregister_subdev(sd);
	pm_runtime_disable(&client->dev);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrls);
	return 0;
}

/* -------------------------------------------------------------------------
 * Module metadata
 */
static const struct dev_pm_ops imx296_pm_ops = {
	SET_RUNTIME_PM_OPS(imx296_runtime_suspend, imx296_runtime_resume, NULL)
};

static const struct of_device_id imx296_of_match[] = {
	{ .compatible = "sony,imx296" },                        /* color LQR */
	{ .compatible = "sony,imx296llr", .data = (void *)1UL }, /* mono LLR */
	{ }
};
MODULE_DEVICE_TABLE(of, imx296_of_match);

static const struct i2c_device_id imx296_id[] = {
	{ "imx296",    0 },
	{ "imx296llr", 1 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, imx296_id);

static struct i2c_driver imx296_driver = {
	.driver = {
		.name           = "imx296",
		.of_match_table = imx296_of_match,
		.pm             = &imx296_pm_ops,
	},
	.probe    = imx296_probe,
	.remove   = imx296_remove,
	.id_table = imx296_id,
};
module_i2c_driver(imx296_driver);

MODULE_DESCRIPTION("Sony IMX296 image sensor V4L2 driver");
MODULE_AUTHOR("Ported for Jetson Orin Nano Super");
MODULE_LICENSE("GPL v2");
