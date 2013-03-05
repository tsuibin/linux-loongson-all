/*
 *  linux/drivers/video/ls2h_fb.c -- Virtual frame buffer device
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>

#include <asm/addrspace.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <ls2h/ls2h.h>
#include "edid.h"

#define DDC_ADDR 0x50
#define DEFAULT_BITS_PER_PIXEL 16 

#ifdef CONFIG_LS2H_SOUTHBRIDGE
#define DEFAULT_CURSOR_MEM 0x90000e0003000000
#define DEFAULT_CURSOR_DMA 0x03000000
#define DEFAULT_FB_MEM 0x90000e0002000000
#define DEFAULT_PHY_ADDR 0x00000e0002000000
#define DEFAULT_FB_DMA 0x02000000
#endif
 
#ifdef CONFIG_CPU_LOONGSON2H
#define DEFAULT_CURSOR_MEM 0x900000000ff00000
#define DEFAULT_CURSOR_DMA 0x0ff00000
#define DEFAULT_FB_MEM 0x900000000f000000
#define DEFAULT_PHY_ADDR 0x0f000000
#define DEFAULT_FB_DMA 0x0f000000
#endif
struct ls2h_fb_par {
	struct platform_device *pdev;
	struct fb_info *fb_info;
	int reg_base;
	int irq;
	u8 *edid;
};
struct vga_struct {
	int pclk;
	int hr, hss, hse, hfl;
	int vr, vss, vse, vfl;
} vgamode[] = {
	{	504430613,	640,	664,	728,	816,	480,	481,	484,	500,	},	/*"640x480_70.00" */	
	{	487784469,	640,	672,	736,	832,	640,	641,	644,	663,	},	/*"640x640_60.00" */	
	{	488046613,	640,	672,	736,	832,	768,	769,	772,	795,	},	/*"640x768_60.00" */	
	{	319815701,	640,	680,	744,	848,	800,	801,	804,	828,	},	/*"640x800_60.00" */	
	{	353304597,	800,	832,	912,	1024,	480,	481,	484,	500,	},	/*"800x480_70.00" */	
	{	353370133,	800,	832,	912,	1024,	600,	601,	604,	622,	},	/*"800x600_60.00" */	
	{	269287445,	800,	832,	912,	1024,	640,	641,	644,	663,	},	/*"800x640_60.00" */	
	{	252444693,	832,	864,	952,	1072,	600,	601,	604,	622,	},	/*"832x600_60.00" */	
	{	353435669,	832,	864,	952,	1072,	608,	609,	612,	630,	},	/*"832x608_60.00" */	
	{	353370133,	1024,	1048,	1152,	1280,	480,	481,	484,	497,	},	/*"1024x480_60.00" */	
	{	522059797,	1024,	1064,	1168,	1312,	600,	601,	604,	622,	},	/*"1024x600_60.00" */	
	{	303235093,	1024,	1072,	1176,	1328,	640,	641,	644,	663,	},	/*"1024x640_60.00" */	
	{	421527573,	1024,	1080,	1184,	1344,	768,	769,	772,	795,	},	/*"1024x768_60.00" */	
	{	320536597,	1152,	1208,	1328,	1504,	764,	765,	768,	791,	},	/*"1152x764_60.00" */	
	{	151978005,	1280,	1344,	1480,	1680,	800,	801,	804,	828,	},	/*"1280x800_60.00" */	
	{	219938837,	1280,	1328,	1440,	1688,	1024,	1025,	1028,	1066,	},	/*"1280x1024_60.00" */
	{	168886293,	1366,	1436,	1579,	1792,	768,	771,	774,	798,	},	/*"1366x768_60.00" */	
	{	135331861,	1440,	1520,	1672,	1904,	900,	903,	909,	934,	},	/*"1440x900_60.00" */	
};

static char *mode_option = NULL; 
    /*
     *  RAM we reserve for the frame buffer. This defines the maximum screen
     *  size
     *
     *  The default can be overridden if the driver is compiled as a module
     */

static void *cursor_mem;
static unsigned int cursor_size = 0x1000;
static void *videomemory;
static	dma_addr_t dma_A;
static dma_addr_t cursor_dma;

static u_long videomemorysize = 0;
module_param(videomemorysize, ulong, 0);
DEFINE_SPINLOCK(fb_lock);

static struct fb_var_screeninfo ls2h_fb_default __initdata = {
	.xres =		1280,
	.yres =		1024,
	.xres_virtual =	1280,
	.yres_virtual =	1024,
	.bits_per_pixel = DEFAULT_BITS_PER_PIXEL,
	.red =		{ 11, 5 ,0},
	.green =	{ 5, 6, 0 },
	.blue =		{ 0, 5, 0 },
	.activate =	FB_ACTIVATE_NOW,
	.height =	-1,
	.width =	-1,
	.pixclock =	20000,
	.left_margin =	64,
	.right_margin =	64,
	.upper_margin =	32,
	.lower_margin =	32,
	.hsync_len =	64,
	.vsync_len =	2,
	.vmode =	FB_VMODE_NONINTERLACED,
};
static struct fb_fix_screeninfo ls2h_fb_fix __initdata = {
	.id =		"Virtual FB",
	.type =		FB_TYPE_PACKED_PIXELS,
	.visual =	FB_VISUAL_TRUECOLOR,
	.xpanstep =	1,
	.ypanstep =	1,
	.ywrapstep =	1,
	.accel =	FB_ACCEL_NONE,
};

static int ls2h_fb_enable __initdata = 0;	/* disabled by default */
module_param(ls2h_fb_enable, bool, 0);

static int ls2h_fb_check_var(struct fb_var_screeninfo *var,
			 struct fb_info *info);
static int ls2h_fb_set_par(struct fb_info *info);
static int ls2h_fb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			 u_int transp, struct fb_info *info);
static int ls2h_fb_pan_display(struct fb_var_screeninfo *var,
				struct fb_info *info);

static struct fb_ops ls2h_fb_ops = {
	.fb_check_var	= ls2h_fb_check_var,
	.fb_set_par	= ls2h_fb_set_par,
	.fb_setcolreg	= ls2h_fb_setcolreg,
	.fb_pan_display	= ls2h_fb_pan_display,
	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
};

/*
 *  Internal routines
 */

static u_long get_line_length(int xres_virtual, int bpp)
{
	u_long length;

	length = xres_virtual * bpp;
	length = (length + 31) & ~31;
	length >>= 3;
	return (length);
}

/*
 *  Setting the video mode has been split into two parts.
 *  First part, xxxfb_check_var, must not write anything
 *  to hardware, it should only verify and adjust var.
 *  This means it doesn't alter par but it does use hardware
 *  data from it to check this var.
 */

static int ls2h_fb_check_var(struct fb_var_screeninfo *var,
			 struct fb_info *info)
{
	u_long line_length;

	/*
	 *  FB_VMODE_CONUPDATE and FB_VMODE_SMOOTH_XPAN are equal!
	 *  as FB_VMODE_SMOOTH_XPAN is only used internally
	 */

	if (var->vmode & FB_VMODE_CONUPDATE) {
		var->vmode |= FB_VMODE_YWRAP;
		var->xoffset = info->var.xoffset;
		var->yoffset = info->var.yoffset;
	}

	/*
	 *  Some very basic checks
	 */
	if (!var->xres)
		var->xres = 1;
	if (!var->yres)
		var->yres = 1;
	if (var->xres > var->xres_virtual)
		var->xres_virtual = var->xres;
	if (var->yres > var->yres_virtual)
		var->yres_virtual = var->yres;
	if (var->bits_per_pixel <= 1)
		var->bits_per_pixel = 1;
	else if (var->bits_per_pixel <= 8)
		var->bits_per_pixel = 8;
	else if (var->bits_per_pixel <= 16)
		var->bits_per_pixel = 16;
	else if (var->bits_per_pixel <= 24)
		var->bits_per_pixel = 24;
	else if (var->bits_per_pixel <= 32)
		var->bits_per_pixel = 32;
	else
		return -EINVAL;

	if (var->xres_virtual < var->xoffset + var->xres)
		var->xres_virtual = var->xoffset + var->xres;
	if (var->yres_virtual < var->yoffset + var->yres)
		var->yres_virtual = var->yoffset + var->yres;

	/*
	 *  Memory limit
	 */
	line_length =
		get_line_length(var->xres_virtual, var->bits_per_pixel);
	if (videomemorysize &&  line_length * var->yres_virtual > videomemorysize)
		return -ENOMEM;

	/*
	 * Now that we checked it we alter var. The reason being is that the video
	 * mode passed in might not work but slight changes to it might make it 
	 * work. This way we let the user know what is acceptable.
	 */
	switch (var->bits_per_pixel) {
	case 1:
	case 8:
		var->red.offset = 0;
		var->red.length = 8;
		var->green.offset = 0;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;
		var->transp.offset = 0;
		var->transp.length = 0;
		break;
	case 15:		/* RGBA 555 */
		var->red.offset = 10;
		var->red.length = 5;
		var->green.offset = 5;
		var->green.length = 5;
		var->blue.offset = 0;
		var->blue.length = 5;
		break;
	case 16:		/* BGR 565 */
		var->red.offset = 11;
		var->red.length = 5;
		var->green.offset = 5;
		var->green.length = 6;
		var->blue.offset = 0;
		var->blue.length = 5;
		var->transp.offset = 0;
		var->transp.length = 0;
		break;
	case 24:		/* RGB 888 */
		var->red.offset = 16;
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;
		var->transp.offset = 0;
		var->transp.length = 0;
		break;
	case 32:		/* RGBA 8888 */
		var->red.offset = 16;
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;
		var->transp.offset = 24;
		var->transp.length = 8;
		break;
	}
	var->red.msb_right = 0;
	var->green.msb_right = 0;
	var->blue.msb_right = 0;
	var->transp.msb_right = 0;

	return 0;
}

static int ls2h_init_regs(struct fb_var_screeninfo *info)
{
	
	int i, j, mode = -1;
	unsigned int chip_reg;
	unsigned int out, depth, fb_xsize, fb_ysize;

	fb_xsize = info->xres;
	fb_ysize = info->yres;
	depth = info->bits_per_pixel;

	for (i = 0; i < sizeof(vgamode) / sizeof(struct vga_struct); i++) {
		if (vgamode[i].hr == fb_xsize && vgamode[i].vr == fb_ysize) {
			mode = i;
			out = vgamode[i].pclk;
			/* change to refclk */
			ls2h_writew(0, LS2H_PIXCLK0_CTRL1_REG);
			ls2h_writew(0, LS2H_PIXCLK1_CTRL1_REG);
			/* pll_powerdown set pstdiv */
			ls2h_writew((out | 0x80000080), LS2H_PIXCLK0_CTRL0_REG);
			ls2h_writew((out | 0x80000080), LS2H_PIXCLK1_CTRL0_REG);
			/* wait 10000ns */
			for (j = 1; j <= 300; j++)
				chip_reg = ls2h_readw(LS2H_CHIP_SAMP0_REG);
			/* pll_powerup unset pstdiv */
			ls2h_writew(out, LS2H_PIXCLK0_CTRL0_REG);
			ls2h_writew(out, LS2H_PIXCLK1_CTRL0_REG);
			/* wait pll_lock */
			while ((ls2h_readw(LS2H_CHIP_SAMP0_REG) & 0x00001800) != 0x00001800) {
			}
			/* change to pllclk */
			ls2h_writew(0x1, LS2H_PIXCLK0_CTRL1_REG);
			ls2h_writew(0x1, LS2H_PIXCLK1_CTRL1_REG);
			break;
		}
	}

	/*Make DVO port putout the panel1 the same with VGA */
	ls2h_writew(0x0, LS2H_FB_CFG_DVO_REG);
	ls2h_writew(0x100200, LS2H_FB_CFG_DVO_REG);

/* VGA */
	ls2h_writew(0, LS2H_FB_CFG_VGA_REG);
	ls2h_writew(0x3, LS2H_FB_CFG_VGA_REG);
	ls2h_writew(dma_A, LS2H_FB_ADDR0_VGA_REG);
	ls2h_writew(dma_A, LS2H_FB_ADDR1_VGA_REG);
	ls2h_writew(0, LS2H_FB_DITCFG_VGA_REG);
	ls2h_writew(0, LS2H_FB_DITTAB_LO_VGA_REG);
	ls2h_writew(0, LS2H_FB_DITTAB_HI_VGA_REG);
	ls2h_writew(0x80001311, LS2H_FB_PANCFG_VGA_REG);
	ls2h_writew(0x80001311, LS2H_FB_PANTIM_VGA_REG);

	ls2h_writew((vgamode[mode].hfl << 16) | vgamode[mode].hr,
			LS2H_FB_HDISPLAY_VGA_REG);
	ls2h_writew(0x40000000 | (vgamode[mode].hse << 16) | vgamode[mode].hss,
			LS2H_FB_HSYNC_VGA_REG);
	ls2h_writew((vgamode[mode].vfl << 16) | vgamode[mode].vr,
			LS2H_FB_VDISPLAY_VGA_REG);
	ls2h_writew(0x40000000 | (vgamode[mode].vse << 16) | vgamode[mode].vss,
			LS2H_FB_VSYNC_VGA_REG);
	switch (depth) {
	case 32:
	case 24:
		ls2h_writew(0x00100104, LS2H_FB_CFG_VGA_REG);
		ls2h_writew((fb_xsize * 4 + 255) & ~255, LS2H_FB_STRI_VGA_REG);
		break;
	case 16:
		ls2h_writew(0x00100103, LS2H_FB_CFG_VGA_REG);
		ls2h_writew((fb_xsize * 2 + 255) & ~255, LS2H_FB_STRI_VGA_REG);
		break;
	case 15:
		ls2h_writew(0x00100102, LS2H_FB_CFG_VGA_REG);
		ls2h_writew((fb_xsize * 2 + 255) & ~255, LS2H_FB_STRI_VGA_REG);
		break;
	case 12:
		ls2h_writew(0x00100101, LS2H_FB_CFG_VGA_REG);
		ls2h_writew((fb_xsize * 2 + 255) & ~255, LS2H_FB_STRI_VGA_REG);
		break;
	default:
		ls2h_writew(0x00100104, LS2H_FB_CFG_VGA_REG);
		ls2h_writew((fb_xsize * 4 + 255) & ~255, LS2H_FB_STRI_VGA_REG);
		break;
	}

/* cursor*/
	ls2h_writew(0x00020200, LS2H_FB_CUR_CFG_REG);
	ls2h_writew(cursor_dma, LS2H_FB_CUR_ADDR_REG);
	ls2h_writew(0x00060122, LS2H_FB_CUR_LOC_ADDR_REG);
	ls2h_writew(0x00eeeeee, LS2H_FB_CUR_BACK_REG);
	ls2h_writew(0x00aaaaaa, LS2H_FB_CUR_FORE_REG);
/* enable interupt */
	ls2h_writew(0x280 << 16, LS2H_FB_INT_REG);
	return 0;
}

/* This routine actually sets the video mode. It's in here where we
 * the hardware state info->par and fix which can be affected by the 
 * change in par. For this driver it doesn't do much. 
 */
static int ls2h_fb_set_par(struct fb_info *info)
{
	info->fix.line_length = get_line_length(info->var.xres_virtual,
						info->var.bits_per_pixel);
	return 0;
}

/*
 *  Set a single color register. The values supplied are already
 *  rounded down to the hardware's capabilities (according to the
 *  entries in the var structure). Return != 0 for invalid regno.
 */

static int ls2h_fb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			 u_int transp, struct fb_info *info)
{
	if (regno >= 256)	/* no. of hw registers */
		return 1;
	/*
	 * Program hardware... do anything you want with transp
	 */

	/* grayscale works only partially under directcolor */
	if (info->var.grayscale) {
		/* grayscale = 0.30*R + 0.59*G + 0.11*B */
		red = green = blue =
			(red * 77 + green * 151 + blue * 28) >> 8;
	}

	/* Directcolor:
	 *   var->{color}.offset contains start of bitfield
	 *   var->{color}.length contains length of bitfield
	 *   {hardwarespecific} contains width of RAMDAC
	 *   cmap[X] is programmed to (X << red.offset) | (X << green.offset) | (X << blue.offset)
	 *   RAMDAC[X] is programmed to (red, green, blue)
	 * 
	 * Pseudocolor:
	 *    uses offset = 0 && length = RAMDAC register width.
	 *    var->{color}.offset is 0
	 *    var->{color}.length contains widht of DAC
	 *    cmap is not used
	 *    RAMDAC[X] is programmed to (red, green, blue)
	 * Truecolor:
	 *    does not use DAC. Usually 3 are present.
	 *    var->{color}.offset contains start of bitfield
	 *    var->{color}.length contains length of bitfield
	 *    cmap is programmed to (red << red.offset) | (green << green.offset) |
	 *                      (blue << blue.offset) | (transp << transp.offset)
	 *    RAMDAC does not exist
	 */
#define CNVT_TOHW(val,width) ((((val)<<(width))+0x7FFF-(val))>>16)
	switch (info->fix.visual) {
	case FB_VISUAL_TRUECOLOR:
	case FB_VISUAL_PSEUDOCOLOR:
		red = CNVT_TOHW(red, info->var.red.length);
		green = CNVT_TOHW(green, info->var.green.length);
		blue = CNVT_TOHW(blue, info->var.blue.length);
		transp = CNVT_TOHW(transp, info->var.transp.length);
		break;
	case FB_VISUAL_DIRECTCOLOR:
		red = CNVT_TOHW(red, 8);	/* expect 8 bit DAC */
		green = CNVT_TOHW(green, 8);
		blue = CNVT_TOHW(blue, 8);
		/* hey, there is bug in transp handling... */
		transp = CNVT_TOHW(transp, 8);
		break;
	}
#undef CNVT_TOHW
	/* Truecolor has hardware independent palette */
	if (info->fix.visual == FB_VISUAL_TRUECOLOR) {
		u32 v;

		if (regno >= 16)
			return 1;

		v = (red << info->var.red.offset) |
			(green << info->var.green.offset) |
			(blue << info->var.blue.offset) |
			(transp << info->var.transp.offset);
		switch (info->var.bits_per_pixel) {
		case 8:
			break;
		case 16:
			((u32 *) (info->pseudo_palette))[regno] = v;
			break;
		case 24:
		case 32:
			((u32 *) (info->pseudo_palette))[regno] = v;
			break;
		}
		return 0;
	}
	return 0;
}

/*
 *  Pan or Wrap the Display
 *
 *  This call looks only at xoffset, yoffset and the FB_VMODE_YWRAP flag
 */

static int ls2h_fb_pan_display(struct fb_var_screeninfo *var,
			struct fb_info *info)
{
	if (var->vmode & FB_VMODE_YWRAP) {
		if (var->yoffset < 0
			|| var->yoffset >= info->var.yres_virtual
			|| var->xoffset)
			return -EINVAL;
	} else {
		if (var->xoffset + var->xres > info->var.xres_virtual ||
			var->yoffset + var->yres > info->var.yres_virtual)
			return -EINVAL;
	}
	info->var.xoffset = var->xoffset;
	info->var.yoffset = var->yoffset;
	if (var->vmode & FB_VMODE_YWRAP)
		info->var.vmode |= FB_VMODE_YWRAP;
	else
		info->var.vmode &= ~FB_VMODE_YWRAP;
	return 0;
}


#ifndef MODULE
static int __init ls2h_fb_setup(char *options)
{
	char *this_opt;
	ls2h_fb_enable = 1;

	if (!options || !*options)
		return 1;

	while ((this_opt = strsep(&options, ",")) != NULL) {
		if (!*this_opt)
			continue;
		if (!strncmp(this_opt, "disable", 7))
			ls2h_fb_enable = 0;
		else
			mode_option = this_opt;
	}
	return 1;
}
#endif  /*  MODULE  */

static void ls2h_i2c_stop(void)
{
again:
        ls2h_writeb(CR_STOP, LS2H_I2C1_CR_REG);
        ls2h_readb(LS2H_I2C1_SR_REG);
        while (ls2h_readb(LS2H_I2C1_SR_REG) & SR_BUSY)
                goto again;
}

static int ls2h_i2c_start(int dev_addr, int flags)
{
	int retry = 5;
	unsigned char addr = (dev_addr & 0x7f) << 1;
	addr |= (flags & I2C_M_RD)? 1:0;
	
start:
	mdelay(1);
	ls2h_writeb(addr, LS2H_I2C1_TXR_REG);
	ls2h_writeb((CR_START | CR_WRITE), LS2H_I2C1_CR_REG);
	while (ls2h_readb(LS2H_I2C1_SR_REG) & SR_TIP) ;

	if (ls2h_readb(LS2H_I2C1_SR_REG) & SR_NOACK) {
		ls2h_i2c_stop();
		while (retry--) 
			goto start;
		printk("%s:There is no edid device ack\n",__func__);
		return 0;
	}
	return 1;
}

static void ls2h_i2c1_init(void)
{
        ls2h_writeb(0, LS2H_I2C1_CTR_REG);
        ls2h_writeb(0x64, LS2H_I2C1_PRER_LO_REG);
        ls2h_writeb(0, LS2H_I2C1_PRER_HI_REG);
        ls2h_writeb(0x80, LS2H_I2C1_CTR_REG);
}

static int ls2h_i2c_read(unsigned char *buf, int count)
{
        int i;

        for (i = 0; i < count; i++) {
                ls2h_writeb((CR_READ | CR_ACK), LS2H_I2C1_CR_REG);
                while (ls2h_readb(LS2H_I2C1_SR_REG) & SR_TIP) ;
                buf[i] = ls2h_readb(LS2H_I2C1_RXR_REG);
        }

        return i;
}

static int ls2h_i2c_write(unsigned char *buf, int count)
{
        int i;

        for (i = 0; i < count; i++) {
		ls2h_writeb(buf[i], LS2H_I2C1_TXR_REG);
		ls2h_writeb(CR_WRITE, LS2H_I2C1_CR_REG);
		while (ls2h_readb(LS2H_I2C1_SR_REG) & SR_TIP) ;

		if (ls2h_readb(LS2H_I2C1_SR_REG) & SR_NOACK) {
			ls2h_i2c_stop();
			return 0;
		}
        }

        return i;
}

static int ls2h_i2c_doxfer(struct i2c_msg *msgs, int num)
{
	struct i2c_msg *m = msgs;
	int i;

	for(i = 0; i < num; i++) {
		if (!ls2h_i2c_start(m->addr, m->flags))
			return 0;
		if (m->flags & I2C_M_RD)
			ls2h_i2c_read(m->buf, m->len);
		else
			ls2h_i2c_write(m->buf, m->len);
		++m;
	}

	ls2h_i2c_stop();

	return i;
}

static int ls2h_i2c_xfer(struct i2c_adapter *adap,
                        struct i2c_msg *msgs, int num)
{
        int retry;
        int ret;

        for (retry = 0; retry < adap->retries; retry++) {

                ret = ls2h_i2c_doxfer(msgs, num);

                if (ret != -EAGAIN)
                        return ret;

                udelay(100);
        }

        return -EREMOTEIO;
}
static unsigned int ls2h_i2c_func(struct i2c_adapter *adap)
{
        return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static unsigned char *fb_do_probe_ddc_edid(struct i2c_adapter *adapter)
{
	int i;
	unsigned char start = 0x0;
	unsigned char *buf = kzalloc(EDID_LENGTH, GFP_KERNEL);
	unsigned char *info = kzalloc(EDID_LENGTH, GFP_KERNEL);
	struct i2c_msg msgs[] = {
		{
			.addr	= DDC_ADDR,
			.flags	= 0,
			.len	= 1,
			.buf	= &start,
		}, { 
			.addr	= DDC_ADDR,
			.flags	= I2C_M_RD,
			.len	= EDID_LENGTH,
			.buf	= buf,
		}
	};

	if (!buf || !info) {
		printk("unable to allocate memory for EDID "
			 "block.\n");
		return NULL;
	}

	if (!(i2c_transfer(adapter, &msgs[0], 1) == 1))
		goto err;
	for (i = 0; i < 128; i++) {
		if (!(i2c_transfer(adapter, &msgs[1], 1) == 1))
			goto err; 
		info[i] = buf[0];
	}

	kfree(buf);
	return info;

err:
	printk("unable to read EDID block.\n");
	kfree(buf);
	kfree(info);
	return NULL;
}
static unsigned char *ls2h_fb_i2c_connector(struct ls2h_fb_par *fb_par)
{
	int ret;
	struct i2c_adapter *adap;
	struct i2c_algorithm *algo;
	unsigned char *edid = NULL;
	
	printk("edid entry\n");

	adap = kzalloc(sizeof(struct i2c_adapter), GFP_KERNEL);
	if (!adap) {
		printk("adap alloc eroor\n");
		return NULL;
	}

	algo = kzalloc(sizeof(struct i2c_algorithm), GFP_KERNEL);
	if (!algo) {
		printk("algo alloc eroor\n");
		return NULL;
	}

	algo->master_xfer = ls2h_i2c_xfer;
	algo->functionality = ls2h_i2c_func;

	strlcpy(adap->name, "LS2H I2C adapter", sizeof(adap->name));
	adap->owner = THIS_MODULE;
	adap->algo = algo;
	adap->retries = 2;
	adap->class = I2C_CLASS_HWMON | I2C_CLASS_SPD | I2C_CLASS_DDC;
	adap->dev.parent = &fb_par->pdev->dev;

	ret = i2c_add_adapter(adap);
        if (ret < 0) {
		printk("i2c add adapter error\n");
		goto err1;
        }

	ls2h_i2c1_init();
	edid = fb_do_probe_ddc_edid(adap);
	if (!edid)
		goto err0;
	fb_par->edid = edid;

err0:
	i2c_del_adapter(adap);
err1:
	kfree(adap);
	kfree(algo);
	return edid;
}

static void ls2h_find_init_mode(struct fb_info *info)
{
        struct fb_videomode mode;
        struct fb_var_screeninfo var;
        struct fb_monspecs *specs = &info->monspecs;
	struct ls2h_fb_par *par = info->par;
        int found = 0;
	unsigned char *edid;
	unsigned int i, support = 0;

        INIT_LIST_HEAD(&info->modelist);
        memset(&mode, 0, sizeof(struct fb_videomode));
	var.bits_per_pixel = DEFAULT_BITS_PER_PIXEL;

	edid = ls2h_fb_i2c_connector(par);
	if (!edid) 
		goto def;

        fb_edid_to_monspecs(par->edid, specs);

        if (specs->modedb == NULL) {
		printk("ls2h-fb: Unable to get Mode Database\n");
		goto def;
	}

        fb_videomode_to_modelist(specs->modedb, specs->modedb_len,
                                 &info->modelist);
        if (specs->modedb != NULL) {
                const struct fb_videomode *m;
                if (!found) {
                        m = fb_find_best_display(&info->monspecs, &info->modelist);
                        mode = *m;
                        found = 1;
                }

                fb_videomode_to_var(&var, &mode);
        }

        if (mode_option) {
		printk("mode_option: %s\n", mode_option);
                fb_find_mode(&var, info, mode_option, specs->modedb,
                             specs->modedb_len, (found) ? &mode : NULL,
                             info->var.bits_per_pixel);
	}

        fb_destroy_modedb(specs->modedb);
        specs->modedb = NULL;

	for (i = 0; i < sizeof(vgamode) / sizeof(struct vga_struct); i++) 
		if (vgamode[i].hr == var.xres && 
				vgamode[i].vr == var.yres) 
				support = 1;
	if (support) {
		info->var = var;
	} else {
		printk("%s: driver doesn't support %dX%d, use default mode",
				__func__, var.xres, var.yres);
		goto def;
	}

	return;
def:
	info->var = ls2h_fb_default;	
	return;
}
/* irq */

static irqreturn_t ls2hfb_irq(int irq, void *dev_id)
{
	unsigned int val, cfg;
	
	spin_lock(&fb_lock);

	val = ls2h_readw(LS2H_FB_INT_REG);
	ls2h_writew(val & (0xffff << 16), LS2H_FB_INT_REG);

	cfg = ls2h_readw(LS2H_FB_CFG_VGA_REG);
	/* if underflow, reset VGA */
	if (val & 0x280) {
		ls2h_writew(0, LS2H_FB_CFG_VGA_REG);
		ls2h_writew(cfg, LS2H_FB_CFG_VGA_REG);
	}

	spin_unlock(&fb_lock);

	return IRQ_HANDLED;
}
/*
 *  Initialisation
 */

static int ls2h_fb_probe(struct platform_device *dev)
{
	int irq;
	struct fb_info *info;
	int retval = -ENOMEM;
	struct ls2h_fb_par *par;

	irq = platform_get_irq(dev, 0);
	if (irq < 0) {
		dev_err(&dev->dev, "no irq for device\n");
		return -ENOENT;
	}

	info = framebuffer_alloc(sizeof(u32) * 256, &dev->dev);
	if (!info) 
		return -ENOMEM;

	info->fix = ls2h_fb_fix;
	info->node = -1;
	info->fbops = &ls2h_fb_ops;
	info->pseudo_palette = info->par;
	info->flags = FBINFO_FLAG_DEFAULT;

	par = kzalloc(sizeof(struct ls2h_fb_par), GFP_KERNEL);
	if (!par) {
		retval = -ENOMEM;
		goto release_info;
	}

	info->par = par;
	par->fb_info = info;
	par->pdev = dev;
	par->irq = irq;
	ls2h_find_init_mode(info);

	if (!videomemorysize) {
		videomemorysize = info->var.xres_virtual *
					info->var.yres_virtual *
					info->var.bits_per_pixel / 8;
	}

	/*
	 * For real video cards we use ioremap.
	 */
	videomemory = (void *)DEFAULT_FB_MEM; 
	dma_A = (dma_addr_t)DEFAULT_FB_DMA;

	printk(KERN_CRIT "videomemory=%llx\n",(long long unsigned int)videomemory);
	printk(KERN_CRIT "videomemorysize=%lx\n",videomemorysize);
	printk(KERN_CRIT "dma_A=%x\n",(int)dma_A);
	memset(videomemory, 0, videomemorysize);

	cursor_mem = (void *)DEFAULT_CURSOR_MEM;
	cursor_dma = (dma_addr_t)DEFAULT_CURSOR_DMA;
	memset (cursor_mem,0x88FFFF00,cursor_size);

	info->screen_base = (char __iomem *)videomemory;
	info->fix.smem_start = DEFAULT_PHY_ADDR;
	info->fix.smem_len = videomemorysize;

	retval = fb_alloc_cmap(&info->cmap, 32, 0);
	if (retval < 0) goto release_par;

	info->fbops->fb_check_var(&info->var, info);
	ls2h_init_regs(&info->var);
	retval = register_framebuffer(info);
	if (retval < 0)
		goto release_map;

	retval = request_irq(irq, ls2hfb_irq, IRQF_DISABLED, dev->name, info);
	if (retval) {
		dev_err(&dev->dev, "cannot get irq %d - err %d\n", irq, retval);
		goto unreg_info;
	}

	platform_set_drvdata(dev, info);

	printk(KERN_INFO
		"fb%d: Virtual frame buffer device, using %ldK of video memory\n",
		info->node, videomemorysize >> 10);

	return 0;
unreg_info:
	unregister_framebuffer(info);
release_map:
	fb_dealloc_cmap(&info->cmap);
release_par:
	kfree(par);
release_info:
	platform_set_drvdata(dev, NULL);
	framebuffer_release(info);
	return retval;
}

static int ls2h_fb_remove(struct platform_device *dev)
{
	struct fb_info *info = platform_get_drvdata(dev);
	struct ls2h_fb_par *par = info->par;
	int irq = par->irq;

	free_irq(irq, info);
	fb_dealloc_cmap(&info->cmap);
	unregister_framebuffer(info);
	platform_set_drvdata(dev, info);
	framebuffer_release(info);
	if (par->edid) 
		kfree(par->edid);
	kfree(par);

	return 0;
}

static struct platform_driver ls2h_fb_driver = {
	.probe	= ls2h_fb_probe,
	.remove = ls2h_fb_remove,
	.driver = {
		.name	= "ls2h-fb",
	},
};

static int __init ls2h_fb_init(void)
{
	int ret = 0;

#ifndef MODULE
	char *option = NULL;

	if (fb_get_options("ls2h-fb", &option))
		return -ENODEV;
	ls2h_fb_setup(option);
#endif

	if (!ls2h_fb_enable)
		return -ENXIO;

	ret = platform_driver_register(&ls2h_fb_driver);

	return ret;
}

module_init(ls2h_fb_init);

#ifdef MODULE
static void __exit ls2h_fb_exit(void)
{
	platform_driver_unregister(&ls2h_fb_driver);
}

module_exit(ls2h_fb_exit);

MODULE_LICENSE("GPL");
#endif				/* MODULE */
