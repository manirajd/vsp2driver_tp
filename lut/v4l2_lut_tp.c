/*
 * Copyright (c) 2016-2017 Renesas Electronics Corporation
 * Released under the MIT license
 * http://opensource.org/licenses/mit-license.php
 */

/******************************************************************************
 *  link state  : rpf -> lut -> wpf
 *  memory type : mmap / userptr / dmabuf
 ******************************************************************************/
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <mediactl/mediactl.h>
#include <mediactl/v4l2subdev.h>

#include "mmngr_user_public.h"
#include "mmngr_buf_user_public.h"

/******************************************************************************
 *  macros
 ******************************************************************************/
/* device name */
#ifndef USE_M3
/* for h3 */
#define MEDIA_DEV_NAME		"/dev/media3"		/* fe9a0000.vsp */
#else
/* for m3 */
#define MEDIA_DEV_NAME		"/dev/media2"		/* fe9a0000.vsp */
#endif

#define SRC_INPUT_DEV		"%s rpf.0 input"
#define DST_OUTPUT_DEV		"%s wpf.0 output"
#define LUT_DEV			"%s lut"

/* source parameter */
#define SRC_FILENAME		"1280_720_ARGB32.argb"
#define SRC_WIDTH		(1280)			/* src: width */
#define SRC_HEIGHT		(720)			/* src: height */
#define SRC_SIZE		(SRC_WIDTH*SRC_HEIGHT*4)

/* destination parameter */
#define DST_FILENAME_MMAP	"1280_720_ARGB32_LUT_MMAP.argb"
#define DST_FILENAME_USERPTR	"1280_720_ARGB32_LUT_USERPTR.argb"
#define DST_FILENAME_DMABUF	"1280_720_ARGB32_LUT_DMABUF.argb"
#define DST_WIDTH		(1280)			/* dst: width */
#define DST_HEIGHT		(720)			/* dst: height */
#define DST_SIZE		(DST_WIDTH*DST_HEIGHT*4)

/* ioctl */
#define VIDIOC_VSP2_LUT_CONFIG \
	_IOWR('V', BASE_VIDIOC_PRIVATE + 1, struct vsp2_lut_config)

/******************************************************************************
 *  structure
 ******************************************************************************/
struct dev_pads {
	struct media_pad	*ppad0;
	struct media_pad	*ppad1;
};

struct vsp2_lut_config {
	void		*addr;	/* Allocate memory size is tbl_num * 8 bytes. */
	unsigned short	tbl_num;	/* 1 to 256 */
	unsigned char	fxa;
};

/******************************************************************************
 *  internal function
 ******************************************************************************/
static int	test_lut_mmap(void);
static int	test_lut_userptr(void);
static int	test_lut_dmabuf(void);
static int	read_file(unsigned char*, unsigned int, const char*);
static int	write_file(unsigned char*, unsigned int, const char*);
static int	call_media_ctl(struct media_device **, const char **);
static int	set_lut(struct media_device *pmedia, void *plut_table,
			char *pentity_base, const char *pmedia_name);
static int	open_video_device(struct media_device *pmedia,
				  char *pentity_base, const char *pmedia_name);

/******************************************************************************
 *  main
 ******************************************************************************/
void print_usage(const char *pname)
{
	printf("----------------------------------\n");
#ifndef USE_M3
	printf(" exec for H3 settings\n");
#else
	printf(" exec for M3 settings\n");
#endif
	printf("----------------------------------\n");
	printf(" Usage : %s [option]\n", pname);
	printf("    option\n");
	printf("        -m: use MMAP [default]\n");
	printf("        -u: use USERPTR\n");
	printf("        -d: use DMABUF\n");
	printf("        -h: print usage\n");
	printf("----------------------------------\n");
}

int main(int argc, char *argv[])
{
	int opt;

	opt = getopt(argc, argv, "mudh");

	switch (opt) {
	case 'm':
		printf("exec MMAP\n");
		test_lut_mmap();
		break;
	case 'u':
		printf("exec USERPTR\n");
		test_lut_userptr();
		break;
	case 'd':
		printf("exec DMABUF\n");
		test_lut_dmabuf();
		break;
	case 'h':
		print_usage(argv[0]);
		break;
	default:
		print_usage(argv[0]);
		printf("exec MMAP\n");
		test_lut_mmap();
		break;
	}

	exit(0);
}

/******************************************************************************
 *  mmap
 ******************************************************************************/
static int test_lut_mmap(void)
{
	struct media_device  *pmedia;

	unsigned char  *psrc_buf;
	unsigned char  *pdst_buf;

	int src_fd = -1;	/* src file descriptor */
	int dst_fd = -1;	/* dst file descriptor */

	unsigned int  type;
	int           ret = -1;

	struct v4l2_format          fmt;
	struct v4l2_requestbuffers  req_buf;
	struct v4l2_buffer          buf;
	struct v4l2_capability      cap;
	struct v4l2_plane           planes[VIDEO_MAX_PLANES];

	unsigned int        caps;
	struct v4l2_format  gfmt;

	unsigned char	*plut_table = NULL;

	const char *pmedia_name;

	/*-------------------------------------------------------------------*/
	/*  Allocate memory for lookup table                                 */
	/*-------------------------------------------------------------------*/
	plut_table = malloc(256*8);
	if (plut_table == NULL) {
		printf("Error : malloc()\n");
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Call media-ctl                                                   */
	/*-------------------------------------------------------------------*/
	ret = call_media_ctl(&pmedia, &pmedia_name);
	if (ret < 0) {
		printf("Error : media-ctl call failed.\n");
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Open device                                                      */
	/*-------------------------------------------------------------------*/
	/* src device(rpf.0) */
	src_fd = open_video_device(pmedia, SRC_INPUT_DEV, pmedia_name);
	if (src_fd == -1) {
		printf("Error open src device: %s (%d).\n",
			strerror(errno), errno);
		return -1;
	}

	/* dst device(wpf.0) */
	dst_fd = open_video_device(pmedia, DST_OUTPUT_DEV, pmedia_name);
	if (dst_fd == -1) {
		printf("Error open dst device: %s (%d).\n",
			strerror(errno), errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_QUERYCAP                                                  */
	/*-------------------------------------------------------------------*/
	/* src */
	memset(&cap, 0, sizeof(cap));
	ret = ioctl(src_fd, VIDIOC_QUERYCAP, &cap);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}
	caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS
	     ? cap.device_caps : cap.capabilities;

	if ((caps & V4L2_CAP_VIDEO_OUTPUT_MPLANE) == 0) {
		printf("Device does not have required capabilitiy. line=%d\n",
			__LINE__);
		return -1;
	}

	/* dst */
	ret = ioctl(dst_fd, VIDIOC_QUERYCAP, &cap);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}
	caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS
	     ? cap.device_caps : cap.capabilities;

	if ((caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) == 0) {
		printf("Device does not have required capabilitiy. line=%d\n",
			__LINE__);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Make lookup table - VIDIOC_VSP2_LUT_CONFIG                       */
	/*-------------------------------------------------------------------*/
	ret = set_lut(pmedia, plut_table, LUT_DEV, pmedia_name);
	if (ret == -1) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*********************************************************************
	 *  src
	 *********************************************************************/
	/*-------------------------------------------------------------------*/
	/*  VIDIOC_S_FMT                                                     */
	/*-------------------------------------------------------------------*/
	memset(&fmt, 0, sizeof(fmt));
	fmt.type			= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	fmt.fmt.pix_mp.width		= SRC_WIDTH;
	fmt.fmt.pix_mp.height		= SRC_HEIGHT;
	fmt.fmt.pix_mp.field		= V4L2_FIELD_ANY;
	fmt.fmt.pix_mp.pixelformat	= V4L2_PIX_FMT_ARGB32;
	fmt.fmt.pix_mp.num_planes	= 1;
	fmt.fmt.pix_mp.plane_fmt[0].bytesperline	= 0;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage		= 0;

	ret = ioctl(src_fd, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_G_FMT                                                     */
	/*-------------------------------------------------------------------*/
	memset(&gfmt, 0x00, sizeof(gfmt));
	gfmt.type = fmt.type;
	ret = ioctl(src_fd, VIDIOC_G_FMT, &gfmt);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	if ((fmt.fmt.pix_mp.width        != gfmt.fmt.pix_mp.width)       ||
	    (fmt.fmt.pix_mp.height       != gfmt.fmt.pix_mp.height)      ||
	    (fmt.fmt.pix_mp.field        != gfmt.fmt.pix_mp.field)       ||
	    (fmt.fmt.pix_mp.pixelformat  != gfmt.fmt.pix_mp.pixelformat) ||
	    (fmt.fmt.pix_mp.num_planes   != gfmt.fmt.pix_mp.num_planes)  ||
	    (fmt.fmt.pix_mp.flags        != gfmt.fmt.pix_mp.flags)) {
		printf("Get format error. line=%d\n", __LINE__);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_REQBUFS (alloc)                                           */
	/*-------------------------------------------------------------------*/
	memset(&req_buf, 0, sizeof(req_buf));
	req_buf.count	= 1;		/* Request 1 buffers */
	req_buf.type	= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	req_buf.memory	= V4L2_MEMORY_MMAP;

	ret = ioctl(src_fd, VIDIOC_REQBUFS, &req_buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_QUERYBUF                                                  */
	/*-------------------------------------------------------------------*/
	memset(&buf, 0, sizeof(buf));
	memset(planes, 0, sizeof(planes));
	buf.index			= 0;
	buf.type			= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory			= V4L2_MEMORY_MMAP;
	buf.length			= VIDEO_MAX_PLANES;
	buf.m.planes			= planes;
	buf.m.planes[0].bytesused	= 0;

	ret = ioctl(src_fd, VIDIOC_QUERYBUF, &buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Mmap for source buffer                                           */
	/*-------------------------------------------------------------------*/
	psrc_buf = mmap(0, SRC_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		src_fd, 0);
	if (psrc_buf == MAP_FAILED) {
		printf("Error(%d) : mmap", __LINE__);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Read file                                                        */
	/*-------------------------------------------------------------------*/
	ret = read_file(psrc_buf, SRC_SIZE, SRC_FILENAME);
	if (ret == 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_QBUF                                                      */
	/*-------------------------------------------------------------------*/
	memset(&buf, 0, sizeof(buf));
	memset(planes, 0, sizeof(planes));

	buf.m.planes	= planes;
	buf.index	= 0;
	buf.type	= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory	= V4L2_MEMORY_MMAP;
	buf.flags	= 0;
	buf.length	= 1;	/* Number of elements in the planes array. */
	buf.m.planes[0].bytesused	= SRC_SIZE;
	buf.bytesused			= SRC_SIZE;

	ret = ioctl(src_fd, VIDIOC_QBUF, &buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_STREAMON                                                  */
	/*-------------------------------------------------------------------*/
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	ret = ioctl(src_fd, VIDIOC_STREAMON, &type);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*********************************************************************
	 *  dst
	 *********************************************************************/
	/*-------------------------------------------------------------------*/
	/*  VIDIOC_S_FMT                                                     */
	/*-------------------------------------------------------------------*/
	memset(&fmt, 0, sizeof(fmt));
	fmt.type			= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width		= DST_WIDTH;
	fmt.fmt.pix_mp.height		= DST_HEIGHT;
	fmt.fmt.pix_mp.pixelformat	= V4L2_PIX_FMT_ARGB32;
	fmt.fmt.pix_mp.field		= V4L2_FIELD_ANY;
	fmt.fmt.pix_mp.num_planes	= 1;		/* argb32 */
	fmt.fmt.pix_mp.plane_fmt[0].bytesperline	= 0;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage		= 0;

	ret = ioctl(dst_fd, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_G_FMT                                                     */
	/*-------------------------------------------------------------------*/
	memset(&gfmt, 0x00, sizeof(gfmt));
	gfmt.type = fmt.type;
	ret = ioctl(dst_fd, VIDIOC_G_FMT, &gfmt);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	if ((fmt.fmt.pix_mp.width        != gfmt.fmt.pix_mp.width)       ||
	    (fmt.fmt.pix_mp.height       != gfmt.fmt.pix_mp.height)      ||
	    (fmt.fmt.pix_mp.field        != gfmt.fmt.pix_mp.field)       ||
	    (fmt.fmt.pix_mp.pixelformat  != gfmt.fmt.pix_mp.pixelformat) ||
	    (fmt.fmt.pix_mp.num_planes   != gfmt.fmt.pix_mp.num_planes)  ||
	    (fmt.fmt.pix_mp.flags        != gfmt.fmt.pix_mp.flags)) {
		printf("Get format error. line=%d\n", __LINE__);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_REQBUFS (alloc)                                           */
	/*-------------------------------------------------------------------*/
	memset(&req_buf, 0, sizeof(req_buf));
	req_buf.count	= 1;		/* Request 1 buffers */
	req_buf.type	= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req_buf.memory	= V4L2_MEMORY_MMAP;

	ret = ioctl(dst_fd, VIDIOC_REQBUFS, &req_buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_QUERYBUF                                                  */
	/*-------------------------------------------------------------------*/
	memset(&buf, 0, sizeof(buf));
	memset(planes, 0, sizeof(planes));
	buf.index			= 0;
	buf.type			= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory			= V4L2_MEMORY_MMAP;
	buf.length			= VIDEO_MAX_PLANES;
	buf.m.planes			= planes;
	buf.m.planes[0].bytesused	= 0;

	ret = ioctl(dst_fd, VIDIOC_QUERYBUF, &buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Mmap for destination buffer                                      */
	/*-------------------------------------------------------------------*/
	pdst_buf = mmap(0, DST_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		dst_fd, 0);
	if (pdst_buf == MAP_FAILED) {
		printf("Error(%d) : mmap", __LINE__);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_QBUF                                                      */
	/*-------------------------------------------------------------------*/
	memset(&buf, 0, sizeof(buf));
	memset(planes, 0, sizeof(planes));

	buf.m.planes	= planes;
	buf.index	= 0;
	buf.type	= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory	= V4L2_MEMORY_MMAP;
	buf.length	= 1;	/* Number of elements in the planes array. */
	buf.m.planes[0].bytesused	= DST_SIZE;

	ret = ioctl(dst_fd, VIDIOC_QBUF, &buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_STREAMON                                                  */
	/*-------------------------------------------------------------------*/
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(dst_fd, VIDIOC_STREAMON, &type);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_DQBUF                                                     */
	/*-------------------------------------------------------------------*/
	memset(&buf, 0, sizeof(buf));
	memset(planes, 0, sizeof(planes));
	buf.m.planes	= planes;
	buf.type	= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory	= V4L2_MEMORY_MMAP;
	buf.length	= VIDEO_MAX_PLANES;

	ret = ioctl(dst_fd, VIDIOC_DQBUF, &buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Write file                                                       */
	/*-------------------------------------------------------------------*/
	ret = write_file(pdst_buf, DST_SIZE, DST_FILENAME_MMAP);
	if (ret == 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*********************************************************************
	 *  src
	 *********************************************************************/
	/*-------------------------------------------------------------------*/
	/*  VIDIOC_DQBUF                                                     */
	/*-------------------------------------------------------------------*/
	memset(&buf, 0, sizeof(buf));
	memset(planes, 0, sizeof(planes));
	buf.m.planes	= planes;
	buf.type	= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory	= V4L2_MEMORY_MMAP;
	buf.length	= 1;

	ret = ioctl(src_fd, VIDIOC_DQBUF, &buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_STREAMOFF                                                 */
	/*-------------------------------------------------------------------*/
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ret = ioctl(src_fd, VIDIOC_STREAMOFF, &type);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Unmap buffer                                                     */
	/*-------------------------------------------------------------------*/
	munmap(psrc_buf, SRC_SIZE);

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_REQBUFS (release)                                         */
	/*-------------------------------------------------------------------*/
	memset(&req_buf, 0, sizeof(req_buf));
	req_buf.count	= 0;		/* Release buffers */
	req_buf.type	= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	req_buf.memory	= V4L2_MEMORY_MMAP;

	ret = ioctl(src_fd, VIDIOC_REQBUFS, &req_buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*********************************************************************
	 *  dst
	 *********************************************************************/
	/*-------------------------------------------------------------------*/
	/*  VIDIOC_STREAMOFF                                                 */
	/*-------------------------------------------------------------------*/
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(dst_fd, VIDIOC_STREAMOFF, &type);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Unmap buffer                                                     */
	/*-------------------------------------------------------------------*/
	munmap(pdst_buf, DST_SIZE);

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_REQBUFS (release)                                         */
	/*-------------------------------------------------------------------*/
	memset(&req_buf, 0, sizeof(req_buf));
	req_buf.count	= 0;		/* Release buffers */
	req_buf.type	= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req_buf.memory	= V4L2_MEMORY_MMAP;
	ret = ioctl(dst_fd, VIDIOC_REQBUFS, &req_buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Release memory for lookup table                                  */
	/*-------------------------------------------------------------------*/
	free(plut_table);

	close(src_fd);
	close(dst_fd);

	media_device_unref(pmedia);

	return 0;
}

/******************************************************************************
 *  userptr
 ******************************************************************************/
static int test_lut_userptr(void)
{
	struct media_device  *pmedia;

	unsigned char  *psrc_buf;
	unsigned char  *pdst_buf;

	int src_fd = -1;	/* src file descriptor */
	int dst_fd = -1;	/* dst file descriptor */

	unsigned int  type;
	int           ret = -1;

	struct v4l2_format          fmt;
	struct v4l2_requestbuffers  req_buf;
	struct v4l2_buffer          buf;
	struct v4l2_capability      cap;
	struct v4l2_plane           planes[VIDEO_MAX_PLANES];

	unsigned int        caps;
	struct v4l2_format  gfmt;

	MMNGR_ID	srcfd;
	unsigned long	src_phys;
	unsigned long	src_hard;
	unsigned long	src_virt;

	MMNGR_ID	dstfd;
	unsigned long	dst_phys;
	unsigned long	dst_hard;
	unsigned long	dst_virt;

	unsigned char	*plut_table = NULL;

	const char *pmedia_name;

	/*-------------------------------------------------------------------*/
	/*  Allocate memory for lookup table                                 */
	/*-------------------------------------------------------------------*/
	plut_table = malloc(256*8);
	if (plut_table == NULL) {
		printf("Error : malloc()\n");
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Call media-ctl                                                   */
	/*-------------------------------------------------------------------*/
	ret = call_media_ctl(&pmedia, &pmedia_name);
	if (ret < 0) {
		printf("Error : media-ctl call failed.\n");
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Open device                                                      */
	/*-------------------------------------------------------------------*/
	/* src device(rpf.0) */
	src_fd = open_video_device(pmedia, SRC_INPUT_DEV, pmedia_name);
	if (src_fd == -1) {
		printf("Error open src device: %s (%d).\n",
			strerror(errno), errno);
		return -1;
	}

	/* dst device(wpf.0) */
	dst_fd = open_video_device(pmedia, DST_OUTPUT_DEV, pmedia_name);
	if (dst_fd == -1) {
		printf("Error open dst device: %s (%d).\n",
			strerror(errno), errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_QUERYCAP                                                  */
	/*-------------------------------------------------------------------*/
	/* src */
	memset(&cap, 0, sizeof(cap));
	ret = ioctl(src_fd, VIDIOC_QUERYCAP, &cap);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}
	caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS
	     ? cap.device_caps : cap.capabilities;

	if ((caps & V4L2_CAP_VIDEO_OUTPUT_MPLANE) == 0) {
		printf("Device does not have required capabilitiy. line=%d\n",
			__LINE__);
		return -1;
	}

	/* dst */
	ret = ioctl(dst_fd, VIDIOC_QUERYCAP, &cap);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}
	caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS
	     ? cap.device_caps : cap.capabilities;

	if ((caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) == 0) {
		printf("Device does not have required capabilitiy. line=%d\n",
			__LINE__);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Make lookup table - VIDIOC_VSP2_LUT_CONFIG                       */
	/*-------------------------------------------------------------------*/
	ret = set_lut(pmedia, plut_table, LUT_DEV, pmedia_name);
	if (ret == -1) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*********************************************************************
	 *  src
	 *********************************************************************/
	/*-------------------------------------------------------------------*/
	/*  VIDIOC_S_FMT                                                     */
	/*-------------------------------------------------------------------*/
	memset(&fmt, 0, sizeof(fmt));
	fmt.type			= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	fmt.fmt.pix_mp.width		= SRC_WIDTH;
	fmt.fmt.pix_mp.height		= SRC_HEIGHT;
	fmt.fmt.pix_mp.field		= V4L2_FIELD_ANY;
	fmt.fmt.pix_mp.pixelformat	= V4L2_PIX_FMT_ARGB32;
	fmt.fmt.pix_mp.num_planes	= 1;
	fmt.fmt.pix_mp.plane_fmt[0].bytesperline	= 0;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage		= 0;

	ret = ioctl(src_fd, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_G_FMT                                                     */
	/*-------------------------------------------------------------------*/
	memset(&gfmt, 0x00, sizeof(gfmt));
	gfmt.type = fmt.type;
	ret = ioctl(src_fd, VIDIOC_G_FMT, &gfmt);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	if ((fmt.fmt.pix_mp.width        != gfmt.fmt.pix_mp.width)       ||
	    (fmt.fmt.pix_mp.height       != gfmt.fmt.pix_mp.height)      ||
	    (fmt.fmt.pix_mp.field        != gfmt.fmt.pix_mp.field)       ||
	    (fmt.fmt.pix_mp.pixelformat  != gfmt.fmt.pix_mp.pixelformat) ||
	    (fmt.fmt.pix_mp.num_planes   != gfmt.fmt.pix_mp.num_planes)  ||
	    (fmt.fmt.pix_mp.flags        != gfmt.fmt.pix_mp.flags)) {
		printf("Get format error. line=%d\n", __LINE__);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_REQBUFS (alloc)                                           */
	/*-------------------------------------------------------------------*/
	memset(&req_buf, 0, sizeof(req_buf));
	req_buf.count	= 1;		/* Request 1 buffers */
	req_buf.type	= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	req_buf.memory	= V4L2_MEMORY_USERPTR;

	ret = ioctl(src_fd, VIDIOC_REQBUFS, &req_buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Allocate memory by mmngr                                         */
	/*-------------------------------------------------------------------*/
	ret = mmngr_alloc_in_user(
		&srcfd, SRC_SIZE,
		&src_phys, &src_hard, &src_virt, MMNGR_VA_SUPPORT);
	if (ret) {
		printf("error line=%d errcode=(%d)\n", __LINE__, ret);
		return -1;
	}
	psrc_buf = (void *)src_virt;

	/*-------------------------------------------------------------------*/
	/*  Read file                                                        */
	/*-------------------------------------------------------------------*/
	ret = read_file(psrc_buf, SRC_SIZE, SRC_FILENAME);
	if (ret == 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_QBUF                                                      */
	/*-------------------------------------------------------------------*/
	memset(&buf, 0, sizeof(buf));
	memset(planes, 0, sizeof(planes));

	buf.m.planes	= planes;
	buf.index	= 0;
	buf.type	= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory	= V4L2_MEMORY_USERPTR;
	buf.flags	= 0;
	buf.length	= 1;	/* Number of elements in the planes array. */
	buf.m.planes[0].bytesused	= SRC_SIZE;
	buf.m.planes[0].length		= SRC_SIZE;
	buf.m.planes[0].m.userptr	= (unsigned long)psrc_buf;
	buf.bytesused			= SRC_SIZE;

	ret = ioctl(src_fd, VIDIOC_QBUF, &buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_STREAMON                                                  */
	/*-------------------------------------------------------------------*/
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	ret = ioctl(src_fd, VIDIOC_STREAMON, &type);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*********************************************************************
	 *  dst
	 *********************************************************************/
	/*-------------------------------------------------------------------*/
	/*  VIDIOC_S_FMT                                                     */
	/*-------------------------------------------------------------------*/
	memset(&fmt, 0, sizeof(fmt));
	fmt.type			= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width		= DST_WIDTH;
	fmt.fmt.pix_mp.height		= DST_HEIGHT;
	fmt.fmt.pix_mp.pixelformat	= V4L2_PIX_FMT_ARGB32;
	fmt.fmt.pix_mp.field		= V4L2_FIELD_ANY;
	fmt.fmt.pix_mp.num_planes	= 1;		/* argb32 */
	fmt.fmt.pix_mp.plane_fmt[0].bytesperline	= 0;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage		= 0;

	ret = ioctl(dst_fd, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_G_FMT                                                     */
	/*-------------------------------------------------------------------*/
	memset(&gfmt, 0x00, sizeof(gfmt));
	gfmt.type = fmt.type;
	ret = ioctl(dst_fd, VIDIOC_G_FMT, &gfmt);
	if (ret < 0) {
		printf("error (%d) line=%d\n", ret, __LINE__);
		return -1;
	}

	if ((fmt.fmt.pix_mp.width        != gfmt.fmt.pix_mp.width)       ||
	    (fmt.fmt.pix_mp.height       != gfmt.fmt.pix_mp.height)      ||
	    (fmt.fmt.pix_mp.field        != gfmt.fmt.pix_mp.field)       ||
	    (fmt.fmt.pix_mp.pixelformat  != gfmt.fmt.pix_mp.pixelformat) ||
	    (fmt.fmt.pix_mp.num_planes   != gfmt.fmt.pix_mp.num_planes)  ||
	    (fmt.fmt.pix_mp.flags        != gfmt.fmt.pix_mp.flags)) {
		printf("Get format error. line=%d\n", __LINE__);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_REQBUFS (alloc)                                           */
	/*-------------------------------------------------------------------*/
	memset(&req_buf, 0, sizeof(req_buf));
	req_buf.count	= 1;		/* Request 1 buffers */
	req_buf.type	= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req_buf.memory	= V4L2_MEMORY_USERPTR;

	ret = ioctl(dst_fd, VIDIOC_REQBUFS, &req_buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Allocate memory by mmngr                                         */
	/*-------------------------------------------------------------------*/
	ret = mmngr_alloc_in_user(
		&dstfd, DST_SIZE,
		&dst_phys, &dst_hard, &dst_virt, MMNGR_VA_SUPPORT);
	if (ret) {
		printf("error line=%d errcode=(%d)\n", __LINE__, ret);
		return -1;
	}
	pdst_buf = (void *)dst_virt;

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_QBUF                                                      */
	/*-------------------------------------------------------------------*/
	memset(&buf, 0, sizeof(buf));
	memset(planes, 0, sizeof(planes));

	buf.m.planes	= planes;
	buf.index	= 0;
	buf.type	= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory	= V4L2_MEMORY_USERPTR;
	buf.length	= 1;	/* Number of elements in the planes array. */
	buf.m.planes[0].bytesused	= DST_SIZE;
	buf.m.planes[0].length		= DST_SIZE;
	buf.m.planes[0].m.userptr	= (unsigned long)pdst_buf;

	ret = ioctl(dst_fd, VIDIOC_QBUF, &buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_STREAMON                                                  */
	/*-------------------------------------------------------------------*/
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(dst_fd, VIDIOC_STREAMON, &type);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_DQBUF                                                     */
	/*-------------------------------------------------------------------*/
	memset(&buf, 0, sizeof(buf));
	memset(planes, 0, sizeof(planes));
	buf.m.planes	= planes;
	buf.type	= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory	= V4L2_MEMORY_USERPTR;
	buf.length	= VIDEO_MAX_PLANES;

	ret = ioctl(dst_fd, VIDIOC_DQBUF, &buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Write file                                                       */
	/*-------------------------------------------------------------------*/
	ret = write_file(pdst_buf, DST_SIZE, DST_FILENAME_USERPTR);
	if (ret == 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*********************************************************************
	 *  src
	 *********************************************************************/
	/*-------------------------------------------------------------------*/
	/*  VIDIOC_DQBUF                                                     */
	/*-------------------------------------------------------------------*/
	memset(&buf, 0, sizeof(buf));
	memset(planes, 0, sizeof(planes));
	buf.m.planes	= planes;
	buf.type	= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory	= V4L2_MEMORY_USERPTR;
	buf.length	= 1;

	ret = ioctl(src_fd, VIDIOC_DQBUF, &buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_STREAMOFF                                                 */
	/*-------------------------------------------------------------------*/
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ret = ioctl(src_fd, VIDIOC_STREAMOFF, &type);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_REQBUFS (release)                                         */
	/*-------------------------------------------------------------------*/
	memset(&req_buf, 0, sizeof(req_buf));
	req_buf.count	= 0;		/* Release buffers */
	req_buf.type	= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	req_buf.memory	= V4L2_MEMORY_USERPTR;

	ret = ioctl(src_fd, VIDIOC_REQBUFS, &req_buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Free buffer                                                      */
	/*-------------------------------------------------------------------*/
	ret = mmngr_free_in_user(srcfd);
	if (ret < 0) {
		printf("error line=%d errcode=(%d)\n", __LINE__, ret);
		return -1;
	}

	/*********************************************************************
	 *  dst
	 *********************************************************************/
	/*-------------------------------------------------------------------*/
	/*  VIDIOC_STREAMOFF                                                 */
	/*-------------------------------------------------------------------*/
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(dst_fd, VIDIOC_STREAMOFF, &type);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_REQBUFS (release)                                         */
	/*-------------------------------------------------------------------*/
	memset(&req_buf, 0, sizeof(req_buf));
	req_buf.count	= 0;		/* Release buffers */
	req_buf.type	= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req_buf.memory	= V4L2_MEMORY_USERPTR;
	ret = ioctl(dst_fd, VIDIOC_REQBUFS, &req_buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Free buffer                                                      */
	/*-------------------------------------------------------------------*/
	ret = mmngr_free_in_user(dstfd);
	if (ret < 0) {
		printf("error line=%d errcode=(%d)\n", __LINE__, ret);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Release memory for lookup table                                  */
	/*-------------------------------------------------------------------*/
	free(plut_table);

	close(src_fd);
	close(dst_fd);

	media_device_unref(pmedia);

	return 0;
}

/******************************************************************************
 *  dmabuf
 ******************************************************************************/
static int test_lut_dmabuf(void)
{
	struct media_device  *pmedia;

	unsigned char  *psrc_buf;
	unsigned char  *pdst_buf;

	int src_fd = -1;	/* src file descriptor */
	int dst_fd = -1;	/* dst file descriptor */

	unsigned int  type;
	int           ret = -1;

	struct v4l2_format          fmt;
	struct v4l2_requestbuffers  req_buf;
	struct v4l2_buffer          buf;
	struct v4l2_capability      cap;
	struct v4l2_plane           planes[VIDEO_MAX_PLANES];

	unsigned int        caps;
	struct v4l2_format  gfmt;

	MMNGR_ID	srcfd;
	unsigned long	src_phys;
	unsigned long	src_hard;
	unsigned long	src_virt;
	int		src_mbid;
	int		src_dmafd;

	MMNGR_ID	dstfd;
	unsigned long	dst_phys;
	unsigned long	dst_hard;
	unsigned long	dst_virt;
	int		dst_mbid;
	int		dst_dmafd;

	unsigned char	*plut_table = NULL;

	const char *pmedia_name;

	/*-------------------------------------------------------------------*/
	/*  Allocate memory for lookup table                                 */
	/*-------------------------------------------------------------------*/
	plut_table = malloc(256*8);
	if (plut_table == NULL) {
		printf("Error : malloc()\n");
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Call media-ctl                                                   */
	/*-------------------------------------------------------------------*/
	ret = call_media_ctl(&pmedia, &pmedia_name);
	if (ret < 0) {
		printf("Error : media-ctl call failed.\n");
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Open device                                                      */
	/*-------------------------------------------------------------------*/
	/* src device(rpf.0) */
	src_fd = open_video_device(pmedia, SRC_INPUT_DEV, pmedia_name);
	if (src_fd == -1) {
		printf("Error open src device: %s (%d).\n",
			strerror(errno), errno);
		return -1;
	}

	/* dst device(wpf.0) */
	dst_fd = open_video_device(pmedia, DST_OUTPUT_DEV, pmedia_name);
	if (dst_fd == -1) {
		printf("Error open dst device: %s (%d).\n",
			strerror(errno), errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_QUERYCAP                                                  */
	/*-------------------------------------------------------------------*/
	/* src */
	memset(&cap, 0, sizeof(cap));
	ret = ioctl(src_fd, VIDIOC_QUERYCAP, &cap);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}
	caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS
	     ? cap.device_caps : cap.capabilities;

	if ((caps & V4L2_CAP_VIDEO_OUTPUT_MPLANE) == 0) {
		printf("Device does not have required capabilitiy. line=%d\n",
			__LINE__);
		return -1;
	}

	/* dst */
	ret = ioctl(dst_fd, VIDIOC_QUERYCAP, &cap);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}
	caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS
	     ? cap.device_caps : cap.capabilities;

	if ((caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) == 0) {
		printf("Device does not have required capabilitiy. line=%d\n",
			__LINE__);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Make lookup table - VIDIOC_VSP2_LUT_CONFIG                       */
	/*-------------------------------------------------------------------*/
	ret = set_lut(pmedia, plut_table, LUT_DEV, pmedia_name);
	if (ret == -1) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*********************************************************************
	 *  src
	 *********************************************************************/
	/*-------------------------------------------------------------------*/
	/*  VIDIOC_S_FMT                                                     */
	/*-------------------------------------------------------------------*/
	memset(&fmt, 0, sizeof(fmt));
	fmt.type			= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	fmt.fmt.pix_mp.width		= SRC_WIDTH;
	fmt.fmt.pix_mp.height		= SRC_HEIGHT;
	fmt.fmt.pix_mp.field		= V4L2_FIELD_ANY;
	fmt.fmt.pix_mp.pixelformat	= V4L2_PIX_FMT_ARGB32;
	fmt.fmt.pix_mp.num_planes	= 1;
	fmt.fmt.pix_mp.plane_fmt[0].bytesperline	= 0;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage		= 0;

	ret = ioctl(src_fd, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_G_FMT                                                     */
	/*-------------------------------------------------------------------*/
	memset(&gfmt, 0x00, sizeof(gfmt));
	gfmt.type = fmt.type;
	ret = ioctl(src_fd, VIDIOC_G_FMT, &gfmt);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	if ((fmt.fmt.pix_mp.width        != gfmt.fmt.pix_mp.width)       ||
	    (fmt.fmt.pix_mp.height       != gfmt.fmt.pix_mp.height)      ||
	    (fmt.fmt.pix_mp.field        != gfmt.fmt.pix_mp.field)       ||
	    (fmt.fmt.pix_mp.pixelformat  != gfmt.fmt.pix_mp.pixelformat) ||
	    (fmt.fmt.pix_mp.num_planes   != gfmt.fmt.pix_mp.num_planes)  ||
	    (fmt.fmt.pix_mp.flags        != gfmt.fmt.pix_mp.flags)) {
		printf("Get format error. line=%d\n", __LINE__);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_REQBUFS (alloc)                                           */
	/*-------------------------------------------------------------------*/
	memset(&req_buf, 0, sizeof(req_buf));
	req_buf.count	= 1;		/* Request 1 buffers */
	req_buf.type	= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	req_buf.memory	= V4L2_MEMORY_DMABUF;

	ret = ioctl(src_fd, VIDIOC_REQBUFS, &req_buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Allocate memory by mmngr                                         */
	/*-------------------------------------------------------------------*/
	ret = mmngr_alloc_in_user(
		&srcfd, SRC_SIZE,
		&src_phys, &src_hard, &src_virt, MMNGR_VA_SUPPORT);
	if (ret) {
		printf("error line=%d errcode=(%d)\n", __LINE__, ret);
		return -1;
	}
	psrc_buf = (void *)src_virt;

	/*-------------------------------------------------------------------*/
	/*  Get dma buffer file descriptor by mmngr                          */
	/*-------------------------------------------------------------------*/
	ret = mmngr_export_start_in_user(&src_mbid, SRC_SIZE, src_hard,
		&src_dmafd);
	if (ret) {
		printf("error line=%d errcode=(%d)\n", __LINE__, ret);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Read file                                                        */
	/*-------------------------------------------------------------------*/
	ret = read_file(psrc_buf, SRC_SIZE, SRC_FILENAME);
	if (ret == 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_QBUF                                                      */
	/*-------------------------------------------------------------------*/
	memset(&buf, 0, sizeof(buf));
	memset(planes, 0, sizeof(planes));

	buf.m.planes	= planes;
	buf.index	= 0;
	buf.type	= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory	= V4L2_MEMORY_DMABUF;
	buf.flags	= 0;
	buf.length	= 1;	/* Number of elements in the planes array. */
	buf.m.planes[0].m.fd		= src_dmafd;
	buf.m.planes[0].bytesused	= SRC_SIZE;
	buf.m.planes[0].length		= SRC_SIZE;
	buf.bytesused			= SRC_SIZE;

	ret = ioctl(src_fd, VIDIOC_QBUF, &buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_STREAMON                                                  */
	/*-------------------------------------------------------------------*/
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	ret = ioctl(src_fd, VIDIOC_STREAMON, &type);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*********************************************************************
	 *  dst
	 *********************************************************************/
	/*-------------------------------------------------------------------*/
	/*  VIDIOC_S_FMT                                                     */
	/*-------------------------------------------------------------------*/
	memset(&fmt, 0, sizeof(fmt));
	fmt.type			= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width		= DST_WIDTH;
	fmt.fmt.pix_mp.height		= DST_HEIGHT;
	fmt.fmt.pix_mp.pixelformat	= V4L2_PIX_FMT_ARGB32;
	fmt.fmt.pix_mp.field		= V4L2_FIELD_ANY;
	fmt.fmt.pix_mp.num_planes	= 1;		/* argb32 */
	fmt.fmt.pix_mp.plane_fmt[0].bytesperline	= 0;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage		= 0;

	ret = ioctl(dst_fd, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_G_FMT                                                     */
	/*-------------------------------------------------------------------*/
	memset(&gfmt, 0x00, sizeof(gfmt));
	gfmt.type = fmt.type;
	ret = ioctl(dst_fd, VIDIOC_G_FMT, &gfmt);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	if ((fmt.fmt.pix_mp.width        != gfmt.fmt.pix_mp.width)       ||
	    (fmt.fmt.pix_mp.height       != gfmt.fmt.pix_mp.height)      ||
	    (fmt.fmt.pix_mp.field        != gfmt.fmt.pix_mp.field)       ||
	    (fmt.fmt.pix_mp.pixelformat  != gfmt.fmt.pix_mp.pixelformat) ||
	    (fmt.fmt.pix_mp.num_planes   != gfmt.fmt.pix_mp.num_planes)  ||
	    (fmt.fmt.pix_mp.flags        != gfmt.fmt.pix_mp.flags)) {
		printf("Get format error. line=%d\n", __LINE__);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_REQBUFS (alloc)                                           */
	/*-------------------------------------------------------------------*/
	memset(&req_buf, 0, sizeof(req_buf));
	req_buf.count	= 1;		/* Request 1 buffers */
	req_buf.type	= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req_buf.memory	= V4L2_MEMORY_DMABUF;

	ret = ioctl(dst_fd, VIDIOC_REQBUFS, &req_buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Allocate memory by mmngr                                         */
	/*-------------------------------------------------------------------*/
	ret = mmngr_alloc_in_user(
		&dstfd, DST_SIZE,
		&dst_phys, &dst_hard, &dst_virt, MMNGR_VA_SUPPORT);
	if (ret) {
		printf("error line=%d errcode=(%d)\n", __LINE__, ret);
		return -1;
	}
	pdst_buf = (void *)dst_virt;

	/*-------------------------------------------------------------------*/
	/*  Get dma buffer file descriptor by mmngr                          */
	/*-------------------------------------------------------------------*/
	ret = mmngr_export_start_in_user(&dst_mbid, DST_SIZE, dst_hard,
		&dst_dmafd);
	if (ret) {
		printf("error line=%d errcode=(%d)\n", __LINE__, ret);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_QBUF                                                      */
	/*-------------------------------------------------------------------*/
	memset(&buf, 0, sizeof(buf));
	memset(planes, 0, sizeof(planes));

	buf.m.planes	= planes;
	buf.index	= 0;
	buf.type	= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory	= V4L2_MEMORY_DMABUF;
	buf.length	= 1;	/* Number of elements in the planes array. */
	buf.m.planes[0].m.fd		= dst_dmafd;
	buf.m.planes[0].bytesused	= DST_SIZE;
	buf.m.planes[0].length		= DST_SIZE;
	buf.bytesused				= DST_SIZE;

	ret = ioctl(dst_fd, VIDIOC_QBUF, &buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_STREAMON                                                  */
	/*-------------------------------------------------------------------*/
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(dst_fd, VIDIOC_STREAMON, &type);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_DQBUF                                                     */
	/*-------------------------------------------------------------------*/
	memset(&buf, 0, sizeof(buf));
	memset(planes, 0, sizeof(planes));
	buf.m.planes	= planes;
	buf.type	= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory	= V4L2_MEMORY_DMABUF;
	buf.length	= VIDEO_MAX_PLANES;

	ret = ioctl(dst_fd, VIDIOC_DQBUF, &buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Write file                                                       */
	/*-------------------------------------------------------------------*/
	ret = write_file(pdst_buf, DST_SIZE, DST_FILENAME_DMABUF);
	if (ret == 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*********************************************************************
	 *  src
	 *********************************************************************/
	/*-------------------------------------------------------------------*/
	/*  VIDIOC_DQBUF                                                     */
	/*-------------------------------------------------------------------*/
	memset(&buf, 0, sizeof(buf));
	memset(planes, 0, sizeof(planes));
	buf.m.planes	= planes;
	buf.type	= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory	= V4L2_MEMORY_DMABUF;
	buf.length	= 1;

	ret = ioctl(src_fd, VIDIOC_DQBUF, &buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_STREAMOFF                                                 */
	/*-------------------------------------------------------------------*/
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ret = ioctl(src_fd, VIDIOC_STREAMOFF, &type);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Release dma buffer file descriptor by mmngr                      */
	/*-------------------------------------------------------------------*/
	ret = mmngr_export_end_in_user(src_mbid);
	if (ret < 0) {
		printf("error line=%d errcode=(%d)\n", __LINE__, ret);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_REQBUFS (release)                                         */
	/*-------------------------------------------------------------------*/
	memset(&req_buf, 0, sizeof(req_buf));
	req_buf.count	= 0;		/* Release buffers */
	req_buf.type	= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	req_buf.memory	= V4L2_MEMORY_DMABUF;

	ret = ioctl(src_fd, VIDIOC_REQBUFS, &req_buf);
	if (ret < 0) {
		printf("error %d\n", __LINE__);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Free buffer                                                      */
	/*-------------------------------------------------------------------*/
	ret = mmngr_free_in_user(srcfd);
	if (ret < 0) {
		printf("error line=%d errcode=(%d)\n", __LINE__, ret);
		return -1;
	}

	/*********************************************************************
	 *  dst
	 *********************************************************************/
	/*-------------------------------------------------------------------*/
	/*  VIDIOC_STREAMOFF                                                 */
	/*-------------------------------------------------------------------*/
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(dst_fd, VIDIOC_STREAMOFF, &type);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Release dma buffer file descriptor by mmngr                      */
	/*-------------------------------------------------------------------*/
	ret = mmngr_export_end_in_user(dst_mbid);
	if (ret < 0) {
		printf("error line=%d errcode=(%d)\n", __LINE__, ret);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  VIDIOC_REQBUFS (release)                                         */
	/*-------------------------------------------------------------------*/
	memset(&req_buf, 0, sizeof(req_buf));
	req_buf.count	= 0;		/* Release buffers */
	req_buf.type	= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req_buf.memory	= V4L2_MEMORY_DMABUF;
	ret = ioctl(dst_fd, VIDIOC_REQBUFS, &req_buf);
	if (ret < 0) {
		printf("error line=%d errno=(%d)\n", __LINE__, errno);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Free buffer                                                      */
	/*-------------------------------------------------------------------*/
	ret = mmngr_free_in_user(dstfd);
	if (ret < 0) {
		printf("error line=%d errcode=(%d)\n", __LINE__, ret);
		return -1;
	}

	/*-------------------------------------------------------------------*/
	/*  Release memory for lookup table                                  */
	/*-------------------------------------------------------------------*/
	free(plut_table);

	close(src_fd);
	close(dst_fd);

	media_device_unref(pmedia);

	return 0;
}

/******************************************************************************
 *  internal function
 ******************************************************************************/
static int read_file(
	unsigned char	*pbuffers,
	unsigned int	size,
	const char	*pfilename
	)
{
	FILE	*fp;
	int	ret;

	fp = fopen(pfilename, "rb");
	if (fp == NULL) {
		printf("file open error...\n");
		ret = 0;
	} else {
		ret = fread(pbuffers, size, 1, fp);
		if (ret == 0)
			printf("buffer read error...\n");
		fclose(fp);
	}
	return ret;
}

static int write_file(
	unsigned char	*pbuffers,
	unsigned int	size,
	const char	*pfilename
	)
{
	FILE	*fp;
	int	ret;

	/* file output */
	fp = fopen(pfilename, "wb");
	if (!fp) {
		printf("output file open error..\n");
		ret = 0;
	} else {
		ret = fwrite(pbuffers, size, 1, fp);
		if (ret == 0)
			printf("buffer write error...\n");
		fclose(fp);
	}
	return ret;
}

static int call_media_ctl(struct media_device **ppmedia,
			  const char **ppmedia_name)
{
	struct media_device		*pmedia;
	struct media_link		*plink;
	char				*endp;
	struct dev_pads			rpfpad;
	struct dev_pads			lutpad;
	struct dev_pads			wpfpad;
	struct v4l2_mbus_framefmt	format;

	const struct media_device_info	*pinfo;
	char		*p;
	const char	*pname;
	char		buf[128];

	/* Initialize v4l2 media controller */
	pmedia = media_device_new(MEDIA_DEV_NAME);
	if (!pmedia) {
		printf("Error : media_device_new()\n");
		return -1;
	}

	*ppmedia = pmedia;

	if (media_device_enumerate(pmedia) != 0) {
		printf("Error : media_device_enumerate()\n");
		return -1;
	}

	if (media_reset_links(pmedia) != 0) {
		printf("Error : media_reset_links()\n");
		return -1;
	}

	/* get media device name */
	pinfo = media_get_info(pmedia);
	p = strchr(pinfo->bus_info, ':');
	if (p)
		pname = p + 1;
	else
		pname = pinfo->bus_info;

	*ppmedia_name = pname;

	/*----------------------*/
	/* rpf.0:1 -> lut:0     */
	/*----------------------*/
	sprintf(buf, "'%s rpf.0':1 -> '%s lut':0", pname, pname);
	plink = media_parse_link(pmedia, buf, &endp);
	if (plink == NULL) {
		printf("Error : media_parse_link(rpf -> lut)\n");
		return -1;
	}
	/*----------------------*/
	if (media_setup_link(pmedia, plink->source, plink->sink, 1) != 0) {
		printf("Error : media_setup_link(rpf -> lut)\n");
		return -1;
	}
	/*----------------------*/
	/* lut:1 -> wpf.0:0     */
	/*----------------------*/
	sprintf(buf, "'%s lut':1 -> '%s wpf.0':0", pname, pname);
	plink = media_parse_link(pmedia, buf, &endp);
	if (plink == NULL) {
		printf("Error : media_parse_link(lut -> wpf)\n");
		return -1;
	}
	/*----------------------*/
	if (media_setup_link(pmedia, plink->source, plink->sink, 1) != 0) {
		printf("Error : media_setup_link(lut -> wpf)\n");
		return -1;
	}
	/*----------------------*/
	/* wpf.0:1 -> output    */
	/*----------------------*/
	sprintf(buf, "'%s wpf.0':1 -> '%s wpf.0 output':0", pname, pname);
	plink = media_parse_link(pmedia, buf, &endp);
	if (plink == NULL) {
		printf("Error : media_parse_link(wpf -> output)\n");
		return -1;
	}
	/*----------------------*/
	if (media_setup_link(pmedia, plink->source, plink->sink, 1) != 0) {
		printf("Error : media_setup_link(wpf -> output)\n");
		return -1;
	}

	/*-------------------------------------------------- get pads*/
	/*----------------------*/
	/* rpf pad              */
	/*----------------------*/
	sprintf(buf, "'%s rpf.0':0", pname);
	rpfpad.ppad0 = media_parse_pad(pmedia, buf, NULL);
	if (rpfpad.ppad0 == NULL) {
		printf("Error : media_parse_pad(rpf pad 0)\n");
		return -1;
	}
	/*----------------------*/
	sprintf(buf, "'%s rpf.0':1", pname);
	rpfpad.ppad1 = media_parse_pad(pmedia, buf, NULL);
	if (rpfpad.ppad1 == NULL) {
		printf("Error : media_parse_pad(rpf pad 1)\n");
		return -1;
	}
	/*----------------------*/
	/* lut pad              */
	/*----------------------*/
	sprintf(buf, "'%s lut':0", pname);
	lutpad.ppad0 = media_parse_pad(pmedia, buf, NULL);
	if (lutpad.ppad0 == NULL) {
		printf("Error : media_parse_pad(rpf pad 0)\n");
		return -1;
	}
	/*----------------------*/
	sprintf(buf, "'%s lut':1", pname);
	lutpad.ppad1 = media_parse_pad(pmedia, buf, NULL);
	if (lutpad.ppad1 == NULL) {
		printf("Error : media_parse_pad(rpf pad 1)\n");
		return -1;
	}
	/*----------------------*/
	/* wpf pad              */
	/*----------------------*/
	sprintf(buf, "'%s wpf.0':0", pname);
	wpfpad.ppad0 = media_parse_pad(pmedia, buf, NULL);
	if (wpfpad.ppad0 == NULL) {
		printf("Error : media_parse_pad(wpf pad 0)\n");
		return -1;
	}
	/*----------------------*/
	sprintf(buf, "'%s wpf.0':1", pname);
	wpfpad.ppad1 = media_parse_pad(pmedia, buf, NULL);
	if (wpfpad.ppad1 == NULL) {
		printf("Error : media_parse_pad(wpf pad 0)\n");
		return -1;
	}

	/*----------------------------------------------------- set format */
	/*----------------------*/
	/* rpf.0:0              */
	/*----------------------*/
	format.width	= SRC_WIDTH;
	format.height	= SRC_HEIGHT;
	format.code	= V4L2_MBUS_FMT_ARGB8888_1X32;
	if (v4l2_subdev_set_format(rpfpad.ppad0->entity, &format,
		rpfpad.ppad0->index, V4L2_SUBDEV_FORMAT_ACTIVE) != 0) {
		printf("Error : v4l2_subdev_set_format(rpf pad 0)\n");
		return -1;
	}
	/*----------------------*/
	/* rpf.0:1              */
	/*----------------------*/
	if (v4l2_subdev_set_format(rpfpad.ppad1->entity, &format,
		rpfpad.ppad1->index, V4L2_SUBDEV_FORMAT_ACTIVE) != 0) {
		printf("Error : v4l2_subdev_set_format(rpf pad 1)\n");
		return -1;
	}
	/*----------------------*/
	/* lut:0                */
	/*----------------------*/
	if (v4l2_subdev_set_format(lutpad.ppad0->entity, &format,
		lutpad.ppad0->index, V4L2_SUBDEV_FORMAT_ACTIVE) != 0) {
		printf("Error : v4l2_subdev_set_format(lut pad 0)\n");
		return -1;
	}
	/*----------------------*/
	/* lut:1                */
	/*----------------------*/
	if (v4l2_subdev_set_format(lutpad.ppad1->entity, &format,
		lutpad.ppad1->index, V4L2_SUBDEV_FORMAT_ACTIVE) != 0) {
		printf("Error : v4l2_subdev_set_format(lut pad 1)\n");
		return -1;
	}
	/*----------------------*/
	/* wpf.0:0              */
	/*----------------------*/
	if (v4l2_subdev_set_format(wpfpad.ppad0->entity, &format,
		wpfpad.ppad0->index, V4L2_SUBDEV_FORMAT_ACTIVE) != 0) {
		printf("Error : v4l2_subdev_set_format(wpf pad 0)\n");
		return -1;
	}
	/*----------------------*/
	/* wpf.0:1              */
	/*----------------------*/
	if (v4l2_subdev_set_format(wpfpad.ppad1->entity, &format,
		wpfpad.ppad1->index, V4L2_SUBDEV_FORMAT_ACTIVE) != 0) {
		printf("Error : v4l2_subdev_set_format(wpf pad 1)\n");
		return -1;
	}
	return 0;
}

static int set_lut(struct media_device *pmedia, void *plut_table,
		   char *pentity_base, const char *pmedia_name)
{
	struct vsp2_lut_config	lut_par;
	char			entity_name[32];
	const char		*psubdevname;
	struct media_entity	*pentity;
	unsigned int		*pdata;
	unsigned int		lut_addr;
	int			lut_fd = -1;
	unsigned char		r, g, b;
	unsigned char		sub;

	int	i   = 0;
	int	ret = -1;

	memset(&lut_par, 0, sizeof(lut_par));

	pdata = (unsigned int *)plut_table;

	/* Set config */
	snprintf(entity_name, sizeof(entity_name), pentity_base, pmedia_name);
	pentity = media_get_entity_by_name(pmedia, entity_name,
		                           strlen(entity_name));
	if (pentity == NULL) {
		printf("Error media_get_entity_by_name(%s)\n", entity_name);
		return -1;
	}
	psubdevname = media_entity_get_devname(pentity);

	if (psubdevname == NULL) {
		printf("Error media_entity_get_devname(%s)\n", entity_name);
		return -1;
	}
	lut_fd = open(psubdevname, O_RDWR);

	if (lut_fd != -1) {
		/* Set lut table */
		lut_addr = 0x00007000;

		/* Negative */
		r = 0xff;
		g = 0xff;
		b = 0xff;
		sub = 0x01;

		for (i = 0; i < 256; i++) {
			*pdata = lut_addr;
			pdata++;
			*pdata = r << 16 | g << 8 | b;
			pdata++;

			r -= sub;
			g -= sub;
			b -= sub;
			lut_addr += 4;
		}

		/* Create config param */
		lut_par.addr	= plut_table;
		lut_par.tbl_num	= 256;
		lut_par.fxa	= 0x80;

		if (ioctl(lut_fd, VIDIOC_VSP2_LUT_CONFIG, &lut_par) == 0)
			ret = 0; /* success !! */
		close(lut_fd);
	}

	return ret;
}

static int open_video_device(struct media_device *pmedia, char *pentity_base,
			     const char *pmedia_name)
{
	char entity_name[32];
	const char *pdevname;
	struct media_entity *pentity;

	snprintf(entity_name, sizeof(entity_name), pentity_base, pmedia_name);
	pentity = media_get_entity_by_name(pmedia, entity_name,
					   strlen(entity_name));
	if (!pentity) {
		printf("Error media_get_entity(%s)\n", entity_name);
		return -1;
	}
	pdevname = media_entity_get_devname(pentity);

	return open(pdevname, O_RDWR);
}
