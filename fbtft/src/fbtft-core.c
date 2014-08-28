/*
 * Copyright (C) 2013 Noralf Tronnes
 *
 * This driver is inspired by:
 *   st7735fb.c, Copyright (C) 2011, Matt Porter
 *   broadsheetfb.c, Copyright (C) 2008, Jaya Kumar
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/backlight.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/dma-mapping.h>

#include "fbtft.h"

extern void fbtft_sysfs_init(struct fbtft_par *par);
extern void fbtft_sysfs_exit(struct fbtft_par *par);
extern void fbtft_expand_debug_value(unsigned long *debug);
extern int fbtft_gamma_parse_str(struct fbtft_par *par, unsigned long *curves,
						const char *str, int size);

static unsigned long debug;
module_param(debug, ulong , 0);
MODULE_PARM_DESC(debug, "override device debug level");

static bool dma = true;
module_param(dma, bool, 0);
MODULE_PARM_DESC(dma, "Use DMA buffer");


void fbtft_dbg_hex(const struct device *dev, int groupsize,
			void *buf, size_t len, const char *fmt, ...)
{
	va_list args;
	static char textbuf[512];
	char *text = textbuf;
	size_t text_len;

	va_start(args, fmt);
	text_len = vscnprintf(text, sizeof(textbuf), fmt, args);
	va_end(args);

	hex_dump_to_buffer(buf, len, 32, groupsize, text + text_len,
				512 - text_len, false);

	if (len > 32)
		dev_info(dev, "%s ...\n", text);
	else
		dev_info(dev, "%s\n", text);
}
EXPORT_SYMBOL(fbtft_dbg_hex);

unsigned long fbtft_request_gpios_match(struct fbtft_par *par,
					const struct fbtft_gpio *gpio)
{
	int ret;
	long val;

	fbtft_par_dbg(DEBUG_REQUEST_GPIOS_MATCH, par, "%s('%s')\n",
		__func__, gpio->name);

	if (strcasecmp(gpio->name, "reset") == 0) {
		par->gpio.reset = gpio->gpio;
		return GPIOF_OUT_INIT_HIGH;
	} else if (strcasecmp(gpio->name, "dc") == 0) {
		par->gpio.dc = gpio->gpio;
		return GPIOF_OUT_INIT_LOW;
	} else if (strcasecmp(gpio->name, "cs") == 0) {
		par->gpio.cs = gpio->gpio;
		return GPIOF_OUT_INIT_HIGH;
	} else if (strcasecmp(gpio->name, "wr") == 0) {
		par->gpio.wr = gpio->gpio;
		return GPIOF_OUT_INIT_HIGH;
	} else if (strcasecmp(gpio->name, "rd") == 0) {
		par->gpio.rd = gpio->gpio;
		return GPIOF_OUT_INIT_HIGH;
	} else if (strcasecmp(gpio->name, "latch") == 0) {
		par->gpio.latch = gpio->gpio;
		return GPIOF_OUT_INIT_LOW;
	} else if (gpio->name[0] == 'd' && gpio->name[1] == 'b') {
		ret = kstrtol(&gpio->name[2], 10, &val);
		if (ret == 0 && val < 16) {
			par->gpio.db[val] = gpio->gpio;
			return GPIOF_OUT_INIT_LOW;
		}
	} else if (strcasecmp(gpio->name, "led") == 0) {
		par->gpio.led[0] = gpio->gpio;
		return GPIOF_OUT_INIT_LOW;
	} else if (strcasecmp(gpio->name, "led_") == 0) {
		par->gpio.led[0] = gpio->gpio;
		return GPIOF_OUT_INIT_HIGH;
	}

	return FBTFT_GPIO_NO_MATCH;
}

int fbtft_request_gpios(struct fbtft_par *par)
{
	struct fbtft_platform_data *pdata = par->pdata;
	const struct fbtft_gpio *gpio;
	unsigned long flags;
	int i;
	int ret;

	/* Initialize gpios to disabled */
	par->gpio.reset = -1;
	par->gpio.dc = -1;
	par->gpio.rd = -1;
	par->gpio.wr = -1;
	par->gpio.cs = -1;
	par->gpio.latch = -1;
	for (i = 0; i < 16; i++) {
		par->gpio.db[i] = -1;
		par->gpio.led[i] = -1;
		par->gpio.aux[i] = -1;
	}

	if (pdata && pdata->gpios) {
		gpio = pdata->gpios;
		while (gpio->name[0]) {
			flags = FBTFT_GPIO_NO_MATCH;
			/* if driver provides match function, try it first,
			   if no match use our own */
			if (par->fbtftops.request_gpios_match)
				flags = par->fbtftops.request_gpios_match(par, gpio);
			if (flags == FBTFT_GPIO_NO_MATCH)
				flags = fbtft_request_gpios_match(par, gpio);
			if (flags != FBTFT_GPIO_NO_MATCH) {
				ret = gpio_request_one(gpio->gpio, flags,
					par->info->device->driver->name);
				if (ret < 0) {
					dev_err(par->info->device,
						"%s: gpio_request_one('%s'=%d) failed with %d\n",
						__func__, gpio->name,
						gpio->gpio, ret);
					return ret;
				}
				fbtft_par_dbg(DEBUG_REQUEST_GPIOS, par,
					"%s: '%s' = GPIO%d\n",
					__func__, gpio->name, gpio->gpio);
			}
			gpio++;
		}
	}

	return 0;
}

void fbtft_free_gpios(struct fbtft_par *par)
{
	struct fbtft_platform_data *pdata = NULL;
	const struct fbtft_gpio *gpio;

	fbtft_par_dbg(DEBUG_FREE_GPIOS, par, "%s()\n", __func__);

	if (par->spi)
		pdata = par->spi->dev.platform_data;
	if (par->pdev)
		pdata = par->pdev->dev.platform_data;

	if (pdata && pdata->gpios) {
		gpio = pdata->gpios;
		while (gpio->name[0]) {
			fbtft_par_dbg(DEBUG_FREE_GPIOS, par,
				"%s(): gpio_free('%s'=%d)\n",
				__func__, gpio->name, gpio->gpio);
			/* if the gpio wasn't recognized by request_gpios,
			   WARN() will protest */
			gpio_direction_input(gpio->gpio);
			gpio_free(gpio->gpio);
			gpio++;
		}
	}
}

#ifdef CONFIG_FB_BACKLIGHT
int fbtft_backlight_update_status(struct backlight_device *bd)
{
	struct fbtft_par *par = bl_get_data(bd);
	bool polarity = !!(bd->props.state & BL_CORE_DRIVER1);

	fbtft_par_dbg(DEBUG_BACKLIGHT, par,
		"%s: polarity=%d, power=%d, fb_blank=%d\n",
		__func__, polarity, bd->props.power, bd->props.fb_blank);

	if ((bd->props.power == FB_BLANK_UNBLANK) && (bd->props.fb_blank == FB_BLANK_UNBLANK))
		gpio_set_value(par->gpio.led[0], polarity);
	else
		gpio_set_value(par->gpio.led[0], !polarity);

	return 0;
}

int fbtft_backlight_get_brightness(struct backlight_device *bd)
{
	return bd->props.brightness;
}

void fbtft_unregister_backlight(struct fbtft_par *par)
{
	const struct backlight_ops *bl_ops;

	fbtft_par_dbg(DEBUG_BACKLIGHT, par, "%s()\n", __func__);

	if (par->info->bl_dev) {
		par->info->bl_dev->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(par->info->bl_dev);
		bl_ops = par->info->bl_dev->ops;
		backlight_device_unregister(par->info->bl_dev);
		par->info->bl_dev = NULL;
		kfree(bl_ops);
	}
}

void fbtft_register_backlight(struct fbtft_par *par)
{
	struct backlight_device *bd;
	struct backlight_properties bl_props = { 0, };
	struct backlight_ops *bl_ops;

	fbtft_par_dbg(DEBUG_BACKLIGHT, par, "%s()\n", __func__);

	if (par->gpio.led[0] == -1) {
		fbtft_par_dbg(DEBUG_BACKLIGHT, par,
			"%s(): led pin not set, exiting.\n", __func__);
		return;
	}

	bl_ops = kzalloc(sizeof(struct backlight_ops), GFP_KERNEL);
	if (!bl_ops) {
		dev_err(par->info->device,
			"%s: could not allocate memeory for backlight operations.\n",
			__func__);
		return;
	}

	bl_ops->get_brightness = fbtft_backlight_get_brightness;
	bl_ops->update_status = fbtft_backlight_update_status;
	bl_props.type = BACKLIGHT_RAW;
	/* Assume backlight is off, get polarity from current state of pin */
	bl_props.power = FB_BLANK_POWERDOWN;
	if (!gpio_get_value(par->gpio.led[0]))
		bl_props.state |= BL_CORE_DRIVER1;

	bd = backlight_device_register(dev_driver_string(par->info->device),
				par->info->device, par, bl_ops, &bl_props);
	if (IS_ERR(bd)) {
		dev_err(par->info->device,
			"cannot register backlight device (%ld)\n",
			PTR_ERR(bd));
		goto failed;
	}
	par->info->bl_dev = bd;

	if (!par->fbtftops.unregister_backlight)
		par->fbtftops.unregister_backlight = fbtft_unregister_backlight;

	return;

failed:
	kfree(bl_ops);
}
#else
void fbtft_register_backlight(struct fbtft_par *par) { };
void fbtft_unregister_backlight(struct fbtft_par *par) { };
#endif
EXPORT_SYMBOL(fbtft_register_backlight);
EXPORT_SYMBOL(fbtft_unregister_backlight);

void fbtft_set_addr_win(struct fbtft_par *par, int xs, int ys, int xe, int ye)
{
	fbtft_par_dbg(DEBUG_SET_ADDR_WIN, par,
		"%s(xs=%d, ys=%d, xe=%d, ye=%d)\n", __func__, xs, ys, xe, ye);

	/* Column address set */
	write_reg(par, 0x2A,
		(xs >> 8) & 0xFF, xs & 0xFF, (xe >> 8) & 0xFF, xe & 0xFF);

	/* Row adress set */
	write_reg(par, 0x2B,
		(ys >> 8) & 0xFF, ys & 0xFF, (ye >> 8) & 0xFF, ye & 0xFF);

	/* Memory write */
	write_reg(par, 0x2C);
}


void fbtft_reset(struct fbtft_par *par)
{
	if (par->gpio.reset == -1)
		return;
	fbtft_par_dbg(DEBUG_RESET, par, "%s()\n", __func__);
	gpio_set_value(par->gpio.reset, 0);
	udelay(20);
	gpio_set_value(par->gpio.reset, 1);
	mdelay(120);
}


void fbtft_update_display(struct fbtft_par *par, unsigned start_line, unsigned end_line)
{
	size_t offset, len;
	struct timespec ts_start, ts_end, ts_fps, ts_duration;
	long fps_ms, fps_us, duration_ms, duration_us;
	long fps, throughput;
	bool timeit = false;
	int ret = 0;

	if (unlikely(par->debug & (DEBUG_TIME_FIRST_UPDATE | DEBUG_TIME_EACH_UPDATE))) {
		if ((par->debug & DEBUG_TIME_EACH_UPDATE) || \
				((par->debug & DEBUG_TIME_FIRST_UPDATE) && !par->first_update_done)) {
			getnstimeofday(&ts_start);
			timeit = true;
		}
	}

	/* Sanity checks */
	if (start_line > end_line) {
		dev_warn(par->info->device,
			"%s: start_line=%u is larger than end_line=%u. Shouldn't happen, will do full display update\n",
			__func__, start_line, end_line);
		start_line = 0;
		end_line = par->info->var.yres - 1;
	}
	if (start_line > par->info->var.yres - 1 || end_line > par->info->var.yres - 1) {
		dev_warn(par->info->device,
			"%s: start_line=%u or end_line=%u is larger than max=%d. Shouldn't happen, will do full display update\n",
			__func__, start_line, end_line, par->info->var.yres - 1);
		start_line = 0;
		end_line = par->info->var.yres - 1;
	}

	fbtft_par_dbg(DEBUG_UPDATE_DISPLAY, par, "%s(start_line=%u, end_line=%u)\n",
		__func__, start_line, end_line);

	if (par->fbtftops.set_addr_win)
		par->fbtftops.set_addr_win(par, 0, start_line,
				par->info->var.xres-1, end_line);

	offset = start_line * par->info->fix.line_length;
	len = (end_line - start_line + 1) * par->info->fix.line_length;
	ret = par->fbtftops.write_vmem(par, offset, len);
	if (ret < 0)
		dev_err(par->info->device,
			"%s: write_vmem failed to update display buffer\n",
			__func__);

	if (unlikely(timeit)) {
		getnstimeofday(&ts_end);
		if (par->update_time.tv_nsec == 0 && par->update_time.tv_sec == 0) {
			par->update_time.tv_sec = ts_start.tv_sec;
			par->update_time.tv_nsec = ts_start.tv_nsec;
		}
		ts_fps = timespec_sub(ts_start, par->update_time);
		par->update_time.tv_sec = ts_start.tv_sec;
		par->update_time.tv_nsec = ts_start.tv_nsec;
		fps_ms = (ts_fps.tv_sec * 1000) + ((ts_fps.tv_nsec / 1000000) % 1000);
		fps_us = (ts_fps.tv_nsec / 1000) % 1000;
		fps = fps_ms * 1000 + fps_us;
		fps = fps ? 1000000 / fps : 0;

		ts_duration = timespec_sub(ts_end, ts_start);
		duration_ms = (ts_duration.tv_sec * 1000) + ((ts_duration.tv_nsec / 1000000) % 1000);
		duration_us = (ts_duration.tv_nsec / 1000) % 1000;
		throughput = duration_ms * 1000 + duration_us;
		throughput = throughput ? (len * 1000) / throughput : 0;
		throughput = throughput * 1000 / 1024;

		dev_info(par->info->device,
			"Display update: %ld kB/s (%ld.%.3ld ms), fps=%ld (%ld.%.3ld ms)\n",
			throughput, duration_ms, duration_us,
			fps, fps_ms, fps_us);
		par->first_update_done = true;
	}
}


void fbtft_mkdirty(struct fb_info *info, int y, int height)
{
	struct fbtft_par *par = info->par;
	struct fb_deferred_io *fbdefio = info->fbdefio;

	/* special case, needed ? */
	if (y == -1) {
		y = 0;
		height = info->var.yres - 1;
	}

	/* Mark display lines/area as dirty */
	spin_lock(&par->dirty_lock);
	if (y < par->dirty_lines_start)
		par->dirty_lines_start = y;
	if (y + height - 1 > par->dirty_lines_end)
		par->dirty_lines_end = y + height - 1;
	spin_unlock(&par->dirty_lock);

	/* Schedule deferred_io to update display (no-op if already on queue)*/
	schedule_delayed_work(&info->deferred_work, fbdefio->delay);
}

void fbtft_deferred_io(struct fb_info *info, struct list_head *pagelist)
{
	struct fbtft_par *par = info->par;
	unsigned dirty_lines_start, dirty_lines_end;
	struct page *page;
	unsigned long index;
	unsigned y_low = 0, y_high = 0;
	int count = 0;

	spin_lock(&par->dirty_lock);
	dirty_lines_start = par->dirty_lines_start;
	dirty_lines_end = par->dirty_lines_end;
	/* set display line markers as clean */
	par->dirty_lines_start = par->info->var.yres - 1;
	par->dirty_lines_end = 0;
	spin_unlock(&par->dirty_lock);

	/* Mark display lines as dirty */
	list_for_each_entry(page, pagelist, lru) {
		count++;
		index = page->index << PAGE_SHIFT;
		y_low = index / info->fix.line_length;
		y_high = (index + PAGE_SIZE - 1) / info->fix.line_length;
		fbtft_dev_dbg(DEBUG_DEFERRED_IO, par, info->device,
			"page->index=%lu y_low=%d y_high=%d\n",
			page->index, y_low, y_high);
		if (y_high > info->var.yres - 1)
			y_high = info->var.yres - 1;
		if (y_low < dirty_lines_start)
			dirty_lines_start = y_low;
		if (y_high > dirty_lines_end)
			dirty_lines_end = y_high;
	}

	par->fbtftops.update_display(info->par,
					dirty_lines_start, dirty_lines_end);
}


void fbtft_fb_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
	struct fbtft_par *par = info->par;

	fbtft_dev_dbg(DEBUG_FB_FILLRECT, par, info->dev,
		"%s: dx=%d, dy=%d, width=%d, height=%d\n",
		__func__, rect->dx, rect->dy, rect->width, rect->height);
	sys_fillrect(info, rect);

	par->fbtftops.mkdirty(info, rect->dy, rect->height);
}

void fbtft_fb_copyarea(struct fb_info *info, const struct fb_copyarea *area)
{
	struct fbtft_par *par = info->par;

	fbtft_dev_dbg(DEBUG_FB_COPYAREA, par, info->dev,
		"%s: dx=%d, dy=%d, width=%d, height=%d\n",
		__func__,  area->dx, area->dy, area->width, area->height);
	sys_copyarea(info, area);

	par->fbtftops.mkdirty(info, area->dy, area->height);
}

void fbtft_fb_imageblit(struct fb_info *info, const struct fb_image *image)
{
	struct fbtft_par *par = info->par;

	fbtft_dev_dbg(DEBUG_FB_IMAGEBLIT, par, info->dev,
		"%s: dx=%d, dy=%d, width=%d, height=%d\n",
		__func__,  image->dx, image->dy, image->width, image->height);
	sys_imageblit(info, image);

	par->fbtftops.mkdirty(info, image->dy, image->height);
}

ssize_t fbtft_fb_write(struct fb_info *info,
			const char __user *buf, size_t count, loff_t *ppos)
{
	struct fbtft_par *par = info->par;
	ssize_t res;

	fbtft_dev_dbg(DEBUG_FB_WRITE, par, info->dev,
		"%s: count=%zd, ppos=%llu\n", __func__,  count, *ppos);
	res = fb_sys_write(info, buf, count, ppos);

	/* TODO: only mark changed area
	   update all for now */
	par->fbtftops.mkdirty(info, -1, 0);

	return res;
}

/* from pxafb.c */
unsigned int chan_to_field(unsigned chan, struct fb_bitfield *bf)
{
	chan &= 0xffff;
	chan >>= 16 - bf->length;
	return chan << bf->offset;
}

int fbtft_fb_setcolreg(unsigned regno,
			       unsigned red, unsigned green, unsigned blue,
			       unsigned transp, struct fb_info *info)
{
	struct fbtft_par *par = info->par;
	unsigned val;
	int ret = 1;

	fbtft_dev_dbg(DEBUG_FB_SETCOLREG, par, info->dev,
		"%s(regno=%u, red=0x%X, green=0x%X, blue=0x%X, trans=0x%X)\n",
		__func__, regno, red, green, blue, transp);

	switch (info->fix.visual) {
	case FB_VISUAL_TRUECOLOR:
		if (regno < 16) {
			u32 *pal = info->pseudo_palette;

			val  = chan_to_field(red,   &info->var.red);
			val |= chan_to_field(green, &info->var.green);
			val |= chan_to_field(blue,  &info->var.blue);

			pal[regno] = val;
			ret = 0;
		}
		break;

	}
	return ret;
}

int fbtft_fb_blank(int blank, struct fb_info *info)
{
	struct fbtft_par *par = info->par;
	int ret = -EINVAL;

	fbtft_dev_dbg(DEBUG_FB_BLANK, par, info->dev, "%s(blank=%d)\n",
		__func__, blank);

	if (!par->fbtftops.blank)
		return ret;

	switch (blank) {
	case FB_BLANK_POWERDOWN:
	case FB_BLANK_VSYNC_SUSPEND:
	case FB_BLANK_HSYNC_SUSPEND:
	case FB_BLANK_NORMAL:
		ret = par->fbtftops.blank(par, true);
		break;
	case FB_BLANK_UNBLANK:
		ret = par->fbtftops.blank(par, false);
		break;
	}
	return ret;
}

void fbtft_merge_fbtftops(struct fbtft_ops *dst, struct fbtft_ops *src)
{
	if (src->write)
		dst->write = src->write;
	if (src->read)
		dst->read = src->read;
	if (src->write_vmem)
		dst->write_vmem = src->write_vmem;
	if (src->write_register)
		dst->write_register = src->write_register;
	if (src->set_addr_win)
		dst->set_addr_win = src->set_addr_win;
	if (src->reset)
		dst->reset = src->reset;
	if (src->mkdirty)
		dst->mkdirty = src->mkdirty;
	if (src->update_display)
		dst->update_display = src->update_display;
	if (src->init_display)
		dst->init_display = src->init_display;
	if (src->blank)
		dst->blank = src->blank;
	if (src->request_gpios_match)
		dst->request_gpios_match = src->request_gpios_match;
	if (src->request_gpios)
		dst->request_gpios = src->request_gpios;
	if (src->free_gpios)
		dst->free_gpios = src->free_gpios;
	if (src->verify_gpios)
		dst->verify_gpios = src->verify_gpios;
	if (src->register_backlight)
		dst->register_backlight = src->register_backlight;
	if (src->unregister_backlight)
		dst->unregister_backlight = src->unregister_backlight;
	if (src->set_var)
		dst->set_var = src->set_var;
	if (src->set_gamma)
		dst->set_gamma = src->set_gamma;
}

/**
 * fbtft_framebuffer_alloc - creates a new frame buffer info structure
 *
 * @display: pointer to structure describing the display
 * @dev: pointer to the device for this fb, this can be NULL
 *
 * Creates a new frame buffer info structure.
 *
 * Also creates and populates the following structures:
 *   info->fbops
 *   info->fbdefio
 *   info->pseudo_palette
 *   par->fbtftops
 *   par->txbuf
 *
 * Returns the new structure, or NULL if an error occurred.
 *
 */
struct fb_info *fbtft_framebuffer_alloc(struct fbtft_display *display,
					struct device *dev)
{
	struct fb_info *info;
	struct fbtft_par *par;
	struct fb_ops *fbops = NULL;
	struct fb_deferred_io *fbdefio = NULL;
	struct fbtft_platform_data *pdata = dev->platform_data;
	u8 *vmem = NULL;
	void *txbuf = NULL;
	void *buf = NULL;
	unsigned width;
	unsigned height;
	int txbuflen = display->txbuflen;
	unsigned bpp = display->bpp;
	unsigned fps = display->fps;
	unsigned rotate = 0;
	bool bgr = false;
	u8 startbyte = 0;
	int vmem_size;
	int *init_sequence = display->init_sequence;
	char *gamma = display->gamma;
	unsigned long *gamma_curves = NULL;

	/* sanity check */
	if (display->gamma_num * display->gamma_len > FBTFT_GAMMA_MAX_VALUES_TOTAL) {
		dev_err(dev,
			"%s: FBTFT_GAMMA_MAX_VALUES_TOTAL=%d is exceeded\n",
			__func__, FBTFT_GAMMA_MAX_VALUES_TOTAL);
		return NULL;
	}

	/* defaults */
	if (!fps)
		fps = 20;
	if (!bpp)
		bpp = 16;

	vmem_size = display->width*display->height*bpp/8;

	/* platform_data override ? */
	if (pdata) {
		if (pdata->fps)
			fps = pdata->fps;
		if (pdata->txbuflen)
			txbuflen = pdata->txbuflen;
		rotate = pdata->rotate;
		bgr = pdata->bgr;
		startbyte = pdata->startbyte;
		if (pdata->display.init_sequence)
			init_sequence = pdata->display.init_sequence;
		if (pdata->gamma)
			gamma = pdata->gamma;
		if (pdata->display.debug)
			display->debug = pdata->display.debug;
		if (pdata->display.backlight)
			display->backlight = pdata->display.backlight;
	}

	display->debug |= debug;
	fbtft_expand_debug_value(&display->debug);

	switch (rotate) {
	case 90:
	case 270:
		width =  display->height;
		height = display->width;
		break;
	default:
		width =  display->width;
		height = display->height;
	}

	vmem = vzalloc(vmem_size);
	if (!vmem)
		goto alloc_fail;

	fbops = kzalloc(sizeof(struct fb_ops), GFP_KERNEL);
	if (!fbops)
		goto alloc_fail;

	fbdefio = kzalloc(sizeof(struct fb_deferred_io), GFP_KERNEL);
	if (!fbdefio)
		goto alloc_fail;

	buf = kmalloc(128, GFP_KERNEL);
	if (!buf)
		goto alloc_fail;

	if (display->gamma_num && display->gamma_len) {
		gamma_curves = kzalloc(display->gamma_num * display->gamma_len * sizeof(gamma_curves[0]), GFP_KERNEL);
		if (!gamma_curves)
			goto alloc_fail;
	}

	info = framebuffer_alloc(sizeof(struct fbtft_par), dev);
	if (!info)
		goto alloc_fail;

	info->screen_base = (u8 __force __iomem *)vmem;
	info->fbops = fbops;
	info->fbdefio = fbdefio;

	fbops->owner        =      dev->driver->owner;
	fbops->fb_read      =      fb_sys_read;
	fbops->fb_write     =      fbtft_fb_write;
	fbops->fb_fillrect  =      fbtft_fb_fillrect;
	fbops->fb_copyarea  =      fbtft_fb_copyarea;
	fbops->fb_imageblit =      fbtft_fb_imageblit;
	fbops->fb_setcolreg =      fbtft_fb_setcolreg;
	fbops->fb_blank     =      fbtft_fb_blank;

	fbdefio->delay =           HZ/fps;
	fbdefio->deferred_io =     fbtft_deferred_io;
	fb_deferred_io_init(info);

	strncpy(info->fix.id, dev->driver->name, 16);
	info->fix.type =           FB_TYPE_PACKED_PIXELS;
	info->fix.visual =         FB_VISUAL_TRUECOLOR;
	info->fix.xpanstep =	   0;
	info->fix.ypanstep =	   0;
	info->fix.ywrapstep =	   0;
	info->fix.line_length =    width*bpp/8;
	info->fix.accel =          FB_ACCEL_NONE;
	info->fix.smem_len =       vmem_size;

	info->var.rotate =         rotate;
	info->var.xres =           width;
	info->var.yres =           height;
	info->var.xres_virtual =   info->var.xres;
	info->var.yres_virtual =   info->var.yres;
	info->var.bits_per_pixel = bpp;
	info->var.nonstd =         1;

	/* RGB565 */
	info->var.red.offset =     11;
	info->var.red.length =     5;
	info->var.green.offset =   5;
	info->var.green.length =   6;
	info->var.blue.offset =    0;
	info->var.blue.length =    5;
	info->var.transp.offset =  0;
	info->var.transp.length =  0;

	info->flags =              FBINFO_FLAG_DEFAULT | FBINFO_VIRTFB;

	par = info->par;
	par->info = info;
	par->pdata = dev->platform_data;
	par->debug = display->debug;
	par->buf = buf;
	spin_lock_init(&par->dirty_lock);
	par->bgr = bgr;
	par->startbyte = startbyte;
	par->init_sequence = init_sequence;
	par->gamma.curves = gamma_curves;
	par->gamma.num_curves = display->gamma_num;
	par->gamma.num_values = display->gamma_len;
	mutex_init(&par->gamma.lock);
	info->pseudo_palette = par->pseudo_palette;

	if (par->gamma.curves && gamma)
		fbtft_gamma_parse_str(par,
			par->gamma.curves, gamma, strlen(gamma));

	/* Transmit buffer */
	if (txbuflen == -1)
		txbuflen = vmem_size + 2; /* add in case startbyte is used */

#ifdef __LITTLE_ENDIAN
	if ((!txbuflen) && (bpp > 8))
		txbuflen = PAGE_SIZE; /* need buffer for byteswapping */
#endif

	if (txbuflen > 0) {
		if (dma) {
			dev->coherent_dma_mask = ~0;
			txbuf = dma_alloc_coherent(dev, txbuflen, &par->txbuf.dma, GFP_DMA);
		} else {
			txbuf = kmalloc(txbuflen, GFP_KERNEL);
		}
		if (!txbuf)
			goto alloc_fail;
		par->txbuf.buf = txbuf;
		par->txbuf.len = txbuflen;
	}

	/* default fbtft operations */
	par->fbtftops.write = fbtft_write_spi;
	par->fbtftops.read = fbtft_read_spi;
	par->fbtftops.write_vmem = fbtft_write_vmem16_bus8;
	par->fbtftops.write_register = fbtft_write_reg8_bus8;
	par->fbtftops.set_addr_win = fbtft_set_addr_win;
	par->fbtftops.reset = fbtft_reset;
	par->fbtftops.mkdirty = fbtft_mkdirty;
	par->fbtftops.update_display = fbtft_update_display;
	par->fbtftops.request_gpios = fbtft_request_gpios;
	par->fbtftops.free_gpios = fbtft_free_gpios;
	if (display->backlight)
		par->fbtftops.register_backlight = fbtft_register_backlight;

	/* use driver provided functions */
	fbtft_merge_fbtftops(&par->fbtftops, &display->fbtftops);

	return info;

alloc_fail:
	vfree(vmem);
	kfree(buf);
	kfree(fbops);
	kfree(fbdefio);
	kfree(gamma_curves);

	return NULL;
}
EXPORT_SYMBOL(fbtft_framebuffer_alloc);

/**
 * fbtft_framebuffer_release - frees up all memory used by the framebuffer
 *
 * @info: frame buffer info structure
 *
 */
void fbtft_framebuffer_release(struct fb_info *info)
{
	struct fbtft_par *par = info->par;

	fb_deferred_io_cleanup(info);
	vfree(info->screen_base);
	if (par->txbuf.buf) {
		if (par->txbuf.dma)
			dma_free_coherent(info->device, par->txbuf.len, par->txbuf.buf, par->txbuf.dma);
		else
			kfree(par->txbuf.buf);
	}
	kfree(par->buf);
	kfree(info->fbops);
	kfree(info->fbdefio);
	kfree(par->gamma.curves);
	framebuffer_release(info);
}
EXPORT_SYMBOL(fbtft_framebuffer_release);

/**
 *	fbtft_register_framebuffer - registers a tft frame buffer device
 *	@fb_info: frame buffer info structure
 *
 *  Sets SPI driverdata if needed
 *  Requests needed gpios.
 *  Initializes display
 *  Updates display.
 *	Registers a frame buffer device @fb_info.
 *
 *	Returns negative errno on error, or zero for success.
 *
 */
int fbtft_register_framebuffer(struct fb_info *fb_info)
{
	int ret;
	char text1[50] = "";
	char text2[50] = "";
	struct fbtft_par *par = fb_info->par;
	struct spi_device *spi = par->spi;

	/* sanity checks */
	if (!par->fbtftops.init_display) {
		dev_err(fb_info->device, "missing fbtftops.init_display()\n");
		return -EINVAL;
	}

	if (spi)
		spi_set_drvdata(spi, fb_info);
	if (par->pdev)
		platform_set_drvdata(par->pdev, fb_info);

	ret = par->fbtftops.request_gpios(par);
	if (ret < 0)
		goto reg_fail;

	if (par->fbtftops.verify_gpios) {
		ret = par->fbtftops.verify_gpios(par);
		if (ret < 0)
			goto reg_fail;
	}

	ret = par->fbtftops.init_display(par);
	if (ret < 0)
		goto reg_fail;
	if (par->fbtftops.set_var) {
		ret = par->fbtftops.set_var(par);
		if (ret < 0)
			goto reg_fail;
	}

	/* update the entire display */
	par->fbtftops.update_display(par, 0, par->info->var.yres - 1);

	if (par->fbtftops.set_gamma && par->gamma.curves) {
		ret = par->fbtftops.set_gamma(par, par->gamma.curves);
		if (ret)
			goto reg_fail;
	}

	if (par->fbtftops.register_backlight)
		par->fbtftops.register_backlight(par);

	ret = register_framebuffer(fb_info);
	if (ret < 0)
		goto reg_fail;

	fbtft_sysfs_init(par);

	if (par->txbuf.buf)
		sprintf(text1, ", %d KiB %sbuffer memory",
			par->txbuf.len >> 10, par->txbuf.dma ? "DMA " : "");
	if (spi)
		sprintf(text2, ", spi%d.%d at %d MHz", spi->master->bus_num,
				spi->chip_select, spi->max_speed_hz/1000000);
	dev_info(fb_info->dev,
		"%s frame buffer, %dx%d, %d KiB video memory%s, fps=%lu%s\n",
		fb_info->fix.id, fb_info->var.xres, fb_info->var.yres,
		fb_info->fix.smem_len >> 10, text1,
		HZ/fb_info->fbdefio->delay, text2);

#ifdef CONFIG_FB_BACKLIGHT
	/* Turn on backlight if available */
	if (fb_info->bl_dev) {
		fb_info->bl_dev->props.power = FB_BLANK_UNBLANK;
		fb_info->bl_dev->ops->update_status(fb_info->bl_dev);
	}
#endif

	return 0;

reg_fail:
	if (par->fbtftops.unregister_backlight)
		par->fbtftops.unregister_backlight(par);
	if (spi)
		spi_set_drvdata(spi, NULL);
	if (par->pdev)
		platform_set_drvdata(par->pdev, NULL);
	par->fbtftops.free_gpios(par);

	return ret;
}
EXPORT_SYMBOL(fbtft_register_framebuffer);

/**
 *	fbtft_unregister_framebuffer - releases a tft frame buffer device
 *	@fb_info: frame buffer info structure
 *
 *  Frees SPI driverdata if needed
 *  Frees gpios.
 *	Unregisters frame buffer device.
 *
 */
int fbtft_unregister_framebuffer(struct fb_info *fb_info)
{
	struct fbtft_par *par = fb_info->par;
	struct spi_device *spi = par->spi;
	int ret;

	if (spi)
		spi_set_drvdata(spi, NULL);
	if (par->pdev)
		platform_set_drvdata(par->pdev, NULL);
	if (par->fbtftops.unregister_backlight)
		par->fbtftops.unregister_backlight(par);
	fbtft_sysfs_exit(par);
	par->fbtftops.free_gpios(par);
	ret = unregister_framebuffer(fb_info);
	return ret;
}
EXPORT_SYMBOL(fbtft_unregister_framebuffer);

/**
 * fbtft_init_display() - Generic init_display() function
 * @par: Driver data
 *
 * Uses par->init_sequence to do the initialization
 *
 * Return: 0 if successful, negative if error
 */
int fbtft_init_display(struct fbtft_par *par)
{
	int buf[64];
	char msg[128];
	char str[16];
	int i = 0;
	int j;

	fbtft_par_dbg(DEBUG_INIT_DISPLAY, par, "%s()\n", __func__);

	/* sanity check */
	if (!par->init_sequence) {
		dev_err(par->info->device,
			"error: init_sequence is not set\n");
		return -EINVAL;
	}

	/* make sure stop marker exists */
	for (i = 0; i < FBTFT_MAX_INIT_SEQUENCE; i++)
		if (par->init_sequence[i] == -3)
			break;
	if (i == FBTFT_MAX_INIT_SEQUENCE) {
		dev_err(par->info->device,
			"missing stop marker at end of init sequence\n");
		return -EINVAL;
	}

	par->fbtftops.reset(par);
	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 0);  /* Activate chip */

	i = 0;
	while (i < FBTFT_MAX_INIT_SEQUENCE) {
		if (par->init_sequence[i] == -3) {
			/* done */
			return 0;
		}
		if (par->init_sequence[i] >= 0) {
			dev_err(par->info->device,
				"missing delimiter at position %d\n", i);
			return -EINVAL;
		}
		if (par->init_sequence[i+1] < 0) {
			dev_err(par->info->device,
				"missing value after delimiter %d at position %d\n",
				par->init_sequence[i], i);
			return -EINVAL;
		}
		switch (par->init_sequence[i]) {
		case -1:
			i++;
			/* make debug message */
			strcpy(msg, "");
			j = i + 1;
			while (par->init_sequence[j] >= 0) {
				sprintf(str, "0x%02X ", par->init_sequence[j]);
				strcat(msg, str);
				j++;
			}
			fbtft_par_dbg(DEBUG_INIT_DISPLAY, par,
				"init: write(0x%02X) %s\n",
				par->init_sequence[i], msg);

			/* Write */
			j = 0;
			while (par->init_sequence[i] >= 0) {
				if (j > 63) {
					dev_err(par->info->device,
					"%s: Maximum register values exceeded\n",
					__func__);
					return -EINVAL;
				}
				buf[j++] = par->init_sequence[i++];
			}
			par->fbtftops.write_register(par, j,
				buf[0], buf[1], buf[2], buf[3],
				buf[4], buf[5], buf[6], buf[7],
				buf[8], buf[9], buf[10], buf[11],
				buf[12], buf[13], buf[14], buf[15],
				buf[16], buf[17], buf[18], buf[19],
				buf[20], buf[21], buf[22], buf[23],
				buf[24], buf[25], buf[26], buf[27],
				buf[28], buf[29], buf[30], buf[31],
				buf[32], buf[33], buf[34], buf[35],
				buf[36], buf[37], buf[38], buf[39],
				buf[40], buf[41], buf[42], buf[43],
				buf[44], buf[45], buf[46], buf[47],
				buf[48], buf[49], buf[50], buf[51],
				buf[52], buf[53], buf[54], buf[55],
				buf[56], buf[57], buf[58], buf[59],
				buf[60], buf[61], buf[62], buf[63]);
			break;
		case -2:
			i++;
			fbtft_par_dbg(DEBUG_INIT_DISPLAY, par,
				"init: mdelay(%d)\n", par->init_sequence[i]);
			mdelay(par->init_sequence[i++]);
			break;
		default:
			dev_err(par->info->device,
				"unknown delimiter %d at position %d\n",
				par->init_sequence[i], i);
			return -EINVAL;
		}
	}

	dev_err(par->info->device,
		"%s: something is wrong. Shouldn't get here.\n", __func__);
	return -EINVAL;
}
EXPORT_SYMBOL(fbtft_init_display);

/**
 * fbtft_verify_gpios() - Generic verify_gpios() function
 * @par: Driver data
 *
 * Uses @spi, @pdev and @buswidth to determine which GPIOs is needed
 *
 * Return: 0 if successful, negative if error
 */
int fbtft_verify_gpios(struct fbtft_par *par)
{
	struct fbtft_platform_data *pdata;
	int i;

	fbtft_par_dbg(DEBUG_VERIFY_GPIOS, par, "%s()\n", __func__);

	pdata = par->info->device->platform_data;
	if (!pdata) {
		dev_warn(par->info->device,
			"%s(): buswidth value is not available\n", __func__);
		return 0;
	}

	if (pdata->display.buswidth != 9 && par->startbyte == 0 && \
							par->gpio.dc < 0) {
		dev_err(par->info->device,
			"Missing info about 'dc' gpio. Aborting.\n");
		return -EINVAL;
	}

	if (!par->pdev)
		return 0;

	if (par->gpio.wr < 0) {
		dev_err(par->info->device, "Missing 'wr' gpio. Aborting.\n");
		return -EINVAL;
	}
	for (i = 0; i < pdata->display.buswidth; i++) {
		if (par->gpio.db[i] < 0) {
			dev_err(par->info->device,
				"Missing 'db%02d' gpio. Aborting.\n", i);
			return -EINVAL;
		}
	}

	return 0;
}

/**
 * fbtft_probe_common() - Generic device probe() helper function
 * @display: Display properties
 * @sdev: SPI device
 * @pdev: Platform device
 *
 * Allocates, initializes and registers a framebuffer
 *
 * Either @sdev or @pdev should be NULL
 *
 * Return: 0 if successful, negative if error
 */
int fbtft_probe_common(struct fbtft_display *display,
			struct spi_device *sdev, struct platform_device *pdev)
{
	struct device *dev;
	struct fb_info *info;
	struct fbtft_par *par;
	struct fbtft_platform_data *pdata;
	int ret;

	if (sdev)
		dev = &sdev->dev;
	else
		dev = &pdev->dev;

	if (unlikely(display->debug & DEBUG_DRIVER_INIT_FUNCTIONS))
		dev_info(dev, "%s()\n", __func__);

	pdata = dev->platform_data;
	if (pdata) {
		if (pdata->display.width)
			display->width = pdata->display.width;
		if (pdata->display.height)
			display->height = pdata->display.height;
		if (pdata->display.buswidth)
			display->buswidth = pdata->display.buswidth;
	}

	info = fbtft_framebuffer_alloc(display, dev);
	if (!info)
		return -ENOMEM;

	par = info->par;
	if (sdev)
		par->spi = sdev;
	else
		par->pdev = pdev;

	/* write register functions */
	if (display->regwidth == 8 && display->buswidth == 8) {
		par->fbtftops.write_register = fbtft_write_reg8_bus8;
	} else
	if (display->regwidth == 8 && display->buswidth == 9 && par->spi) {
		par->fbtftops.write_register = fbtft_write_reg8_bus9;
	} else if (display->regwidth == 16 && display->buswidth == 8) {
		par->fbtftops.write_register = fbtft_write_reg16_bus8;
	} else if (display->regwidth == 16 && display->buswidth == 16) {
		par->fbtftops.write_register = fbtft_write_reg16_bus16;
	} else {
		dev_warn(dev,
			"no default functions for regwidth=%d and buswidth=%d\n",
			display->regwidth, display->buswidth);
	}

	/* write_vmem() functions */
	if (display->buswidth == 8)
		par->fbtftops.write_vmem = fbtft_write_vmem16_bus8;
	else if (display->buswidth == 9)
		par->fbtftops.write_vmem = fbtft_write_vmem16_bus9;
	else if (display->buswidth == 16)
		par->fbtftops.write_vmem = fbtft_write_vmem16_bus16;

	/* GPIO write() functions */
	if (par->pdev) {
		if (display->buswidth == 8)
			par->fbtftops.write = fbtft_write_gpio8_wr;
		else if (display->buswidth == 16)
			par->fbtftops.write = fbtft_write_gpio16_wr;
	}

	/* 9-bit SPI setup */
	if (par->spi && display->buswidth == 9) {
		par->spi->bits_per_word = 9;
		ret = par->spi->master->setup(par->spi);
		if (ret) {
			dev_warn(&par->spi->dev,
				"9-bit SPI not available, emulating using 8-bit.\n");
			par->spi->bits_per_word = 8;
			ret = par->spi->master->setup(par->spi);
			if (ret)
				goto out_release;
			/* allocate buffer with room for dc bits */
			par->extra = kmalloc(
				par->txbuf.len + (par->txbuf.len / 8) + 8,
				GFP_KERNEL);
			if (!par->extra) {
				ret = -ENOMEM;
				goto out_release;
			}
			par->fbtftops.write = fbtft_write_spi_emulate_9;
		}
	}

	if (!par->fbtftops.verify_gpios)
		par->fbtftops.verify_gpios = fbtft_verify_gpios;

	/* make sure we still use the driver provided functions */
	fbtft_merge_fbtftops(&par->fbtftops, &display->fbtftops);

	/* use init_sequence if provided */
	if (par->init_sequence)
		par->fbtftops.init_display = fbtft_init_display;

	/* use platform_data provided functions above all */
	if (pdata)
		fbtft_merge_fbtftops(&par->fbtftops, &pdata->display.fbtftops);

	ret = fbtft_register_framebuffer(info);
	if (ret < 0)
		goto out_release;

	return 0;

out_release:
	fbtft_framebuffer_release(info);

	return ret;
}
EXPORT_SYMBOL(fbtft_probe_common);

/**
 * fbtft_remove_common() - Generic device remove() helper function
 * @dev: Device
 * @info: Framebuffer
 *
 * Unregisters and releases the framebuffer
 *
 * Return: 0 if successful, negative if error
 */
int fbtft_remove_common(struct device *dev, struct fb_info *info)
{
	struct fbtft_par *par;

	if (!info)
		return -EINVAL;
	par = info->par;
	if (par) {
		fbtft_par_dbg(DEBUG_DRIVER_INIT_FUNCTIONS, par,
			"%s()\n", __func__);
		if (par->extra)
			kfree(par->extra);
	}
	fbtft_unregister_framebuffer(info);
	fbtft_framebuffer_release(info);

	return 0;
}
EXPORT_SYMBOL(fbtft_remove_common);

MODULE_LICENSE("GPL");