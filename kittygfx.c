/* See LICENSE file for copyright and license details. */
/*
 * Kitty graphics protocol support.
 *
 * This implements the subset used by interactive programs (OpenTUI/opencode,
 * kitten icat, ...): PNG and raw RGB/RGBA images, direct/file transmission,
 * chunked uploads, transmit+display, placement and deletion.  Images are kept
 * as RGBA buffers and composited over the terminal cell grid with XRender.
 */
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>

#include <png.h>
#include <zlib.h>

#include "st.h"
#include "kittygfx.h"

#define MAX_IMAGE_DIM       16384
#define MAX_IMAGE_PIXELS    (64u * 1024u * 1024u)
#define MAX_UPLOAD_BYTES    (64u * 1024u * 1024u)
#define MAX_IMAGE_BYTES     (256u * 1024u * 1024u)
#define MAX_PLACEMENTS      1024
#define MAX_IMAGES          512

typedef struct {
	uint32_t id;
	int w, h;
	int gen;
	size_t nbytes;
	unsigned char *pix; /* RGBA, straight alpha */
} GfxImage;

typedef struct {
	int screen;
	uint32_t image_id;
	uint32_t placement_id;
	int col, row;
	int xoff, yoff;      /* pixel offset inside the anchor cell (X/Y) */
	int sx, sy, sw, sh;  /* source rectangle (0 means "not given") */
	int cols, rows;      /* requested size in cells (0 means "not given") */
	int z;
	int span_cols, span_rows;
	int pad_x, pad_y;    /* letterbox padding inside the c/r rectangle */
	int virtual;         /* U=1: drawn only through Unicode placeholders */

	/* cached scaled pixmap */
	unsigned long pixmap;
	unsigned long picture;
	int cache_w, cache_h;
	int cache_gen;
	int cache_cellgen;
} GfxPlacement;

typedef struct {
	int has_action;
	char action;
	int has_id, has_pid;
	uint32_t id, pid;
	int has_format, format;
	int has_medium;
	char medium;
	int has_width, has_height;   /* s / v */
	int width, height;
	int has_cols, has_rows;
	int cols, rows;
	int has_sx, has_sy, has_sw, has_sh;
	int sx, sy, sw, sh;
	int has_xoff, has_yoff;
	int xoff, yoff;
	int has_z, z;
	int has_cursor, cursor;
	int has_quiet, quiet;
	int has_more, more;
	int has_compression;
	char compression;
	int has_delete;
	char delete_what;
	int has_unicode, unicode;    /* U=1: virtual placement for placeholders */
} GfxControl;

typedef struct {
	int active;
	GfxControl ctl;
	unsigned char *data;
	size_t len, cap;
	unsigned b64acc;
	int b64bits;
} GfxUpload;

static GfxImage *images;
static size_t nimages, images_cap;
static size_t image_bytes;

static GfxPlacement *placements;
static size_t nplacements, placements_cap;

static GfxUpload upload;
static uint32_t next_image_id = 1;
static int image_gen;

static int gfx_screen_no;
static int gfx_cw = 1, gfx_ch = 1;
static int gfx_cellgen;

/* state used while drawing */
static Display *gfx_dpy;
static unsigned long gfx_drawable;
static Visual *gfx_visual;
static int gfx_depth;
static int gfx_border;
static int gfx_winw, gfx_winh;
static unsigned short gfx_br, gfx_bg, gfx_bb;

static GfxImage *find_image(uint32_t id);
static void free_placement(GfxPlacement *p);
static int add_placement(const GfxControl *c, int screen, int cx, int cy);
static int delete_by_control(const GfxControl *c, int screen, int cx, int cy);
static int finish_upload(int screen, int cx, int cy, GfxResult *res);

/* ------------------------------------------------------------------ */
/* small helpers                                                       */

static void *
xrealloc0(void *p, size_t n)
{
	void *q = realloc(p, n);
	if (!q)
		die("out of memory");
	return q;
}

static void
upload_reset(void)
{
	free(upload.data);
	memset(&upload, 0, sizeof(upload));
}

/*
 * Row/column diacritics used by the Unicode placeholder encoding. The table
 * comes from kitty's rowcolumn-diacritics.txt: each entry is a contiguous
 * code point run and the index of its first character (0-based). Returns the
 * 1-based index expected by the protocol, or 0 when `code` is not in the table.
 */
uint16_t
gfx_diacritic_num(uint32_t code)
{
	static const struct {
		uint32_t first;
		uint16_t value;
		uint16_t count;
	} ranges[] = {
		{ 0x00305,   0,  1 },
		{ 0x0030D,   1,  2 },
		{ 0x00310,   3,  1 },
		{ 0x00312,   4,  1 },
		{ 0x0033D,   5,  3 },
		{ 0x00346,   8,  1 },
		{ 0x0034A,   9,  3 },
		{ 0x00350,  12,  3 },
		{ 0x00357,  15,  1 },
		{ 0x0035B,  16,  1 },
		{ 0x00363,  17, 13 },
		{ 0x00483,  30,  5 },
		{ 0x00592,  35,  4 },
		{ 0x00597,  39,  3 },
		{ 0x0059C,  42,  6 },
		{ 0x005A8,  48,  2 },
		{ 0x005AB,  50,  2 },
		{ 0x005AF,  52,  1 },
		{ 0x005C4,  53,  1 },
		{ 0x00610,  54,  8 },
		{ 0x00657,  62,  5 },
		{ 0x0065D,  67,  2 },
		{ 0x006D6,  69,  7 },
		{ 0x006DF,  76,  4 },
		{ 0x006E4,  80,  1 },
		{ 0x006E7,  81,  2 },
		{ 0x006EB,  83,  2 },
		{ 0x00730,  85,  1 },
		{ 0x00732,  86,  2 },
		{ 0x00735,  88,  2 },
		{ 0x0073A,  90,  1 },
		{ 0x0073D,  91,  1 },
		{ 0x0073F,  92,  3 },
		{ 0x00743,  95,  1 },
		{ 0x00745,  96,  1 },
		{ 0x00747,  97,  1 },
		{ 0x00749,  98,  2 },
		{ 0x007EB, 100,  7 },
		{ 0x007F3, 107,  1 },
		{ 0x00816, 108,  4 },
		{ 0x0081B, 112,  9 },
		{ 0x00825, 121,  3 },
		{ 0x00829, 124,  5 },
		{ 0x00951, 129,  1 },
		{ 0x00953, 130,  2 },
		{ 0x00F82, 132,  2 },
		{ 0x00F86, 134,  2 },
		{ 0x0135D, 136,  3 },
		{ 0x017DD, 139,  1 },
		{ 0x0193A, 140,  1 },
		{ 0x01A17, 141,  1 },
		{ 0x01A75, 142,  8 },
		{ 0x01B6B, 150,  1 },
		{ 0x01B6D, 151,  7 },
		{ 0x01CD0, 158,  3 },
		{ 0x01CDA, 161,  2 },
		{ 0x01CE0, 163,  1 },
		{ 0x01DC0, 164,  2 },
		{ 0x01DC3, 166,  7 },
		{ 0x01DCB, 173,  2 },
		{ 0x01DD1, 175, 22 },
		{ 0x01DFE, 197,  1 },
		{ 0x020D0, 198,  2 },
		{ 0x020D4, 200,  4 },
		{ 0x020DB, 204,  2 },
		{ 0x020E1, 206,  1 },
		{ 0x020E7, 207,  1 },
		{ 0x020E9, 208,  1 },
		{ 0x020F0, 209,  1 },
		{ 0x02CEF, 210,  3 },
		{ 0x02DE0, 213, 32 },
		{ 0x0A66F, 245,  1 },
		{ 0x0A67C, 246,  2 },
		{ 0x0A6F0, 248,  2 },
		{ 0x0A8E0, 250, 18 },
		{ 0x0AAB0, 268,  1 },
		{ 0x0AAB2, 269,  2 },
		{ 0x0AAB7, 271,  2 },
		{ 0x0AABE, 273,  2 },
		{ 0x0AAC1, 275,  1 },
		{ 0x0FE20, 276,  7 },
		{ 0x10A0F, 283,  1 },
		{ 0x10A38, 284,  1 },
		{ 0x1D185, 285,  5 },
		{ 0x1D1AA, 290,  4 },
		{ 0x1D242, 294,  3 },
	};

	for (size_t i = 0; i < LEN(ranges); i++) {
		if (code >= ranges[i].first &&
		    code - ranges[i].first < ranges[i].count)
			return ranges[i].value +
			       (uint16_t)(code - ranges[i].first) + 1;
	}
	return 0;
}

static int
append_bytes(const unsigned char *s, size_t n)
{
	if (n == 0)
		return 1;
	if (upload.len + n > MAX_UPLOAD_BYTES)
		return 0;
	if (upload.len + n > upload.cap) {
		size_t cap = upload.cap ? upload.cap : 4096;
		while (cap < upload.len + n)
			cap *= 2;
		upload.data = xrealloc0(upload.data, cap);
		upload.cap = cap;
	}
	memcpy(upload.data + upload.len, s, n);
	upload.len += n;
	return 1;
}

static int
b64val(unsigned char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

static int
append_base64(const char *s, size_t n)
{
	unsigned char out[4096];
	size_t o = 0;
	int v;

	for (size_t i = 0; i < n; i++) {
		if (s[i] == '=' || s[i] == '\n' || s[i] == '\r' ||
		    s[i] == ' ' || s[i] == '\t')
			continue;
		if ((v = b64val((unsigned char)s[i])) < 0)
			continue;
		upload.b64acc = (upload.b64acc << 6) | (unsigned)v;
		upload.b64bits += 6;
		if (upload.b64bits >= 8) {
			upload.b64bits -= 8;
			out[o++] = (upload.b64acc >> upload.b64bits) & 0xff;
			if (o == sizeof(out)) {
				if (!append_bytes(out, o))
					return 0;
				o = 0;
			}
		}
	}
	return append_bytes(out, o);
}

static unsigned char *
read_file(const char *path, size_t *len)
{
	FILE *f;
	long n;
	unsigned char *buf;
	struct stat st;

	/* Only regular files may be read from the terminal. */
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
	    st.st_size <= 0 || (unsigned long)st.st_size > MAX_UPLOAD_BYTES)
		return NULL;

	if (!(f = fopen(path, "rb")))
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	n = ftell(f);
	if (n <= 0 || (unsigned long)n > MAX_UPLOAD_BYTES) {
		fclose(f);
		return NULL;
	}
	rewind(f);
	buf = xmalloc((size_t)n + 1);
	if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	buf[n] = '\0';
	*len = (size_t)n;
	return buf;
}

/* ------------------------------------------------------------------ */
/* control string parsing                                              */

static void
set_int(int *has, int *dst, const char *v, size_t n)
{
	char tmp[32];
	char *end;
	long x;

	if (n == 0 || n >= sizeof(tmp))
		return;
	memcpy(tmp, v, n);
	tmp[n] = '\0';
	x = strtol(tmp, &end, 10);
	if (end == tmp)
		return;
	*dst = (int)x;
	*has = 1;
}

static void
set_uint(int *has, uint32_t *dst, const char *v, size_t n)
{
	char tmp[32];
	char *end;
	unsigned long x;

	if (n == 0 || n >= sizeof(tmp))
		return;
	memcpy(tmp, v, n);
	tmp[n] = '\0';
	x = strtoul(tmp, &end, 10);
	if (end == tmp)
		return;
	*dst = (uint32_t)x;
	*has = 1;
}

static void
parse_control(const char *s, size_t len, GfxControl *c)
{
	size_t i;

	memset(c, 0, sizeof(*c));
	for (i = 0; i < len; ) {
		char k;
		size_t start, end;
		const char *v;

		if (s[i] == ',') {
			i++;
			continue;
		}
		k = s[i++];
		if (i >= len || s[i] != '=')
			continue;
		start = ++i;
		while (i < len && s[i] != ',')
			i++;
		end = i;
		v = s + start;
		switch (k) {
		case 'a':
			if (end > start) {
				c->action = v[0];
				c->has_action = 1;
			}
			break;
		case 'i': set_uint(&c->has_id, &c->id, v, end - start); break;
		case 'p': set_uint(&c->has_pid, &c->pid, v, end - start); break;
		case 'f': set_int(&c->has_format, &c->format, v, end - start); break;
		case 't':
			if (end > start) {
				c->medium = v[0];
				c->has_medium = 1;
			}
			break;
		case 's': set_int(&c->has_width, &c->width, v, end - start); break;
		case 'v': set_int(&c->has_height, &c->height, v, end - start); break;
		case 'c': set_int(&c->has_cols, &c->cols, v, end - start); break;
		case 'r': set_int(&c->has_rows, &c->rows, v, end - start); break;
		case 'x': set_int(&c->has_sx, &c->sx, v, end - start); break;
		case 'y': set_int(&c->has_sy, &c->sy, v, end - start); break;
		case 'w': set_int(&c->has_sw, &c->sw, v, end - start); break;
		case 'h': set_int(&c->has_sh, &c->sh, v, end - start); break;
		case 'X': set_int(&c->has_xoff, &c->xoff, v, end - start); break;
		case 'Y': set_int(&c->has_yoff, &c->yoff, v, end - start); break;
		case 'z': set_int(&c->has_z, &c->z, v, end - start); break;
		case 'C': set_int(&c->has_cursor, &c->cursor, v, end - start); break;
		case 'U': set_int(&c->has_unicode, &c->unicode, v, end - start); break;
		case 'q': set_int(&c->has_quiet, &c->quiet, v, end - start); break;
		case 'm': set_int(&c->has_more, &c->more, v, end - start); break;
		case 'o':
			if (end > start) {
				c->compression = v[0];
				c->has_compression = 1;
			}
			break;
		case 'd':
			if (end > start) {
				c->delete_what = v[0];
				c->has_delete = 1;
			}
			break;
		default:
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* image storage                                                       */

static GfxImage *
find_image(uint32_t id)
{
	for (size_t i = 0; i < nimages; i++)
		if (images[i].id == id)
			return &images[i];
	return NULL;
}

static GfxPlacement *
find_placement(uint32_t image_id, uint32_t placement_id)
{
	for (size_t i = 0; i < nplacements; i++)
		if (placements[i].image_id == image_id &&
		    placements[i].placement_id == placement_id)
			return &placements[i];
	return NULL;
}

/* Placeholders do not carry a placement id; ratatui-image uses the default
 * placement. Fall back to the first virtual placement of the image so clients
 * that do send a p= still display. */
static GfxPlacement *
find_virtual_placement(uint32_t image_id)
{
	GfxPlacement *fallback = NULL;

	for (size_t i = 0; i < nplacements; i++) {
		GfxPlacement *p = &placements[i];

		if (p->screen != gfx_screen_no || !p->virtual ||
		    p->image_id != image_id)
			continue;
		if (p->placement_id == 0)
			return p;
		if (!fallback)
			fallback = p;
	}
	return fallback;
}

static void
remove_image_at(size_t idx)
{
	GfxImage *im = &images[idx];

	free(im->pix);
	image_bytes -= im->nbytes;
	memmove(images + idx, images + idx + 1,
	        (nimages - idx - 1) * sizeof(*images));
	nimages--;
}

static void
remove_image_data(uint32_t id)
{
	for (size_t i = 0; i < nimages; i++) {
		if (images[i].id == id) {
			remove_image_at(i);
			return;
		}
	}
}

static void
remove_image(uint32_t id)
{
	for (size_t i = 0; i < nplacements; ) {
		if (placements[i].image_id == id) {
			free_placement(&placements[i]);
			memmove(placements + i, placements + i + 1,
			        (nplacements - i - 1) * sizeof(*placements));
			nplacements--;
		} else {
			i++;
		}
	}
	remove_image_data(id);
}

static int
image_used(uint32_t id)
{
	for (size_t i = 0; i < nplacements; i++)
		if (placements[i].image_id == id)
			return 1;
	return 0;
}

/* Drop the least recently added image that is no longer displayed. */
static void
evict_images(size_t need)
{
	while (nimages > 0 &&
	       (image_bytes + need > MAX_IMAGE_BYTES || nimages >= MAX_IMAGES)) {
		size_t victim = nimages;
		int best = INT_MAX;

		for (size_t i = 0; i < nimages; i++) {
			if (image_used(images[i].id))
				continue;
			if (images[i].gen < best) {
				best = images[i].gen;
				victim = i;
			}
		}
		if (victim == nimages)
			break;
		remove_image_at(victim);
	}
}

static int
store_image(uint32_t id, unsigned char *pix, int w, int h)
{
	GfxImage *im;
	size_t nbytes;

	if (w <= 0 || h <= 0 || w > MAX_IMAGE_DIM || h > MAX_IMAGE_DIM)
		return 0;
	if ((size_t)w * (size_t)h > MAX_IMAGE_PIXELS)
		return 0;
	nbytes = (size_t)w * (size_t)h * 4;

	remove_image_data(id);
	evict_images(nbytes);

	if (image_bytes + nbytes > MAX_IMAGE_BYTES)
		return 0;

	if (nimages == images_cap) {
		images_cap = images_cap ? images_cap * 2 : 16;
		images = xrealloc0(images, images_cap * sizeof(*images));
	}
	im = &images[nimages++];
	im->id = id;
	im->w = w;
	im->h = h;
	im->pix = pix;
	im->nbytes = nbytes;
	im->gen = ++image_gen;
	image_bytes += nbytes;
	return 1;
}

/* ------------------------------------------------------------------ */
/* placements                                                          */

static void
free_placement(GfxPlacement *p)
{
	if (gfx_dpy) {
		if (p->picture)
			XRenderFreePicture(gfx_dpy, p->picture);
		if (p->pixmap)
			XFreePixmap(gfx_dpy, p->pixmap);
	}
	p->picture = 0;
	p->pixmap = 0;
}

static void
remove_placement_at(size_t idx)
{
	free_placement(&placements[idx]);
	memmove(placements + idx, placements + idx + 1,
	        (nplacements - idx - 1) * sizeof(*placements));
	nplacements--;
}

static void
placement_dims(const GfxPlacement *p, const GfxImage *im, int *dw, int *dh,
               int *sx, int *sy, int *sw, int *sh, int *padx, int *pady)
{
	int x = p->sx, y = p->sy;
	int w = p->sw > 0 ? p->sw : im->w - x;
	int h = p->sh > 0 ? p->sh : im->h - y;

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x > im->w) x = im->w;
	if (y > im->h) y = im->h;
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	if (x + w > im->w) w = im->w - x;
	if (y + h > im->h) h = im->h - y;
	if (w < 1) w = 1;
	if (h < 1) h = 1;

	*sx = x;
	*sy = y;
	*sw = w;
	*sh = h;
	*padx = *pady = 0;

	if (p->cols > 0 && p->rows > 0) {
		int rw = p->cols * gfx_cw;
		int rh = p->rows * gfx_ch;
		double scale = (double)rw / w;

		if ((double)rh / h < scale)
			scale = (double)rh / h;
		*dw = (int)(w * scale + 0.5);
		*dh = (int)(h * scale + 0.5);
		if (*dw > rw) *dw = rw;
		if (*dh > rh) *dh = rh;
		*padx = (rw - *dw) / 2;
		*pady = (rh - *dh) / 2;
	} else if (p->cols > 0) {
		*dw = p->cols * gfx_cw;
		*dh = (int)((double)h * *dw / w + 0.5);
	} else if (p->rows > 0) {
		*dh = p->rows * gfx_ch;
		*dw = (int)((double)w * *dh / h + 0.5);
	} else {
		*dw = w;
		*dh = h;
	}
	if (*dw < 1) *dw = 1;
	if (*dh < 1) *dh = 1;
}

static void
placement_spans(GfxPlacement *p)
{
	GfxImage *im = find_image(p->image_id);
	int dw, dh, sx, sy, sw, sh, padx, pady;

	if (!im) {
		p->span_cols = p->span_rows = 1;
		p->pad_x = p->pad_y = 0;
		return;
	}
	placement_dims(p, im, &dw, &dh, &sx, &sy, &sw, &sh, &padx, &pady);
	p->pad_x = padx;
	p->pad_y = pady;
	p->span_cols = (p->xoff + padx + dw + gfx_cw - 1) / gfx_cw;
	p->span_rows = (p->yoff + pady + dh + gfx_ch - 1) / gfx_ch;
	if (p->span_cols < 1) p->span_cols = 1;
	if (p->span_rows < 1) p->span_rows = 1;
}

static int
add_placement(const GfxControl *c, int screen, int cx, int cy)
{
	GfxImage *im;
	GfxPlacement *p;
	uint32_t pid = c->has_pid ? c->pid : 0;
	uint32_t iid = c->has_id ? c->id : 0;
	int xoff = c->has_xoff ? c->xoff : 0;
	int yoff = c->has_yoff ? c->yoff : 0;
	int sx = c->has_sx ? c->sx : 0;
	int sy = c->has_sy ? c->sy : 0;
	int sw = c->has_sw ? c->sw : 0;
	int sh = c->has_sh ? c->sh : 0;
	int cols = c->has_cols ? c->cols : 0;
	int rows = c->has_rows ? c->rows : 0;
	int z = c->has_z ? c->z : 0;
	int virt = c->has_unicode && c->unicode == 1;

	if (!iid)
		return 0;
	im = find_image(iid);
	if (!im)
		return 0;

	/* A virtual placement has no screen position of its own; its size is
	 * defined by the placeholder grid. Derive one when the client did not
	 * spell out c/r. */
	if (virt) {
		if (cols <= 0)
			cols = (im->w + gfx_cw - 1) / gfx_cw;
		if (rows <= 0)
			rows = (im->h + gfx_ch - 1) / gfx_ch;
		if (cols < 1) cols = 1;
		if (rows < 1) rows = 1;
	}

	/* a placement is identified by (image id, placement id) */
	for (size_t i = 0; i < nplacements; i++) {
		if (placements[i].image_id == iid &&
		    placements[i].placement_id == pid) {
			GfxPlacement *old = &placements[i];

			/* A repeated placement with identical geometry can keep
			 * its scaled pixmap; avoid rescaling on full repaints. */
			if (old->screen == screen && old->col == cx &&
			    old->row == cy && old->xoff == xoff &&
			    old->yoff == yoff && old->sx == sx && old->sy == sy &&
			    old->sw == sw && old->sh == sh && old->cols == cols &&
			    old->rows == rows && old->z == z &&
			    old->virtual == virt) {
				placement_spans(old);
				return 1;
			}
			free_placement(old);
			nplacements--;
			memmove(placements + i, placements + i + 1,
			        (nplacements - i) * sizeof(*placements));
			break;
		}
	}

	if (nplacements == MAX_PLACEMENTS)
		return 0;
	if (nplacements == placements_cap) {
		placements_cap = placements_cap ? placements_cap * 2 : 32;
		placements = xrealloc0(placements,
		                       placements_cap * sizeof(*placements));
	}

	p = &placements[nplacements++];
	memset(p, 0, sizeof(*p));
	p->screen = screen;
	p->image_id = iid;
	p->placement_id = pid;
	p->col = cx;
	p->row = cy;
	p->xoff = xoff;
	p->yoff = yoff;
	p->sx = sx;
	p->sy = sy;
	p->sw = sw;
	p->sh = sh;
	p->cols = cols;
	p->rows = rows;
	p->z = z;
	p->virtual = virt;
	placement_spans(p);
	return 1;
}

static int
delete_by_control(const GfxControl *c, int screen, int cx, int cy)
{
	char raw = c->has_delete ? c->delete_what : 0;
	char op = raw;
	int upper = 0, changed = 0;
	int px, py;

	if (raw >= 'A' && raw <= 'Z') {
		upper = 1;
		op = raw - 'A' + 'a';
	}

	/* Any delete command aborts an in-progress chunked upload. */
	if (upload.active) {
		upload_reset();
		changed = 1;
	}

	if (!raw) {
		if (c->has_id) {
			remove_image(c->id);
			return 1;
		}
		for (size_t i = 0; i < nplacements; ) {
			if (placements[i].screen == screen) {
				remove_placement_at(i);
				changed = 1;
			} else {
				i++;
			}
		}
		return changed;
	}

	if (op == 'a') {
		for (size_t i = 0; i < nplacements; ) {
			if (placements[i].screen == screen) {
				remove_placement_at(i);
				changed = 1;
			} else {
				i++;
			}
		}
		if (upper) {
			for (size_t i = 0; i < nimages; ) {
				if (!image_used(images[i].id)) {
					remove_image_at(i);
					changed = 1;
				} else {
					i++;
				}
			}
		}
		return changed;
	}

	if (op == 'i') {
		for (size_t i = 0; i < nplacements; ) {
			GfxPlacement *p = &placements[i];
			int hit = p->screen == screen;

			if (hit && c->has_id)
				hit = p->image_id == c->id;
			if (hit && c->has_pid)
				hit = p->placement_id == c->pid;
			if (hit) {
				remove_placement_at(i);
				changed = 1;
			} else {
				i++;
			}
		}
		if (upper && c->has_id) {
			remove_image(c->id);
			changed = 1;
		}
		return changed;
	}

	if (op == 'r') {
		uint32_t lo = c->has_sx ? (uint32_t)MAX(0, c->sx) : 0;
		uint32_t hi = c->has_sy ? (uint32_t)MAX(0, c->sy) : UINT_MAX;

		for (size_t i = 0; i < nplacements; ) {
			GfxPlacement *p = &placements[i];

			if (p->screen == screen && p->image_id >= lo &&
			    p->image_id <= hi) {
				remove_placement_at(i);
				changed = 1;
			} else {
				i++;
			}
		}
		if (upper)
			for (size_t i = 0; i < nimages; ) {
				if (!image_used(images[i].id)) {
					remove_image_at(i);
					changed = 1;
				} else {
					i++;
				}
			}
		return changed;
	}

	px = c->has_sx ? c->sx - 1 : cx;
	py = c->has_sy ? c->sy - 1 : cy;

	for (size_t i = 0; i < nplacements; ) {
		GfxPlacement *p = &placements[i];
		int hit = 0;

		if (p->screen != screen) {
			i++;
			continue;
		}
		switch (op) {
		case 'p':
			hit = px >= p->col && px < p->col + p->span_cols &&
			      py >= p->row && py < p->row + p->span_rows;
			break;
		case 'q':
			hit = c->has_z && p->z == c->z &&
			      px >= p->col && px < p->col + p->span_cols &&
			      py >= p->row && py < p->row + p->span_rows;
			break;
		case 'c':
			hit = cx >= p->col && cx < p->col + p->span_cols &&
			      cy >= p->row && cy < p->row + p->span_rows;
			break;
		case 'x':
			hit = px >= p->col && px < p->col + p->span_cols;
			break;
		case 'y':
			hit = py >= p->row && py < p->row + p->span_rows;
			break;
		case 'z':
			hit = c->has_z && p->z == c->z;
			break;
		default:
			break;
		}
		if (hit) {
			remove_placement_at(i);
			changed = 1;
		} else {
			i++;
		}
	}

	if (upper)
		for (size_t i = 0; i < nimages; ) {
			if (!image_used(images[i].id)) {
				remove_image_at(i);
				changed = 1;
			} else {
				i++;
			}
		}
	return changed;
}

/* ------------------------------------------------------------------ */
/* scrolling / erasing                                                 */

void
gfx_set_screen(int screen)
{
	gfx_screen_no = screen;
}

void
gfx_set_cell(int cw, int ch)
{
	if (cw < 1) cw = 1;
	if (ch < 1) ch = 1;
	if (cw == gfx_cw && ch == gfx_ch)
		return;
	gfx_cw = cw;
	gfx_ch = ch;
	gfx_cellgen++;
	for (size_t i = 0; i < nplacements; i++)
		placement_spans(&placements[i]);
}

void
gfx_scroll(int screen, int top, int bot, int n)
{
	if (n == 0)
		return;

	for (size_t i = 0; i < nplacements; ) {
		GfxPlacement *p = &placements[i];

		if (p->virtual || p->screen != screen ||
		    p->row < top || p->row > bot) {
			i++;
			continue;
		}
		p->row -= n;
		if (n > 0 && p->row + p->span_rows <= top) {
			remove_placement_at(i);
		} else if (n < 0 && p->row > bot) {
			remove_placement_at(i);
		} else {
			i++;
		}
	}
}

void
gfx_clear_screen(int screen)
{
	/* Erasing the screen removes the cells that show virtual placements but
	 * not the image data itself: a program may print the placeholders
	 * again without retransmitting. */
	for (size_t i = 0; i < nplacements; ) {
		if (placements[i].screen == screen && !placements[i].virtual)
			remove_placement_at(i);
		else
			i++;
	}
}

/* ------------------------------------------------------------------ */
/* upload / decoding                                                   */

static int
decode_png(const unsigned char *data, size_t len, unsigned char **out,
           int *w, int *h)
{
	png_image png;
	unsigned char *pix;
	size_t size;

	memset(&png, 0, sizeof(png));
	png.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&png, data, len))
		return 0;
	png.format = PNG_FORMAT_RGBA;
	size = PNG_IMAGE_SIZE(png);
	if (size == 0 || png.width > MAX_IMAGE_DIM || png.height > MAX_IMAGE_DIM ||
	    (size_t)png.width * png.height > MAX_IMAGE_PIXELS) {
		png_image_free(&png);
		return 0;
	}
	pix = xmalloc(size);
	if (!png_image_finish_read(&png, NULL, pix, 0, NULL)) {
		free(pix);
		png_image_free(&png);
		return 0;
	}
	*w = (int)png.width;
	*h = (int)png.height;
	*out = pix;
	png_image_free(&png);
	return 1;
}

static void
reply(uint32_t id, int has_id, int pid, int has_pid, int quiet,
      const char *msg)
{
	char buf[256];
	int n;

	if (quiet >= 2)
		return;
	if (quiet == 1 && strcmp(msg, "OK") == 0)
		return;
	if (has_id && has_pid)
		n = snprintf(buf, sizeof(buf), "\033_Gi=%u,p=%u;%s\033\\",
		             id, pid, msg);
	else if (has_id)
		n = snprintf(buf, sizeof(buf), "\033_Gi=%u;%s\033\\", id, msg);
	else if (has_pid)
		n = snprintf(buf, sizeof(buf), "\033_Gp=%u;%s\033\\", pid, msg);
	else
		n = snprintf(buf, sizeof(buf), "\033_G;%s\033\\", msg);
	if (n > 0)
		ttywrite(buf, (size_t)n, 0);
}

static int
raw_to_rgba(const GfxControl *c, const unsigned char *data, size_t len,
            unsigned char **out, int *ow, int *oh)
{
	int w = c->width, h = c->height;
	int bpp = c->format == 24 ? 3 : 4;
	size_t need;
	unsigned char *src = NULL, *pix;

	if (w <= 0 || h <= 0 || w > MAX_IMAGE_DIM || h > MAX_IMAGE_DIM ||
	    (size_t)w * h > MAX_IMAGE_PIXELS)
		return 0;
	need = (size_t)w * h * bpp;

	if (c->has_compression && c->compression == 'z') {
		uLongf dlen = need;
		src = xmalloc(need ? need : 1);
		if (uncompress(src, &dlen, data, len) != Z_OK || dlen != need) {
			free(src);
			return 0;
		}
	} else {
		if (len < need)
			return 0;
		src = (unsigned char *)data;
	}

	pix = xmalloc((size_t)w * h * 4);
	if (bpp == 4) {
		memcpy(pix, src, need);
	} else {
		for (size_t i = 0, j = 0; i < (size_t)w * h; i++, j += 3) {
			pix[i * 4 + 0] = src[j + 0];
			pix[i * 4 + 1] = src[j + 1];
			pix[i * 4 + 2] = src[j + 2];
			pix[i * 4 + 3] = 0xff;
		}
	}
	if (src != data && src != NULL)
		free(src);
	*out = pix;
	*ow = w;
	*oh = h;
	return 1;
}

static void
placement_cursor_moved(GfxControl *c, uint32_t id, uint32_t pid,
                       GfxResult *res)
{
	GfxPlacement *p = find_placement(id, pid);

	res->dirty = 1;
	/* Virtual placements are positioned by the placeholders themselves and
	 * never move the cursor. */
	if (p && p->virtual)
		return;
	if (!p || (c->has_cursor && c->cursor == 1))
		return;
	res->move = 1;
	res->col = p->col + p->span_cols;
	res->row = p->row + p->span_rows;
}

static int
finish_upload(int screen, int cx, int cy, GfxResult *res)
{
	GfxControl *c = &upload.ctl;
	unsigned char *data = upload.data;
	size_t len = upload.len;
	unsigned char *pix = NULL;
	unsigned char *filebuf = NULL;
	int w = 0, h = 0;
	uint32_t id = c->has_id ? c->id : 0;
	int ok = 0, placed = 0;

	(void)res;

	if (!c->has_action) {
		upload_reset();
		return 0;
	}

	if (c->action == 'p') {
		if (!c->has_id || !find_image(c->id)) {
			reply(c->has_id ? c->id : 0, c->has_id,
			      c->has_pid ? c->pid : 0, c->has_pid,
			      c->has_quiet ? c->quiet : 0, "ENOIMG");
		} else if (add_placement(c, screen, cx, cy)) {
			reply(c->id, 1, c->has_pid ? c->pid : 0, c->has_pid,
			      c->has_quiet ? c->quiet : 0, "OK");
			placed = 1;
			placement_cursor_moved(c, c->id,
			                       c->has_pid ? c->pid : 0, res);
		} else {
			reply(c->id, 1, c->has_pid ? c->pid : 0, c->has_pid,
			      c->has_quiet ? c->quiet : 0, "EPLACE");
		}
		upload_reset();
		return placed;
	}

	if (c->action != 't' && c->action != 'T') {
		upload_reset();
		return 0;
	}

	if (!id) {
		id = next_image_id++;
		c->id = id;
		c->has_id = 1;
	}

	if (c->has_medium && (c->medium == 'f' || c->medium == 't')) {
		char *path;

		if (len == 0 || len >= MAX_UPLOAD_BYTES) {
			reply(id, 1, 0, 0, c->has_quiet ? c->quiet : 0, "ENODATA");
			upload_reset();
			return 0;
		}
		path = xmalloc(len + 1);
		memcpy(path, data, len);
		path[len] = '\0';
		filebuf = read_file(path, &len);
		free(path);
		if (!filebuf) {
			reply(id, 1, 0, 0, c->has_quiet ? c->quiet : 0, "ENOFILE");
			upload_reset();
			return 0;
		}
		data = filebuf;
	}

	if (!c->has_format)
		c->format = 100;

	if (c->format == 100) {
		ok = decode_png(data, len, &pix, &w, &h);
	} else if (c->format == 24 || c->format == 32) {
		ok = raw_to_rgba(c, data, len, &pix, &w, &h);
	}

	if (ok && store_image(id, pix, w, h)) {
		if (c->action == 'T') {
			ok = add_placement(c, screen, cx, cy);
			placed = ok;
			if (placed)
				placement_cursor_moved(c, id,
				                       c->has_pid ? c->pid : 0,
				                       res);
		}
		reply(id, 1, c->has_pid ? c->pid : 0, c->has_pid,
		      c->has_quiet ? c->quiet : 0, ok ? "OK" : "EPLACE");
	} else {
		free(pix);
		reply(id, 1, 0, 0, c->has_quiet ? c->quiet : 0, "EDECODE");
	}

	free(filebuf);
	upload_reset();
	return placed;
}

/* ------------------------------------------------------------------ */
/* public entry points                                                 */

int
gfx_apc(const char *buf, size_t len, int screen, int cx, int cy,
        int termcols, GfxResult *res)
{
	const char *semi;
	GfxControl c;
	size_t ctrl_len, payload_len;

	(void)termcols;
	memset(res, 0, sizeof(*res));
	if (len < 2 || buf[0] != 'G')
		return 0;

	semi = memchr(buf + 1, ';', len - 1);
	if (semi) {
		ctrl_len = (size_t)(semi - (buf + 1));
		payload_len = len - 1 - ctrl_len - 1;
	} else {
		ctrl_len = len - 1;
		payload_len = 0;
	}
	parse_control(buf + 1, ctrl_len, &c);

	/* Immediate commands. */
	if (c.has_action && (c.action == 'q' || c.action == 'a' ||
	                     c.action == 'f')) {
		if (c.action == 'q') {
			if (c.has_id)
				reply(c.id, 1, c.has_pid ? c.pid : 0, c.has_pid,
				      c.has_quiet ? c.quiet : 0, "OK");
			else
				reply(0, 0, 0, 0,
				      c.has_quiet ? c.quiet : 0, "OK");
		}
		return 1;
	}
	if (c.has_action && c.action == 'd') {
		if (delete_by_control(&c, screen, cx, cy))
			res->dirty = 1;
		reply(c.has_id ? c.id : 0, c.has_id,
		      c.has_pid ? c.pid : 0, c.has_pid,
		      c.has_quiet ? c.quiet : 0, "OK");
		return 1;
	}

	/* A chunk continuation may carry no control keys at all. */
	if (!c.has_action && !upload.active)
		return 1;

	if (!upload.active) {
		upload.active = 1;
		upload.ctl = c;
	} else {
		/* merge newly supplied keys, keeping earlier ones otherwise */
		if (c.has_action) { upload.ctl.has_action = 1; upload.ctl.action = c.action; }
		if (c.has_id) { upload.ctl.has_id = 1; upload.ctl.id = c.id; }
		if (c.has_pid) { upload.ctl.has_pid = 1; upload.ctl.pid = c.pid; }
		if (c.has_format) { upload.ctl.has_format = 1; upload.ctl.format = c.format; }
		if (c.has_medium) { upload.ctl.has_medium = 1; upload.ctl.medium = c.medium; }
		if (c.has_width) { upload.ctl.has_width = 1; upload.ctl.width = c.width; }
		if (c.has_height) { upload.ctl.has_height = 1; upload.ctl.height = c.height; }
		if (c.has_cols) { upload.ctl.has_cols = 1; upload.ctl.cols = c.cols; }
		if (c.has_rows) { upload.ctl.has_rows = 1; upload.ctl.rows = c.rows; }
		if (c.has_sx) { upload.ctl.has_sx = 1; upload.ctl.sx = c.sx; }
		if (c.has_sy) { upload.ctl.has_sy = 1; upload.ctl.sy = c.sy; }
		if (c.has_sw) { upload.ctl.has_sw = 1; upload.ctl.sw = c.sw; }
		if (c.has_sh) { upload.ctl.has_sh = 1; upload.ctl.sh = c.sh; }
		if (c.has_xoff) { upload.ctl.has_xoff = 1; upload.ctl.xoff = c.xoff; }
		if (c.has_yoff) { upload.ctl.has_yoff = 1; upload.ctl.yoff = c.yoff; }
		if (c.has_z) { upload.ctl.has_z = 1; upload.ctl.z = c.z; }
		if (c.has_cursor) { upload.ctl.has_cursor = 1; upload.ctl.cursor = c.cursor; }
		if (c.has_unicode) { upload.ctl.has_unicode = 1; upload.ctl.unicode = c.unicode; }
		if (c.has_quiet) { upload.ctl.has_quiet = 1; upload.ctl.quiet = c.quiet; }
		if (c.has_compression) { upload.ctl.has_compression = 1; upload.ctl.compression = c.compression; }
	}

	if (payload_len && !append_base64(semi + 1, payload_len)) {
		reply(c.has_id ? c.id : 0, c.has_id, 0, 0,
		      c.has_quiet ? c.quiet : 0, "ETOOBIG");
		upload_reset();
		return 1;
	}

	if (c.has_more && c.more) {
		upload.ctl.has_more = 1;
		upload.ctl.more = 1;
		return 1;
	}

	finish_upload(screen, cx, cy, res);
	return 1;
}

void
gfx_reset(void)
{
	for (size_t i = 0; i < nplacements; i++)
		free_placement(&placements[i]);
	nplacements = 0;
	for (size_t i = 0; i < nimages; i++)
		free(images[i].pix);
	nimages = 0;
	image_bytes = 0;
	upload_reset();
	gfx_cellgen++;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */

static int
placement_cmp(const void *a, const void *b)
{
	const GfxPlacement *pa = a, *pb = b;

	if (pa->z != pb->z)
		return pa->z < pb->z ? -1 : 1;
	return 0;
}

static void
free_cache(GfxPlacement *p)
{
	if (gfx_dpy) {
		if (p->picture)
			XRenderFreePicture(gfx_dpy, p->picture);
		if (p->pixmap)
			XFreePixmap(gfx_dpy, p->pixmap);
	}
	p->picture = 0;
	p->pixmap = 0;
	p->cache_w = p->cache_h = 0;
}

static unsigned long
pack_pixel(XRenderPictFormat *fmt, int r, int g, int b, int a)
{
	unsigned long v = 0;

	v |= ((unsigned long)(((r * a + 127) / 255) &
	      (unsigned short)fmt->direct.redMask)) << fmt->direct.red;
	v |= ((unsigned long)(((g * a + 127) / 255) &
	      (unsigned short)fmt->direct.greenMask)) << fmt->direct.green;
	v |= ((unsigned long)(((b * a + 127) / 255) &
	      (unsigned short)fmt->direct.blueMask)) << fmt->direct.blue;
	if (fmt->direct.alphaMask)
		v |= ((unsigned long)(a & (unsigned short)fmt->direct.alphaMask))
		     << fmt->direct.alpha;
	return v;
}

/*
 * Resample the source rectangle into the destination pixmap.
 *
 * When an axis shrinks, average the whole source footprint instead of
 * sampling two neighbouring pixels: a terminal often receives the full
 * resolution image and scales it down to a few cells, and point sampling
 * there turns fine detail (text, photos) into visible blocks. Upscaled axes
 * keep the bilinear two-tap filter. Colors are averaged premultiplied so
 * transparent pixels do not darken glyph edges.
 */
static void
scale_rgba(uint32_t *dst, int dw, int dh, const unsigned char *src,
           int sw, int sx, int sy, int cropw, int croph,
           XRenderPictFormat *fmt, int has_alpha,
           int br, int bg, int bb)
{
	int xdown = dw <= cropw;
	int ydown = dh <= croph;

	for (int y = 0; y < dh; y++) {
		double ys = 0, ye = 0, ty = 0;
		int y0, y1;

		if (ydown) {
			ys = (double)y * croph / dh;
			ye = (double)(y + 1) * croph / dh;
			y0 = (int)floor(ys);
			y1 = (int)ceil(ye) - 1;
		} else {
			double fy = ((double)y + 0.5) * croph / dh - 0.5;
			y0 = (int)floor(fy);
			ty = fy - y0;
			y1 = y0 + 1;
			if (y0 < 0) { y0 = 0; ty = 0; }
			if (y1 < 0) y1 = 0;
			if (y0 > croph - 1) y0 = croph - 1;
			if (y1 > croph - 1) y1 = croph - 1;
		}

		for (int x = 0; x < dw; x++) {
			double xs = 0, xe = 0, tx = 0;
			double ar = 0, ag = 0, ab = 0, aa = 0, wsum = 0;
			int x0, x1;
			int r = 0, g = 0, b = 0, a = 0;

			if (xdown) {
				xs = (double)x * cropw / dw;
				xe = (double)(x + 1) * cropw / dw;
				x0 = (int)floor(xs);
				x1 = (int)ceil(xe) - 1;
			} else {
				double fx = ((double)x + 0.5) * cropw / dw - 0.5;
				x0 = (int)floor(fx);
				tx = fx - x0;
				x1 = x0 + 1;
				if (x0 < 0) { x0 = 0; tx = 0; }
				if (x1 < 0) x1 = 0;
				if (x0 > cropw - 1) x0 = cropw - 1;
				if (x1 > cropw - 1) x1 = cropw - 1;
			}

			for (int yy = y0; yy <= y1; yy++) {
				double wy;
				const unsigned char *row;

				if (yy < 0 || yy >= croph)
					continue;
				if (ydown)
					wy = MIN(yy + 1.0, ye) - MAX((double)yy, ys);
				else
					wy = (y0 == y1) ? 1.0 :
					     (yy == y0 ? 1 - ty : ty);
				if (wy <= 0)
					continue;
				row = src + ((size_t)(sy + yy) * sw + sx) * 4;
				for (int xx = x0; xx <= x1; xx++) {
					double wx, w, sa;
					const unsigned char *p;

					if (xx < 0 || xx >= cropw)
						continue;
					if (xdown)
						wx = MIN(xx + 1.0, xe) -
						     MAX((double)xx, xs);
					else
						wx = (x0 == x1) ? 1.0 :
						     (xx == x0 ? 1 - tx : tx);
					if (wx <= 0)
						continue;
					p = row + (size_t)xx * 4;
					w = wx * wy;
					sa = p[3] / 255.0;
					ar += w * p[0] * sa;
					ag += w * p[1] * sa;
					ab += w * p[2] * sa;
					aa += w * p[3];
					wsum += w;
				}
			}

			if (aa > 0 && wsum > 0) {
				r = (int)(255.0 * ar / aa + 0.5);
				g = (int)(255.0 * ag / aa + 0.5);
				b = (int)(255.0 * ab / aa + 0.5);
				a = (int)(aa / wsum + 0.5);
			}
			if (r > 255) r = 255;
			if (g > 255) g = 255;
			if (b > 255) b = 255;
			if (a < 0) a = 0;
			if (a > 255) a = 255;

			if (!has_alpha) {
				r = (r * a + br * (255 - a)) / 255;
				g = (g * a + bg * (255 - a)) / 255;
				b = (b * a + bb * (255 - a)) / 255;
				a = 255;
			}
			dst[(size_t)y * dw + x] = pack_pixel(fmt, r, g, b, a);
		}
	}
}

static int
ensure_cache(GfxPlacement *p, XRenderPictFormat *fmt, int has_alpha)
{
	GfxImage *im = find_image(p->image_id);
	int dw, dh, sx, sy, sw, sh, padx, pady;
	uint32_t *pixels;
	XImage *xi;
	GC gc;

	if (!im)
		return 0;
	placement_dims(p, im, &dw, &dh, &sx, &sy, &sw, &sh, &padx, &pady);
	p->pad_x = padx;
	p->pad_y = pady;
	if (dw > MAX_IMAGE_DIM || dh > MAX_IMAGE_DIM)
		return 0;
	if ((size_t)dw * dh > MAX_IMAGE_PIXELS)
		return 0;

	if (p->pixmap && p->cache_w == dw && p->cache_h == dh &&
	    p->cache_gen == im->gen && p->cache_cellgen == gfx_cellgen)
		return 1;

	free_cache(p);

	p->pixmap = XCreatePixmap(gfx_dpy, gfx_drawable, dw, dh, gfx_depth);
	if (!p->pixmap)
		return 0;
	p->picture = XRenderCreatePicture(gfx_dpy, p->pixmap, fmt, 0, NULL);
	if (!p->picture) {
		XFreePixmap(gfx_dpy, p->pixmap);
		p->pixmap = 0;
		return 0;
	}

	pixels = xmalloc((size_t)dw * dh * 4);
	scale_rgba(pixels, dw, dh, im->pix, im->w, sx, sy, sw, sh,
	           fmt, has_alpha, gfx_br >> 8, gfx_bg >> 8, gfx_bb >> 8);

	xi = XCreateImage(gfx_dpy, gfx_visual, gfx_depth, ZPixmap, 0,
	                  (char *)pixels, dw, dh, 32, 0);
	if (!xi) {
		free(pixels);
		free_cache(p);
		return 0;
	}
	gc = XCreateGC(gfx_dpy, p->pixmap, 0, NULL);
	XPutImage(gfx_dpy, p->pixmap, gc, xi, 0, 0, 0, 0, dw, dh);
	XFreeGC(gfx_dpy, gc);
	XDestroyImage(xi); /* frees pixels */

	p->cache_w = dw;
	p->cache_h = dh;
	p->cache_gen = im->gen;
	p->cache_cellgen = gfx_cellgen;
	return 1;
}

void
gfx_draw(void *dpy, unsigned long drawable, void *visual, int depth,
         int cw, int ch, int borderpx, int winw, int winh,
         unsigned short br, unsigned short bg, unsigned short bb)
{
	XRenderPictFormat *fmt, *dstfmt;
	Picture dst;
	int has_alpha;

	if (nplacements == 0)
		return;
	if (!dpy || !visual)
		return;

	gfx_dpy = dpy;
	gfx_drawable = drawable;
	gfx_visual = visual;
	gfx_depth = depth;
	gfx_border = borderpx;
	gfx_winw = winw;
	gfx_winh = winh;
	gfx_br = br;
	gfx_bg = bg;
	gfx_bb = bb;

	if (cw != gfx_cw || ch != gfx_ch) {
		gfx_cw = cw > 0 ? cw : 1;
		gfx_ch = ch > 0 ? ch : 1;
		gfx_cellgen++;
		for (size_t i = 0; i < nplacements; i++)
			placement_spans(&placements[i]);
	}

	fmt = XRenderFindVisualFormat(gfx_dpy, gfx_visual);
	if (!fmt)
		return;
	dstfmt = fmt;
	has_alpha = fmt->direct.alphaMask != 0;
	if (getenv("ST_GFX_DEBUG"))
		fprintf(stderr, "gfx: n=%zu depth=%d fmt=%d/%d r=%d/%lx g=%d/%lx b=%d/%lx a=%d/%lx\n",
		        nplacements,
		        gfx_depth, fmt->depth, fmt->type,
		        fmt->direct.red, (unsigned long)fmt->direct.redMask,
		        fmt->direct.green, (unsigned long)fmt->direct.greenMask,
		        fmt->direct.blue, (unsigned long)fmt->direct.blueMask,
		        fmt->direct.alpha, (unsigned long)fmt->direct.alphaMask);

	dst = XRenderCreatePicture(gfx_dpy, gfx_drawable, dstfmt, 0, NULL);
	if (!dst)
		return;

	qsort(placements, nplacements, sizeof(*placements), placement_cmp);

	for (size_t i = 0; i < nplacements; i++) {
		GfxPlacement *p = &placements[i];
		int x, y;

		if (p->screen != gfx_screen_no)
			continue;
		if (p->virtual)
			continue;
		if (!ensure_cache(p, fmt, has_alpha))
			continue;

		x = gfx_border + p->col * gfx_cw + p->xoff + p->pad_x;
		y = gfx_border + p->row * gfx_ch + p->yoff + p->pad_y;
		if (x >= gfx_winw || y >= gfx_winh ||
		    x + p->cache_w <= 0 || y + p->cache_h <= 0)
			continue;

		XRenderComposite(gfx_dpy, has_alpha ? PictOpOver : PictOpSrc,
		                 p->picture, None, dst,
		                 0, 0, 0, 0, x, y,
		                 p->cache_w, p->cache_h);
	}

	XRenderFreePicture(gfx_dpy, dst);
}

/*
 * Compose the image fragment for every Unicode placeholder cell on one line.
 * Placeholder cells are ordinary Glyph entries (with ATTR_IMAGE), so they
 * move and scroll with the text. Only the first cell of a row carries the
 * row/column diacritics; later cells inherit them, as the protocol allows.
 */
void
gfx_draw_line_placeholders(void *dpy, unsigned long drawable, void *visual,
                           int depth, int cw, int ch, int borderpx,
                           int winw, int winh,
                           unsigned short br, unsigned short bg,
                           unsigned short bb, const void *lineptr,
                           int y, int x1, int x2, int only_x)
{
	const Glyph *line = lineptr;
	XRenderPictFormat *fmt;
	Picture dst;
	int has_alpha;
	int cur_row = -1, cur_col = -1, cur_extra = -1, prev = 0;
	int found = 0;

	if (!line || !dpy || !visual || x2 <= x1)
		return;
	if (only_x >= 0) {
		found = only_x >= x1 && only_x < x2 &&
		        (line[only_x].mode & ATTR_IMAGE);
	} else {
		for (int x = x1; x < x2; x++) {
			if (line[x].mode & ATTR_IMAGE) {
				found = 1;
				break;
			}
		}
	}
	if (!found)
		return;

	gfx_dpy = dpy;
	gfx_drawable = drawable;
	gfx_visual = visual;
	gfx_depth = depth;
	gfx_border = borderpx;
	gfx_winw = winw;
	gfx_winh = winh;
	gfx_br = br;
	gfx_bg = bg;
	gfx_bb = bb;

	if (cw != gfx_cw || ch != gfx_ch) {
		gfx_cw = cw > 0 ? cw : 1;
		gfx_ch = ch > 0 ? ch : 1;
		gfx_cellgen++;
		for (size_t i = 0; i < nplacements; i++)
			placement_spans(&placements[i]);
	}

	fmt = XRenderFindVisualFormat(gfx_dpy, gfx_visual);
	if (!fmt)
		return;
	has_alpha = fmt->direct.alphaMask != 0;

	dst = XRenderCreatePicture(gfx_dpy, gfx_drawable, fmt, 0, NULL);
	if (!dst)
		return;

	for (int x = x1; x < x2; x++) {
		const Glyph *g = &line[x];
		GfxPlacement *p;
		GfxImage *im;
		uint32_t image_id;
		int row, col;
		int dw, dh, sx, sy, sw, sh, padx, pady;
		int srcx, srcy, dx, dy, w, h;

		if (!(g->mode & ATTR_IMAGE)) {
			prev = 0;
			cur_row = cur_col = cur_extra = -1;
			continue;
		}

		row = (int)GFX_IMG_ROW(g->img);
		col = (int)GFX_IMG_COL(g->img);
		if (row != GFX_IMG_COORD_UNSET)
			cur_row = row;
		else if (!prev && cur_row < 0)
			cur_row = 0;
		if (col != GFX_IMG_COORD_UNSET)
			cur_col = col;
		else if (prev && cur_col >= 0)
			cur_col++;
		else if (cur_col < 0)
			cur_col = 0;
		if (GFX_IMG_EXTRA(g->img) != GFX_IMG_COORD_UNSET)
			cur_extra = (int)GFX_IMG_EXTRA(g->img);
		else if (cur_extra < 0)
			cur_extra = 0;
		prev = 1;

		if (only_x >= 0 && x != only_x)
			continue;

		image_id = ((uint32_t)cur_extra << 24) |
		           (g->fg & 0xFFFFFF);
		p = find_virtual_placement(image_id);
		if (!p || !(im = find_image(p->image_id)))
			continue;
		if (!ensure_cache(p, fmt, has_alpha))
			continue;

		placement_dims(p, im, &dw, &dh, &sx, &sy, &sw, &sh,
		               &padx, &pady);

		srcx = cur_col * gfx_cw - padx;
		srcy = cur_row * gfx_ch - pady;
		dx = gfx_border + x * gfx_cw;
		dy = gfx_border + y * gfx_ch;
		w = gfx_cw;
		h = gfx_ch;

		if (srcx < 0) { dx -= srcx; w += srcx; srcx = 0; }
		if (srcy < 0) { dy -= srcy; h += srcy; srcy = 0; }
		if (srcx + w > dw) w = dw - srcx;
		if (srcy + h > dh) h = dh - srcy;
		if (dx < 0) { srcx -= dx; w += dx; dx = 0; }
		if (dy < 0) { srcy -= dy; h += dy; dy = 0; }
		if (dx + w > gfx_winw) w = gfx_winw - dx;
		if (dy + h > gfx_winh) h = gfx_winh - dy;
		if (w <= 0 || h <= 0)
			continue;

		XRenderComposite(gfx_dpy, has_alpha ? PictOpOver : PictOpSrc,
		                 p->picture, None, dst,
		                 srcx, srcy, 0, 0, dx, dy, w, h);
	}

	XRenderFreePicture(gfx_dpy, dst);
}
