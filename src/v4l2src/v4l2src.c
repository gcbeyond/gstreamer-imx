/*
 * Copyright (c) 2013-2014, Black Moth Technologies
 *   Author: Philip Craig <phil@blackmoth.com.au>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include <config.h>
#include "v4l2src.h"
#include "v4l2_buffer_pool.h"

#define DEFAULT_CAPTURE_MODE 0
#define DEFAULT_FRAMERATE_NUM 30
#define DEFAULT_FRAMERATE_DEN 1
#define DEFAULT_INPUT 1
#define DEFAULT_DEVICE "/dev/video0"
#define DEFAULT_QUEUE_SIZE 6

enum
{
	IMX_V4L2SRC_0,
	IMX_V4L2SRC_CAPTURE_MODE,
	IMX_V4L2SRC_FRAMERATE_NUM,
	IMX_V4L2SRC_INPUT,
	IMX_V4L2SRC_DEVICE,
	IMX_V4L2SRC_QUEUE_SIZE,
};

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
	"src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(
		"video/x-raw"
	)
);

GST_DEBUG_CATEGORY_STATIC(gst_imx_v4l2src_debug_category);
#define GST_CAT_DEFAULT gst_imx_v4l2src_debug_category

#define DEBUG_INIT \
	GST_DEBUG_CATEGORY_INIT(gst_imx_v4l2src_debug_category, \
			"imxv4l2src", 0, "V4L2 CSI video source");

G_DEFINE_TYPE_WITH_CODE(GstImxV4l2Src, gst_imx_v4l2src,
	GST_TYPE_PUSH_SRC, DEBUG_INIT)

/* TODO: This part is nonessential, and causes compilation errors with certain 3.10 kernels,
 * since VIDIOC_DBG_G_CHIP_IDENT is an experimental interface. Disabled for now. */
/*#define WITH_CHIP_IDENTIFICATION*/

static gint gst_imx_v4l2src_capture_setup(GstImxV4l2Src *v4l2src)
{
	struct v4l2_format fmt = {0};
	struct v4l2_streamparm parm = {0};
#ifdef WITH_CHIP_IDENTIFICATION
	struct v4l2_dbg_chip_ident chip;
#endif
	struct v4l2_frmsizeenum fszenum = {0};
	gint input;
	gint fd_v4l;

	fd_v4l = open(v4l2src->devicename, O_RDWR, 0);
	if (fd_v4l < 0) {
		GST_ERROR_OBJECT(v4l2src, "Unable to open %s",
				v4l2src->devicename);
		return -1;
	}

#ifdef WITH_CHIP_IDENTIFICATION
	memset(&chip, 0, sizeof(chip));
	if (ioctl(fd_v4l, VIDIOC_DBG_G_CHIP_IDENT, &chip))
		GST_ERROR_OBJECT(v4l2src, "VIDIOC_DBG_G_CHIP_IDENT failed");
	else
		GST_INFO_OBJECT(v4l2src, "sensor chip is %s", chip.match.name);
#endif

	fszenum.index = v4l2src->capture_mode;
	fszenum.pixel_format = V4L2_PIX_FMT_YUV420;
	if (ioctl(fd_v4l, VIDIOC_ENUM_FRAMESIZES, &fszenum) < 0) {
		GST_ERROR_OBJECT(v4l2src, "VIDIOC_ENUM_FRAMESIZES failed");
		close(fd_v4l);
		return -1;
	}
	v4l2src->capture_width = fszenum.discrete.width;
	v4l2src->capture_height = fszenum.discrete.height;
	GST_INFO_OBJECT(v4l2src, "capture mode %d: %dx%d",
			v4l2src->capture_mode,
			v4l2src->capture_width, v4l2src->capture_height);

	input = v4l2src->input;
	if (ioctl(fd_v4l, VIDIOC_S_INPUT, &input) < 0) {
		GST_ERROR_OBJECT(v4l2src, "VIDIOC_S_INPUT failed");
		close(fd_v4l);
		return -1;
	}

	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.timeperframe.numerator = v4l2src->fps_d;
	parm.parm.capture.timeperframe.denominator = v4l2src->fps_n;
	parm.parm.capture.capturemode = v4l2src->capture_mode;
	if (ioctl(fd_v4l, VIDIOC_S_PARM, &parm) < 0) {
		GST_ERROR_OBJECT(v4l2src, "VIDIOC_S_PARM failed");
		close(fd_v4l);
		return -1;
	}

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
	fmt.fmt.pix.bytesperline = 0;
	fmt.fmt.pix.priv = 0;
	fmt.fmt.pix.sizeimage = 0;
	if (ioctl(fd_v4l, VIDIOC_S_FMT, &fmt) < 0) {
		GST_ERROR_OBJECT(v4l2src, "VIDIOC_S_FMT failed");
		close(fd_v4l);
		return -1;
	}

	return fd_v4l;
}

static gboolean gst_imx_v4l2src_start(GstBaseSrc *src)
{
	GstImxV4l2Src *v4l2src = GST_IMX_V4L2SRC(src);
	struct v4l2_format fmt;

	GST_LOG_OBJECT(v4l2src, "start");

	v4l2src->fd_v4l = gst_imx_v4l2src_capture_setup(v4l2src);
	if (v4l2src->fd_v4l < 0) {
		GST_ERROR_OBJECT(v4l2src, "capture_setup failed");
		return FALSE;
	}

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(v4l2src->fd_v4l, VIDIOC_G_FMT, &fmt) < 0) {
		GST_ERROR_OBJECT(v4l2src, "VIDIOC_G_FMT failed");
		return FALSE;
	}

	GST_DEBUG_OBJECT(v4l2src, "width = %d", fmt.fmt.pix.width);
	GST_DEBUG_OBJECT(v4l2src, "height = %d", fmt.fmt.pix.height);
	GST_DEBUG_OBJECT(v4l2src, "sizeimage = %d", fmt.fmt.pix.sizeimage);
	GST_DEBUG_OBJECT(v4l2src, "pixelformat = %d", fmt.fmt.pix.pixelformat);

	v4l2src->time_per_frame = gst_util_uint64_scale_int(GST_SECOND,
			v4l2src->fps_d, v4l2src->fps_n);
	v4l2src->count = 0;

	return TRUE;
}

static gboolean gst_imx_v4l2src_stop(GstBaseSrc *src)
{
	GstImxV4l2Src *v4l2src = GST_IMX_V4L2SRC(src);

	GST_LOG_OBJECT(v4l2src, "stop");

	close(v4l2src->fd_v4l);
	v4l2src->fd_v4l = -1;
	return TRUE;
}

static gboolean gst_imx_v4l2src_decide_allocation(GstBaseSrc *bsrc,
		GstQuery *query)
{
	GstImxV4l2Src *v4l2src = GST_IMX_V4L2SRC(bsrc);
	struct v4l2_format fmt;
	GstBufferPool *pool;
	guint size, min, max;
	gboolean update;
	GstStructure *config;
	GstCaps *caps;

	gst_query_parse_allocation(query, &caps, NULL);

	/* Determine min and max */
	if (gst_query_get_n_allocation_pools(query) > 0)
	{
		gst_query_parse_nth_allocation_pool(query, 0, NULL, NULL,
				&min, &max);
		update = TRUE;
	}
	else
	{
		min = max = 0;
		update = FALSE;
	}

	if (min != 0)
		/* Need an extra buffer to capture while other buffers
		 * are downstream */
		min += 1;
	else
		min = v4l2src->queue_size;

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(v4l2src->fd_v4l, VIDIOC_G_FMT, &fmt) < 0) {
		GST_ERROR_OBJECT(v4l2src, "VIDIOC_G_FMT failed");
		return FALSE;
	}

	size = fmt.fmt.pix.sizeimage;

	pool = gst_imx_v4l2_buffer_pool_new(v4l2src->fd_v4l);
	config = gst_buffer_pool_get_config(pool);
	gst_buffer_pool_config_set_params(config, caps, size, min, max);
	gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
	gst_buffer_pool_set_config(pool, config);

	if (update)
		gst_query_set_nth_allocation_pool(query, 0, pool, size, min, max);
	else
		gst_query_add_allocation_pool(query, pool, size, min, max);

	gst_object_unref(pool);

	return TRUE;
}

static GstFlowReturn gst_imx_v4l2src_fill(GstPushSrc *src, GstBuffer *buf)
{
	GstImxV4l2Src *v4l2src = GST_IMX_V4L2SRC(src);
	GstClockTime ts;

	GST_LOG_OBJECT(v4l2src, "fill");

	ts = gst_clock_get_time(GST_ELEMENT(v4l2src)->clock);
	if (ts != GST_CLOCK_TIME_NONE)
		ts -= gst_element_get_base_time(GST_ELEMENT(v4l2src));
	else
		ts = v4l2src->count * v4l2src->time_per_frame;
	v4l2src->count++;

	GST_BUFFER_TIMESTAMP(buf) = ts;
	GST_BUFFER_DURATION(buf) = v4l2src->time_per_frame;
	return GST_FLOW_OK;
}

static GstCaps *gst_imx_v4l2src_fixate(GstBaseSrc *src, GstCaps *caps)
{
	GstImxV4l2Src *v4l2src = GST_IMX_V4L2SRC(src);
	GstStructure *structure = NULL;
	guint i;

	GST_INFO_OBJECT(src, "fixating caps %" GST_PTR_FORMAT, (gpointer)caps);

	caps = gst_caps_make_writable(caps);

	for (i = 0; i < gst_caps_get_size(caps); ++i) {
		structure = gst_caps_get_structure(caps, i);
		gst_structure_fixate_field_nearest_int(structure, "width",
				v4l2src->capture_width);
		gst_structure_fixate_field_nearest_int(structure, "height",
				v4l2src->capture_height);
		gst_structure_fixate_field_nearest_fraction(structure, "framerate",
				v4l2src->fps_n, v4l2src->fps_d);
		gst_structure_fixate_field_nearest_fraction(structure,
				"pixel-aspect-ratio", 1, 1);
		gst_structure_set(structure, "format", G_TYPE_STRING, "I420", NULL);
	}

	GST_INFO_OBJECT(src, "fixated caps %" GST_PTR_FORMAT, (gpointer)caps);

	caps = GST_BASE_SRC_CLASS(gst_imx_v4l2src_parent_class)->fixate(src, caps);

	return caps;
}

static GstCaps *gst_imx_v4l2src_get_caps(GstBaseSrc *src, GstCaps *filter)
{
	GstImxV4l2Src *v4l2src = GST_IMX_V4L2SRC(src);
	GstCaps *caps;

	GST_INFO_OBJECT(v4l2src, "get caps filter %" GST_PTR_FORMAT, (gpointer)filter);

	caps = gst_caps_new_simple("video/x-raw",
			"format", G_TYPE_STRING, "I420",
			"width", GST_TYPE_INT_RANGE, 16, G_MAXINT,
			"height", GST_TYPE_INT_RANGE, 16, G_MAXINT,
			"framerate", GST_TYPE_FRACTION_RANGE, 0, 1, 100, 1,
			"pixel-aspect-ratio", GST_TYPE_FRACTION_RANGE, 0, 1, 100, 1,
			NULL);

	GST_INFO_OBJECT(v4l2src, "get caps %" GST_PTR_FORMAT, (gpointer)caps);

	return caps;
}

static gboolean gst_imx_v4l2src_set_caps(GstBaseSrc *src, GstCaps *caps)
{
	GstImxV4l2Src *v4l2src = GST_IMX_V4L2SRC(src);

	GST_INFO_OBJECT(v4l2src, "set caps %" GST_PTR_FORMAT, (gpointer)caps);

	return TRUE;
}

static void gst_imx_v4l2src_set_property(GObject *object, guint prop_id,
		const GValue *value, GParamSpec *pspec)
{
	GstImxV4l2Src *v4l2src = GST_IMX_V4L2SRC(object);

	switch (prop_id)
	{
		case IMX_V4L2SRC_CAPTURE_MODE:
			v4l2src->capture_mode = g_value_get_int(value);
			break;

		case IMX_V4L2SRC_FRAMERATE_NUM:
			v4l2src->fps_n = g_value_get_int(value);
			break;

		case IMX_V4L2SRC_INPUT:
			v4l2src->input = g_value_get_int(value);
			break;

		case IMX_V4L2SRC_DEVICE:
			if (v4l2src->devicename)
				g_free(v4l2src->devicename);
			v4l2src->devicename = g_strdup(g_value_get_string(value));
			break;

		case IMX_V4L2SRC_QUEUE_SIZE:
			v4l2src->queue_size = g_value_get_int(value);
			break;

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}

static void gst_imx_v4l2src_get_property(GObject *object, guint prop_id,
		GValue *value, GParamSpec *pspec)
{
	GstImxV4l2Src *v4l2src = GST_IMX_V4L2SRC(object);

	switch (prop_id)
	{
		case IMX_V4L2SRC_CAPTURE_MODE:
			g_value_set_int(value, v4l2src->capture_mode);
			break;

		case IMX_V4L2SRC_FRAMERATE_NUM:
			g_value_set_int(value, v4l2src->fps_n);
			break;

		case IMX_V4L2SRC_INPUT:
			g_value_set_int(value, v4l2src->input);
			break;

		case IMX_V4L2SRC_DEVICE:
			g_value_set_string(value, v4l2src->devicename);
			break;

		case IMX_V4L2SRC_QUEUE_SIZE:
			g_value_set_int(value, v4l2src->queue_size);
			break;

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}

static void gst_imx_v4l2src_init(GstImxV4l2Src *v4l2src)
{
	v4l2src->capture_mode = DEFAULT_CAPTURE_MODE;
	v4l2src->fps_n = DEFAULT_FRAMERATE_NUM;
	v4l2src->fps_d = DEFAULT_FRAMERATE_DEN;
	v4l2src->input = DEFAULT_INPUT;
	v4l2src->devicename = g_strdup(DEFAULT_DEVICE);
	v4l2src->queue_size = DEFAULT_QUEUE_SIZE;

	gst_base_src_set_format(GST_BASE_SRC(v4l2src), GST_FORMAT_TIME);
	gst_base_src_set_live(GST_BASE_SRC(v4l2src), TRUE);
}

static void gst_imx_v4l2src_class_init(GstImxV4l2SrcClass *klass)
{
	GObjectClass *gobject_class;
	GstElementClass *element_class;
	GstBaseSrcClass *basesrc_class;
	GstPushSrcClass *pushsrc_class;

	gobject_class = G_OBJECT_CLASS(klass);
	element_class = GST_ELEMENT_CLASS(klass);
	basesrc_class = GST_BASE_SRC_CLASS(klass);
	pushsrc_class = GST_PUSH_SRC_CLASS(klass);

	gobject_class->set_property = gst_imx_v4l2src_set_property;
	gobject_class->get_property = gst_imx_v4l2src_get_property;

	g_object_class_install_property(gobject_class, IMX_V4L2SRC_CAPTURE_MODE,
			g_param_spec_int("capture-mode", "Capture mode",
				"Capture mode of camera, varies with each v4l2 driver,\n"
				"\t\t\t\tfor example ov5460:\n   "
				"\t\t\t\tov5640_mode_VGA_640_480 = 0,\n"
				"\t\t\t\tov5640_mode_QVGA_320_240 = 1,\n"
				"\t\t\t\tov5640_mode_NTSC_720_480 = 2,\n"
				"\t\t\t\tov5640_mode_PAL_720_576 = 3,\n"
				"\t\t\t\tov5640_mode_720P_1280_720 = 4,\n"
				"\t\t\t\tov5640_mode_1080P_1920_1080 = 5",
				0, G_MAXINT, DEFAULT_CAPTURE_MODE,
				G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property(gobject_class, IMX_V4L2SRC_FRAMERATE_NUM,
			g_param_spec_int("fps-n", "FPS numerator",
				"Numerator of the framerate at which"
				"the input stream is to be captured",
				0, G_MAXINT, DEFAULT_FRAMERATE_NUM,
				G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property(gobject_class, IMX_V4L2SRC_INPUT,
			g_param_spec_int("input", "Input",
				"Video input selected with VIDIOC_S_INPUT",
				0, G_MAXINT, DEFAULT_INPUT,
				G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property(gobject_class, IMX_V4L2SRC_DEVICE,
			g_param_spec_string("device", "Device", "Device location",
				DEFAULT_DEVICE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property(gobject_class, IMX_V4L2SRC_QUEUE_SIZE,
			g_param_spec_int("queue-size", "Queue size",
				"Number of V4L2 buffers to request",
				0, G_MAXINT, DEFAULT_QUEUE_SIZE,
				G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	basesrc_class->fixate = gst_imx_v4l2src_fixate;
	basesrc_class->get_caps = gst_imx_v4l2src_get_caps;
	basesrc_class->set_caps = gst_imx_v4l2src_set_caps;
	basesrc_class->start = gst_imx_v4l2src_start;
	basesrc_class->stop = gst_imx_v4l2src_stop;
	basesrc_class->decide_allocation = gst_imx_v4l2src_decide_allocation;
	pushsrc_class->fill = gst_imx_v4l2src_fill;

	gst_element_class_set_static_metadata(element_class,
			"V4L2 CSI Video Source",
			"Source/Video",
			"Capture video streams using V4L2 CSI interface",
			"Philip Craig <phil@blackmoth.com.au>");

	gst_element_class_add_pad_template(element_class,
			gst_static_pad_template_get(&src_template));

	return;
}

static gboolean plugin_init(GstPlugin *plugin)
{
	return gst_element_register(plugin, "imxv4l2src", GST_RANK_PRIMARY,
			gst_imx_v4l2src_get_type());
}

GST_PLUGIN_DEFINE(
		GST_VERSION_MAJOR,
		GST_VERSION_MINOR,
		imxv4l2src,
		"GStreamer i.MX V4L2 CSI video source",
		plugin_init,
		VERSION,
		"LGPL",
		GST_PACKAGE_NAME,
		GST_PACKAGE_ORIGIN
)
