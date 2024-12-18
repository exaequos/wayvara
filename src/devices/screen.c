#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <limits.h>
#include <time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "../uxn.h"
#include "screen.h"
#include "mouse.h"
#include "controller.h"

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

#include <xkbcommon/xkbcommon-compose.h>

#include <emscripten.h>

/*
Copyright (c) 2021-2023 Devine Lu Linvega, Andrew Alderwick

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE.
*/

extern uint32_t zoom;

UxnScreen uxn_screen;

static int
clamp(int v, int min, int max)
{
	return v < min ? min : v > max ? max : v;
}

/* c = !ch ? (color % 5 ? color >> 2 : 0) : color % 4 + ch == 1 ? 0 : (ch - 2 + (color & 3)) % 3 + 1; */

static Uint8 blending[][16] = {
	{0, 0, 0, 0, 1, 0, 1, 1, 2, 2, 0, 2, 3, 3, 3, 0},
	{0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3},
	{1, 2, 3, 1, 1, 2, 3, 1, 1, 2, 3, 1, 1, 2, 3, 1},
	{2, 3, 1, 2, 2, 3, 1, 2, 2, 3, 1, 2, 2, 3, 1, 2},
	{0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0}};

void
screen_change(Uint16 x1, Uint16 y1, Uint16 x2, Uint16 y2)
{
	if(x1 > uxn_screen.width && x2 > x1) return;
	if(y1 > uxn_screen.height && y2 > y1) return;
	if(x1 > x2) x1 = 0;
	if(y1 > y2) y1 = 0;
	if(x1 < uxn_screen.x1) uxn_screen.x1 = x1;
	if(y1 < uxn_screen.y1) uxn_screen.y1 = y1;
	if(x2 > uxn_screen.x2) uxn_screen.x2 = x2;
	if(y2 > uxn_screen.y2) uxn_screen.y2 = y2;
}

void
screen_fill(Uint8 *layer, int color)
{
	int i, length = uxn_screen.width * uxn_screen.height;
	for(i = 0; i < length; i++)
		layer[i] = color;
}

void
screen_rect(Uint8 *layer, Uint16 x1, Uint16 y1, Uint16 x2, Uint16 y2, int color)
{
	int row, x, y, w = uxn_screen.width, h = uxn_screen.height;
	for(y = y1; y < y2 && y < h; y++)
		for(x = x1, row = y * w; x < x2 && x < w; x++)
			layer[x + row] = color;
}

static void
screen_2bpp(Uint8 *layer, Uint8 *addr, Uint16 x1, Uint16 y1, Uint16 color, int fx, int fy)
{
	int w = uxn_screen.width, h = uxn_screen.height, opaque = blending[4][color];
	Uint16 y, ymod = (fy < 0 ? 7 : 0), ymax = y1 + ymod + fy * 8;
	Uint16 x, xmod = (fx > 0 ? 7 : 0), xmax = x1 + xmod - fx * 8;
	for(y = y1 + ymod; y != ymax; y += fy, addr++) {
		int c = addr[0] | (addr[8] << 8), row = y * w;
		if(y < h)
			for(x = x1 + xmod; x != xmax; x -= fx, c >>= 1) {
				Uint8 ch = (c & 1) | ((c >> 7) & 2);
				if(x < w && (opaque || ch))
					layer[x + row] = blending[ch][color];
			}
	}
}

static void
screen_1bpp(Uint8 *layer, Uint8 *addr, Uint16 x1, Uint16 y1, Uint16 color, int fx, int fy)
{
	int w = uxn_screen.width, h = uxn_screen.height, opaque = blending[4][color];
	Uint16 y, ymod = (fy < 0 ? 7 : 0), ymax = y1 + ymod + fy * 8;
	Uint16 x, xmod = (fx > 0 ? 7 : 0), xmax = x1 + xmod - fx * 8;
	for(y = y1 + ymod; y != ymax; y += fy) {
		int c = *addr++, row = y * w;
		if(y < h)
			for(x = x1 + xmod; x != xmax; x -= fx, c >>= 1) {
				Uint8 ch = c & 1;
				if(x < w && (opaque || ch))
					layer[x + row] = blending[ch][color];
			}
	}
}

/* clang-format off */

static Uint8 icons[] = {
	0x00, 0x7c, 0x82, 0x82, 0x82, 0x82, 0x82, 0x7c, 0x00, 0x30, 0x10, 0x10, 0x10, 0x10, 0x10, 
	0x10, 0x00, 0x7c, 0x82, 0x02, 0x7c, 0x80, 0x80, 0xfe, 0x00, 0x7c, 0x82, 0x02, 0x1c, 0x02, 
	0x82, 0x7c, 0x00, 0x0c, 0x14, 0x24, 0x44, 0x84, 0xfe, 0x04, 0x00, 0xfe, 0x80, 0x80, 0x7c, 
	0x02, 0x82, 0x7c, 0x00, 0x7c, 0x82, 0x80, 0xfc, 0x82, 0x82, 0x7c, 0x00, 0x7c, 0x82, 0x02, 
	0x1e, 0x02, 0x02, 0x02, 0x00, 0x7c, 0x82, 0x82, 0x7c, 0x82, 0x82, 0x7c, 0x00, 0x7c, 0x82, 
	0x82, 0x7e, 0x02, 0x82, 0x7c, 0x00, 0x7c, 0x82, 0x02, 0x7e, 0x82, 0x82, 0x7e, 0x00, 0xfc, 
	0x82, 0x82, 0xfc, 0x82, 0x82, 0xfc, 0x00, 0x7c, 0x82, 0x80, 0x80, 0x80, 0x82, 0x7c, 0x00, 
	0xfc, 0x82, 0x82, 0x82, 0x82, 0x82, 0xfc, 0x00, 0x7c, 0x82, 0x80, 0xf0, 0x80, 0x82, 0x7c,
	0x00, 0x7c, 0x82, 0x80, 0xf0, 0x80, 0x80, 0x80 };
static Uint8 arrow[] = {
	0x00, 0x00, 0x00, 0xfe, 0x7c, 0x38, 0x10, 0x00 };

/* clang-format on */

static void
draw_byte(Uint8 b, Uint16 x, Uint16 y, Uint8 color)
{
	screen_1bpp(uxn_screen.fg, &icons[(b >> 4) << 3], x, y, color, 1, 1);
	screen_1bpp(uxn_screen.fg, &icons[(b & 0xf) << 3], x + 8, y, color, 1, 1);
	screen_change(x, y, x + 0x10, y + 0x8);
}

static void
screen_debugger(Uxn *u)
{
	int i;
	for(i = 0; i < 0x08; i++) {
		Uint8 pos = u->wst.ptr - 4 + i;
		Uint8 color = i > 4 ? 0x01 : !pos ? 0xc
			: i == 4                      ? 0x8
										  : 0x2;
		draw_byte(u->wst.dat[pos], i * 0x18 + 0x8, uxn_screen.height - 0x18, color);
	}
	for(i = 0; i < 0x08; i++) {
		Uint8 pos = u->rst.ptr - 4 + i;
		Uint8 color = i > 4 ? 0x01 : !pos ? 0xc
			: i == 4                      ? 0x8
										  : 0x2;
		draw_byte(u->rst.dat[pos], i * 0x18 + 0x8, uxn_screen.height - 0x10, color);
	}
	screen_1bpp(uxn_screen.fg, &arrow[0], 0x68, uxn_screen.height - 0x20, 3, 1, 1);
	for(i = 0; i < 0x20; i++)
		draw_byte(u->ram[i], (i & 0x7) * 0x18 + 0x8, ((i >> 3) << 3) + 0x8, 1 + !!u->ram[i]);
}

void
screen_palette(Uint8 *addr)
{
	int i, shift;
	for(i = 0, shift = 4; i < 4; ++i, shift ^= 4) {
		Uint8
			r = (addr[0 + i / 2] >> shift) & 0xf,
			g = (addr[2 + i / 2] >> shift) & 0xf,
			b = (addr[4 + i / 2] >> shift) & 0xf;
		uxn_screen.palette[i] = 0x0f000000 | b << 16 | g << 8 | r;
		uxn_screen.palette[i] |= uxn_screen.palette[i] << 4;
	}
	screen_change(0, 0, uxn_screen.width, uxn_screen.height);
}

struct wl_display *display;
struct wl_registry *registry;
struct wl_compositor *cp;
struct wl_shm *shm;
struct xdg_wm_base *wm_base;
struct wl_seat *seat;

struct wl_surface *surf;
struct xdg_surface *xdgsurf;
struct xdg_toplevel *toplvl;

struct wl_output *output;

struct zxdg_decoration_manager_v1 *manager;
struct zxdg_toplevel_decoration_v1 *deco;

struct wl_callback *cb;

static struct wl_keyboard *kbd;
static struct wl_pointer *ptr;

struct xkb_context *xkb_ctx;
struct xkb_state *xkb_state;
struct xkb_keymap *xkb_keymap;
struct xkb_compose_table *xkb_compose_table;
struct xkb_compose_state *xkb_compose_state;
xkb_mod_index_t xkb_alt;
xkb_mod_index_t xkb_ctrl;
xkb_mod_index_t xkb_shift;

static void buffer_release(void *data, struct wl_buffer *b)
{
  //printf("--> buffer_release: data=%p\n", data);
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

static int buffer_size;
static struct wl_buffer * buffer = NULL;
static void * buffer_data;

static int can_redraw_screen = 0;
static Uxn * uxn = NULL;

static int buffer_init(int width, int height)
{
  //emscripten_log(EM_LOG_CONSOLE, "screen: --> buffer_init %d %d", width, height);
  
	struct wl_shm_pool *pool;
	char shm_name[14];
	int fd, stride;
	int max = 100;
	
	stride = width * zoom * 4;
	buffer_size = stride * height * zoom;

	//printf("buffer_init: buf->size=%d\n", buf->size);

	srand(time(NULL));
	do {
		sprintf(shm_name, "/havoc-%d", rand() % 1000000);
		fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);

		//printf("buffer_init: name=%s fd=%d\n", shm_name, fd);
		
	} while (fd < 0 && errno == EEXIST && --max);

	if (fd < 0) {
		fprintf(stderr, "shm_open failed: %m\n");
		return -1;
	}
	shm_unlink(shm_name);

	if (ftruncate(fd, buffer_size) < 0) {
		fprintf(stderr, "ftruncate failed: %m\n");
		close(fd);
		return -1;
	}

	buffer_data = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			 fd, 0);

	//printf("buffer_init: fd=%d data=%p\n", fd, buf->data);
	

	if (buffer_data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %m\n");
		close(fd);
		return -1;
	}

	pool = wl_shm_create_pool(shm, fd, buffer_size);
	buffer = wl_shm_pool_create_buffer(pool, 0, width * zoom, height * zoom,
					   stride, WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buffer, &buffer_listener, buffer);
	wl_shm_pool_destroy(pool);
	
	close(fd);

	//emscripten_log(EM_LOG_CONSOLE, "screen: buffer_init -> fd=%d data=%p buffer=%p", fd, buffer_data, buffer);

	return 0;
}

static int buffer_close()
{

  //TOODO
  
  return 0;
}

void
screen_resize(Uint16 width, Uint16 height)
{
  //emscripten_log(EM_LOG_CONSOLE, "screen: --> screen_resize %d %d", width, height);
  
	Uint8 *bg, *fg;
	Uint32 *pixels = NULL;
	if(width < 0x8 || height < 0x8 || width >= 0x800 || height >= 0x800)
		return;
	if(uxn_screen.width == width && uxn_screen.height == height)
		return;
	bg = malloc(width * height), fg = malloc(width * height);
	if(bg && fg) {

	  if (buffer) {
	    buffer_close();
	  }

	  buffer_init(width, height);

	  pixels = buffer_data;
	}
	if(!bg || !fg || !pixels) {
		free(bg), free(fg);
		return;
	}
	free(uxn_screen.bg), free(uxn_screen.fg);
	uxn_screen.bg = bg, uxn_screen.fg = fg;
	uxn_screen.pixels = pixels;
	uxn_screen.width = width, uxn_screen.height = height;
	screen_fill(uxn_screen.bg, 0), screen_fill(uxn_screen.fg, 0);
	emu_resize(width, height);
	screen_change(0, 0, width, height);
}

static void frame_callback(void *data, struct wl_callback *cb, uint32_t time)
{
  wl_callback_destroy(cb);
  cb = NULL;
}

static const struct wl_callback_listener frame_listener = {
	frame_callback
};

void draw_frame() {

    can_redraw_screen = 0;

    wl_surface_attach(surf, buffer, 0, 0);

    //TODO improve damage
    wl_surface_damage(surf, 0, 0, uxn_screen.width * zoom, uxn_screen.height * zoom);

    cb = wl_surface_frame(surf);
    //wl_callback_add_listener(cb, &frame_listener, NULL);

    wl_surface_commit(surf);
}

void
screen_redraw(Uxn *u)
{
  //emscripten_log(EM_LOG_CONSOLE, "screen: --> screen_redraw w=%d h=%d zoom=%d buffer=%p", uxn_screen.width, uxn_screen.height, zoom, buffer);

  if (!uxn)
    uxn = u;
  
  if (!buffer)
    return;

  if (!can_redraw_screen)
    return;
  
	int i, j, o, y;
	Uint8 *fg = uxn_screen.fg, *bg = uxn_screen.bg;
	Uint16 w = uxn_screen.width, h = uxn_screen.height;
	Uint16 x1 = uxn_screen.x1, y1 = uxn_screen.y1;
	Uint16 x2 = uxn_screen.x2 > w ? w : uxn_screen.x2, y2 = uxn_screen.y2 > h ? h : uxn_screen.y2;
	Uint32 palette[16], *pixels = uxn_screen.pixels;
	
	uxn_screen.x1 = uxn_screen.y1 = 0xffff;
	uxn_screen.x2 = uxn_screen.y2 = 0;
	
	if(u->dev[0x0e])
		screen_debugger(u);
	for(i = 0; i < 16; i++)
		palette[i] = uxn_screen.palette[(i >> 2) ? (i >> 2) : (i & 3)];

	int x;
	
	for(y = y1; y < y2; y++) {
	  for(o = y * w, x = x1, i = x1 + o, j = x2 + o; i < j; i++,x++) {
	    if (zoom == 1) {
	      pixels[i] = palette[fg[i] << 2 | bg[i]];
	    }
	    else {
	      for (int l=0; l < zoom; l++)
		for (int k=0; k < zoom; k++)
		  pixels[(y*zoom+l)*(w*zoom)+x*zoom+k] = palette[fg[i] << 2 | bg[i]];
	    }
	  }
	}

	draw_frame();

	//emscripten_log(EM_LOG_CONSOLE, "screen: <-- screen_redraw");
}

/* screen registers */

static Uint16 rX, rY, rA, rMX, rMY, rMA, rML, rDX, rDY;

Uint8
screen_dei(Uxn *u, Uint8 addr)
{
	switch(addr) {
	case 0x22: return uxn_screen.width >> 8;
	case 0x23: return uxn_screen.width;
	case 0x24: return uxn_screen.height >> 8;
	case 0x25: return uxn_screen.height;
	case 0x28: return rX >> 8;
	case 0x29: return rX;
	case 0x2a: return rY >> 8;
	case 0x2b: return rY;
	case 0x2c: return rA >> 8;
	case 0x2d: return rA;
	default: return u->dev[addr];
	}
}

void
screen_deo(Uint8 *ram, Uint8 *d, Uint8 port)
{
	switch(port) {
	case 0x3: screen_resize(PEEK2(d + 2), uxn_screen.height); return;
	case 0x5: screen_resize(uxn_screen.width, PEEK2(d + 4)); return;
	case 0x6: rMX = d[0x6] & 0x1, rMY = d[0x6] & 0x2, rMA = d[0x6] & 0x4, rML = d[0x6] >> 4, rDX = rMX << 3, rDY = rMY << 2; return;
	case 0x8:
	case 0x9: rX = (d[0x8] << 8) | d[0x9]; return;
	case 0xa:
	case 0xb: rY = (d[0xa] << 8) | d[0xb]; return;
	case 0xc:
	case 0xd: rA = (d[0xc] << 8) | d[0xd]; return;
	case 0xe: {
		Uint8 ctrl = d[0xe];
		Uint8 color = ctrl & 0x3;
		Uint8 *layer = ctrl & 0x40 ? uxn_screen.fg : uxn_screen.bg;
		/* fill mode */
		if(ctrl & 0x80) {
			Uint16 x1, y1, x2, y2;
			if(ctrl & 0x10)
				x1 = 0, x2 = rX;
			else
				x1 = rX, x2 = uxn_screen.width;
			if(ctrl & 0x20)
				y1 = 0, y2 = rY;
			else
				y1 = rY, y2 = uxn_screen.height;
			screen_rect(layer, x1, y1, x2, y2, color);
			screen_change(x1, y1, x2, y2);
		}
		/* pixel mode */
		else {
			Uint16 w = uxn_screen.width;
			if(rX < w && rY < uxn_screen.height)
				layer[rX + rY * w] = color;
			screen_change(rX, rY, rX + 1, rY + 1);
			if(rMX) rX++;
			if(rMY) rY++;
		}
		return;
	}
	case 0xf: {
		Uint8 i;
		Uint8 ctrl = d[0xf];
		Uint8 twobpp = !!(ctrl & 0x80);
		Uint8 color = ctrl & 0xf;
		Uint8 *layer = ctrl & 0x40 ? uxn_screen.fg : uxn_screen.bg;
		int fx = ctrl & 0x10 ? -1 : 1;
		int fy = ctrl & 0x20 ? -1 : 1;
		Uint16 dxy = rDX * fy, dyx = rDY * fx, addr_incr = rMA << (1 + twobpp);
		if(twobpp)
			for(i = 0; i <= rML; i++, rA += addr_incr)
				screen_2bpp(layer, &ram[rA], rX + dyx * i, rY + dxy * i, color, fx, fy);
		else
			for(i = 0; i <= rML; i++, rA += addr_incr)
				screen_1bpp(layer, &ram[rA], rX + dyx * i, rY + dxy * i, color, fx, fy);
		screen_change(rX, rY, rX + dyx * rML + 8, rY + dxy * rML + 8);
		if(rMX) rX += rDX * fx;
		if(rMY) rY += rDY * fy;
		return;
	}
	}
}

static void ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	ping
};

static void shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
  //if (format == WL_SHM_FORMAT_ARGB8888)
  //term.shm_argb = true;
}

static const struct wl_shm_listener shm_listener = {
	shm_format
};

static void noop()
{
}

static void setup_compose(void)
{
	struct xkb_compose_table *compose_table;
	struct xkb_compose_state *compose_state;
	char *lang = getenv("LANG");

	/*if (lang)
	  printf("--> setup_compose: lang=%s\n", lang);
	else
	printf("--> setup_compose: NO lang\n");*/

	if (lang == NULL)
		return;

	compose_table =
		xkb_compose_table_new_from_locale(xkb_ctx,
						  lang,
						  XKB_COMPOSE_COMPILE_NO_FLAGS);
	if (!compose_table) {
		fprintf(stderr, "could not create XKB compose table "
				"for locale '%s'.\n", lang);
		return;
	}

	compose_state = xkb_compose_state_new(compose_table,
					      XKB_COMPOSE_STATE_NO_FLAGS);
	if (!compose_state) {
		fprintf(stderr, "could not create XKB compose state. "
				"Disabling compose.\n");
		xkb_compose_table_unref(compose_table);
		return;
	}

	xkb_compose_table_unref(xkb_compose_table);
	xkb_compose_state_unref(xkb_compose_state);
	xkb_compose_table = compose_table;
	xkb_compose_state = compose_state;
}

static void kbd_keymap(void *data, struct wl_keyboard *k, uint32_t fmt,
		       int32_t fd, uint32_t size)
{
  //emscripten_log(EM_LOG_CONSOLE, "screen: kbd_keymap fd=%d size=%d", fd, size);
  
	struct xkb_keymap *keymap;
	struct xkb_state *state;
	char *map;

	if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}

	//printf("--> kbd_keymap: fd=%x\n", fd);

	map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return;
	}

	keymap = xkb_keymap_new_from_string(xkb_ctx, map,
					    XKB_KEYMAP_FORMAT_TEXT_V1,
					    XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	close(fd);

	if (!keymap) {
		fprintf(stderr, "failed to compile keymap\n");
		return;
	}

	state = xkb_state_new(keymap);
	if (!state) {
		fprintf(stderr, "failed to create XKB state\n");
		xkb_keymap_unref(keymap);
		return;
	}

	xkb_keymap_unref(xkb_keymap);
	xkb_state_unref(xkb_state);
	xkb_keymap = keymap;
	xkb_state = state;

	setup_compose();

	xkb_ctrl = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL);
	xkb_alt = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_ALT);
	xkb_shift = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
	
}

static void kbd_enter(void *data, struct wl_keyboard *k, uint32_t serial,
		      struct wl_surface *surf, struct wl_array *keys)
{
}

static void kbd_leave(void *data, struct wl_keyboard *k, uint32_t serial,
		      struct wl_surface *surf)
{
  #if 0
	reset_repeat();
	#endif
}


static xkb_keysym_t compose(xkb_keysym_t sym)
{
	if (!xkb_compose_state)
		return sym;
	if (sym == XKB_KEY_NoSymbol)
		return sym;
	if (xkb_compose_state_feed(xkb_compose_state,
				   sym) != XKB_COMPOSE_FEED_ACCEPTED)
		return sym;

	switch (xkb_compose_state_get_status(xkb_compose_state)) {
	case XKB_COMPOSE_COMPOSED:
		return xkb_compose_state_get_one_sym(xkb_compose_state);
	case XKB_COMPOSE_COMPOSING:
	case XKB_COMPOSE_CANCELLED:
		return XKB_KEY_NoSymbol;
	case XKB_COMPOSE_NOTHING:
	default:
		return sym;
	}
	
}


static void kbd_key(void *data, struct wl_keyboard *k, uint32_t serial,
		    uint32_t time, uint32_t key, uint32_t state)
{
  //emscripten_log(EM_LOG_CONSOLE, "screen: kbd_key=%d state=%d", key, state);

  if ( (key >= 65353) && (key <= 65356) ) {

    char arrows[] = { 0x40, 0x10, 0x80, 0x20 };

    if (state == 0) {
      controller_up(uxn, &uxn->dev[0x80], arrows[key-65353]);
    }
    else {
      controller_down(uxn, &uxn->dev[0x80], arrows[key-65353]);
    }
      
    return;
  }
  else if ( key == 65499 ) {

    if (state == 0) {
      controller_up(uxn, &uxn->dev[0x80], 0x1);
    }
    else {
      controller_down(uxn, &uxn->dev[0x80], 0x1);
    }
    
    return;
  }
  
	xkb_keysym_t sym, lsym;
	uint32_t unicode;
	struct binding *b;
	void (*action)(void) = NULL;

	if (!xkb_keymap || !xkb_state)
		return;

	//emscripten_log(EM_LOG_CONSOLE, "kbd_key=%d state=%d", key, state);

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
	  //if (term.repeat.key == key)
	  //		reset_repeat();
		return;
	}

	//cursor_set(NULL);

        sym = compose(xkb_state_key_get_one_sym(xkb_state, key + 8));

	//emscripten_log(EM_LOG_CONSOLE, "sym=%d %x", sym, sym);
	
	unicode = xkb_keysym_to_utf32(sym);

	controller_key(uxn, &uxn->dev[0x80], unicode & 0xff);

	//emscripten_log(EM_LOG_CONSOLE, "unicode=%d %x", unicode, unicode);
	
	//if (unicode == 0)
	//	unicode = TSM_VTE_INVALID;

	lsym = xkb_keysym_to_lower(sym);

	//emscripten_log(EM_LOG_CONSOLE, "lsym=%d %x\n", lsym, lsym);
	
	//action_copy_serial = serial;
	#if 0
	b = term.binding;
	while (b) {
		if (term.mods == b->mods && lsym == b->sym) {
			b->action();
			action = b->action;
			break;
		}
		b = b->next;
		}

	if (!action)
		tsm_vte_handle_keyboard(term.vte, sym, XKB_KEY_NoSymbol,
					term.mods , unicode);
	
	if (xkb_keymap_key_repeats(term.xkb_keymap, key + 8)) {
		term.repeat.key = key;
		term.repeat.sym = sym;
		term.repeat.unicode = unicode;
		term.repeat.action = action;
		timerfd_settime(term.repeat.fd, 0, &term.repeat.its, NULL);
	}
	#endif
}

static void kbd_mods(void *data, struct wl_keyboard *k, uint32_t serial,
		     uint32_t depressed, uint32_t latched, uint32_t locked,
		     uint32_t group)
{
  #if 0
	if (!term.xkb_keymap || !term.xkb_state)
		return;

	//emscripten_log(EM_LOG_CONSOLE, "--> kbd_mods: %d %d %d %d", depressed, latched, locked, group);

	xkb_state_update_mask(term.xkb_state, depressed, latched, locked,
			      0, 0, group);

	term.mods = 0;
	if (xkb_state_mod_index_is_active(term.xkb_state, term.xkb_alt,
					  XKB_STATE_MODS_EFFECTIVE) == 1)
		term.mods |= TSM_ALT_MASK;
	if (xkb_state_mod_index_is_active(term.xkb_state, term.xkb_ctrl,
					  XKB_STATE_MODS_EFFECTIVE) == 1)
		term.mods |= TSM_CONTROL_MASK;
	if (xkb_state_mod_index_is_active(term.xkb_state, term.xkb_shift,
					  XKB_STATE_MODS_EFFECTIVE) == 1)
		term.mods |= TSM_SHIFT_MASK;

	//emscripten_log(EM_LOG_CONSOLE, "<-- term.mods: %d", term.mods);

	reset_repeat();
	#endif
}

static void kbd_repeat(void *data, struct wl_keyboard *k,
		       int32_t rate, int32_t delay)
{
#if 0
  //emscripten_log(EM_LOG_CONSOLE, "kbd_repeat: %d %d", rate, delay);
  
	if (rate == 0)
		return;
	else if (rate == 1)
		term.repeat.its.it_interval.tv_sec = 1;
	else
		term.repeat.its.it_interval.tv_nsec = 1000000000 / rate;

	term.repeat.its.it_value.tv_sec = delay / 1000;
	delay -= term.repeat.its.it_value.tv_sec * 1000;
	term.repeat.its.it_value.tv_nsec = delay * 1000 * 1000;
	#endif
}

static struct wl_keyboard_listener kbd_listener = {
	kbd_keymap,
	kbd_enter,
	kbd_leave,
	kbd_key,
	kbd_mods,
	kbd_repeat
};

static void ptr_enter(void *data, struct wl_pointer *wl_pointer,
		      uint32_t serial, struct wl_surface *surface,
		      wl_fixed_t x, wl_fixed_t y)
{
  #if 0
	term.ptr_x = x;
	term.ptr_y = y;

	term.cursor.enter_serial = serial;
	cursor_set(term.cursor.text);
	#endif
}

static void ptr_leave(void *data, struct wl_pointer *wl_pointer,
		      uint32_t serial, struct wl_surface *surface)
{
  #if 0
	cursor_unset();
	#endif
}

static void ptr_motion(void *data, struct wl_pointer *wl_pointer,
		       uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
  #if 0
  //emscripten_log(EM_LOG_CONSOLE, "ptr_motion: %d %d", x, y);
  
	term.ptr_x = x;
	term.ptr_y = y;

	switch (term.select) {
	case 1:
	  //ps_uncopy();
		term.select = 2;
		tsm_screen_selection_start(term.screen, grid_x(), grid_y());
		term.need_redraw = true;
		break;
	case 2:
		tsm_screen_selection_target(term.screen, grid_x(), grid_y());
		term.need_redraw = true;
	}

	if (term.cursor.current == NULL && term.cursor.text)
		cursor_set(term.cursor.text);
	#endif

	if (uxn) {

	  int x1 = clamp(x >> 8, 0, uxn_screen.width * zoom - 1) / zoom;
	  int y1 = clamp(y >> 8, 0, uxn_screen.height * zoom - 1) / zoom;

	  //emscripten_log(EM_LOG_CONSOLE, "screen: ptr_motion: %d %d (%d %d)", x, y, x1, y1);
	  mouse_pos(uxn, &uxn->dev[0x90], x1, y1);
	}
}

static void ptr_button(void *data, struct wl_pointer *wl_pointer,
		       uint32_t serial, uint32_t time, uint32_t button,
		       uint32_t state)
{
  emscripten_log(EM_LOG_CONSOLE, "ptr_button: button=%x state=%d", button, state);

  if (!uxn)
    return;
  
  switch (state) {
      
  case WL_POINTER_BUTTON_STATE_PRESSED:

    mouse_down(uxn, &uxn->dev[0x90], button - 0x10F);
      
    break;
  case WL_POINTER_BUTTON_STATE_RELEASED:

    mouse_up(uxn, &uxn->dev[0x90], button - 0x10F);
    break;

  default:
    break;
  }
}

static void ptr_axis(void *data, struct wl_pointer *wl_pointer,
		     uint32_t time, uint32_t axis, wl_fixed_t value)
{
	int v = wl_fixed_to_double(value) / 3;

	if (axis == 0)
	  mouse_scroll(uxn, &uxn->dev[0x90], 0, v);
	else
	  mouse_scroll(uxn, &uxn->dev[0x90], v, 0);

	#if 0
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return;

	if (v > 0)
		tsm_screen_sb_down(term.screen, v);
	else
		tsm_screen_sb_up(term.screen, -v);
	term.need_redraw = true;
	#endif
}

static void ptr_frame(void *data, struct wl_pointer *wl_pointer)
{
}

static void ptr_axis_source(void *data, struct wl_pointer *wl_pointer,
			    uint32_t axis_source)
{
}

static void ptr_axis_stop(void *data, struct wl_pointer *wl_pointer,
			  uint32_t time, uint32_t axis)
{
}

static void ptr_axis_discrete(void *data, struct wl_pointer *wl_pointer,
			      uint32_t axis, int32_t discrete)
{
}

static struct wl_pointer_listener ptr_listener = {
	ptr_enter,
	ptr_leave,
	ptr_motion,
	ptr_button,
	ptr_axis,
	ptr_frame,
	ptr_axis_source,
	ptr_axis_stop,
	ptr_axis_discrete,
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
	noop,
#endif
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !kbd) {
		kbd = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(kbd, &kbd_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && kbd) {
		wl_keyboard_release(kbd);
		kbd = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ptr) {
		ptr = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(ptr, &ptr_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && ptr) {
		wl_pointer_release(ptr);
		ptr = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	seat_capabilities,
	seat_name
};

static void geometry(void *data,
			 struct wl_output *wl_output,
			 int32_t x,
			 int32_t y,
			 int32_t physical_width,
			 int32_t physical_height,
			 int32_t subpixel,
			 const char *make,
			 const char *model,
		     int32_t transform) {

  //screen_physical_width = physical_width;
  //screen_physical_height = physical_height;


  //emscripten_log(EM_LOG_CONSOLE, "screen: geometry: %d %d %d %d", x, y, physical_width, physical_height);
}

//TODO: several modes with done event at the end

static void mode(void *data,
		     struct wl_output *wl_output,
		     uint32_t flags,
		     int32_t width,
		     int32_t height,
		 int32_t refresh) {

  //mode_width = width;
  //mode_height = height;

  //emscripten_log(EM_LOG_CONSOLE, "mode: width=%d height=%d refresh=%d\n", width, height, refresh);
}

void scale(void *data,
		      struct wl_output *wl_output,
	   int32_t factor) {

  //emscripten_log(EM_LOG_CONSOLE, "scale factor=%d\n", factor);
}

static const struct wl_output_listener output_listener = {
  .geometry = &geometry,
  .mode = &mode,
  .scale = &scale
};

static void registry_get(void *data, struct wl_registry *r, uint32_t id,
			 const char *i, uint32_t version)
{
  //emscripten_log(EM_LOG_CONSOLE, "screen: registry_get %s", i);
  
	if (strcmp(i, "wl_compositor") == 0) {
	  cp = wl_registry_bind(r, id, &wl_compositor_interface, 1);
	} else if (strcmp(i, "wl_shm") == 0) {
	  shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
	  wl_shm_add_listener(shm, &shm_listener, NULL);
	} else if (strcmp(i, "xdg_wm_base") == 0) {
	  wm_base = wl_registry_bind(r, id, &xdg_wm_base_interface,
						1);
	  xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (strcmp(i, "wl_seat") == 0) {
	  seat = wl_registry_bind(r, id, &wl_seat_interface, 5);
	  wl_seat_add_listener(seat, &seat_listener, NULL);
	  /*} else if (strcmp(i, "wl_data_device_manager") == 0) {
	    term.d_dm = wl_registry_bind(r, id,
	    &wl_data_device_manager_interface, 2);*/
	} /*else if (strcmp(i, "zwp_primary_selection_device_manager_v1") == 0) {
	    term.ps_dm = wl_registry_bind(r, id,
	    &zwp_primary_selection_device_manager_v1_interface, 1);
	    }*/
	else if (strcmp(i, "zxdg_decoration_manager_v1") == 0) {
	  manager = wl_registry_bind(r, id,
				     &zxdg_decoration_manager_v1_interface, 1);
	}
	else if (strcmp(i, "wl_output") == 0) {

	  output = wl_registry_bind(
				    r, id, &wl_output_interface, 3);

	  wl_registry_add_listener(output, &output_listener, NULL);
	  
	}
	
}

static void registry_loose(void *data, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener reg_listener = {
	registry_get,
	registry_loose
};


static void toplvl_configure(void *data, struct xdg_toplevel *xdg_toplevel,
			     int32_t width, int32_t height,
			     struct wl_array *state)
{

  //emscripten_log(EM_LOG_CONSOLE, "screen: toplvl_configure: %d %d", width, height);
}


extern int end_of_app;

static void toplvl_close(void *data, struct xdg_toplevel *t)
{
  end_of_app = 1;
}

static const struct xdg_toplevel_listener toplvl_listener = {
	toplvl_configure,
	toplvl_close,
	#ifdef XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION
	noop,
#endif
#ifdef XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION
	noop,
#endif
};

static void configure(void *d, struct xdg_surface *surf, uint32_t serial)
{
  //emscripten_log(EM_LOG_CONSOLE, "screen: xdg_surface configure");
  
  xdg_surface_ack_configure(surf, serial);

  can_redraw_screen = 1;
  
  /* Request another frame */
  if (uxn && uxn_screen.x2) {
    screen_redraw(uxn);
  }
}

static const struct xdg_surface_listener surf_listener = {
	configure
};

static void
wl_surface_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
  //emscripten_log(EM_LOG_CONSOLE, "screen: wl_surface_frame_done");
  
  /* Destroy this callback */
  wl_callback_destroy(cb);

  can_redraw_screen = 1;
  
  if (uxn) {

    Uint8 *vector_addr = &uxn->dev[0x20];
  
    uint16_t screen_vector = PEEK2(vector_addr);

    uxn_eval(uxn, screen_vector);
  
    /* Request another frame */
    if (uxn_screen.x2) {
      screen_redraw(uxn);
    }
  }
}

static const struct wl_callback_listener wl_surface_frame_listener = {
	.done = wl_surface_frame_done,
};

void screen_init() {

  //emscripten_log(EM_LOG_CONSOLE, "--> screen_init");

  display = wl_display_connect("");
  
  if (display == NULL) {
    //emscripten_log(EM_LOG_CONSOLE, "screen: could not connect to display");

    return;
  }

  //emscripten_log(EM_LOG_CONSOLE, "screen: wl display connect ok");

  registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &reg_listener, NULL);

  wl_display_roundtrip(display);

  surf = wl_compositor_create_surface(cp);
  
  if (surf == NULL) {
    //emscripten_log(EM_LOG_CONSOLE, "screen: could not create surface");
    return;
  }

  //emscripten_log(EM_LOG_CONSOLE, "screen: wl surface created surf=%p", surf);
  
  xdgsurf = xdg_wm_base_get_xdg_surface(wm_base, surf);
  
  if (xdgsurf == NULL) {
    //emscripten_log(EM_LOG_CONSOLE, "screen: could not create xdg_surface");
    return;
  }

  //emscripten_log(EM_LOG_CONSOLE, "screen: xdg surface created wm_base=%p xdgsurf=%p", wm_base, xdgsurf);
  
  xdg_surface_add_listener(xdgsurf, &surf_listener, NULL);

  toplvl = xdg_surface_get_toplevel(xdgsurf);
  
  if (toplvl == NULL) {
    //emscripten_log(EM_LOG_CONSOLE, "screen: could not create xdg_toplevel");
    return;
  }
  
  //emscripten_log(EM_LOG_CONSOLE, "screen: top level %p", toplvl);
  
  xdg_toplevel_add_listener(toplvl, &toplvl_listener, NULL);
  xdg_toplevel_set_title(toplvl, "Wayvara");

  //xdg_toplevel_set_app_id(term.toplvl, term.opt.app_id);

  deco = zxdg_decoration_manager_v1_get_toplevel_decoration(manager, toplvl);

  zxdg_toplevel_decoration_v1_set_mode(deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

  wl_surface_commit(surf);

  struct wl_callback *cb = wl_surface_frame(surf);

  wl_callback_add_listener(cb, &wl_surface_frame_listener, NULL);
  
  //emscripten_log(EM_LOG_CONSOLE, "<-- screen_init done");
}
