/* See LICENSE file for copyright and license details. */
#ifndef KITTYGFX_H
#define KITTYGFX_H

#include <stddef.h>
#include <stdint.h>

/* Result of handling one APC command. */
typedef struct {
	int move;   /* cursor should be moved */
	int col;    /* target cell (0-based) */
	int row;
	int dirty;  /* visible placements changed */
} GfxResult;

/* Unicode placeholder cell encoding (kitty graphics protocol, U=1). */
#define GFX_PLACEHOLDER_CHAR 0x10EEEE
#define GFX_IMG_COORD_UNSET  0x1ff
#define GFX_IMG_PACK(row, col, extra) \
	((((uint32_t)(row) & 0x1ff) << 18) | \
	 (((uint32_t)(col) & 0x1ff) << 9) | \
	 ((uint32_t)(extra) & 0x1ff))
#define GFX_IMG_ROW(img)   (((img) >> 18) & 0x1ff)
#define GFX_IMG_COL(img)   (((img) >> 9) & 0x1ff)
#define GFX_IMG_EXTRA(img) ((img) & 0x1ff)

/* 1-based index of a row/column diacritic, or 0 if it is not one. */
uint16_t gfx_diacritic_num(uint32_t code);

void gfx_reset(void);
void gfx_set_screen(int screen);
void gfx_set_cell(int cw, int ch);
void gfx_scroll(int screen, int top, int bot, int n);
void gfx_clear_screen(int screen);

/*
 * Handle an APC payload (the text between "ESC _" and "ESC \").  `buf` starts
 * with the 'G' that introduces a kitty graphics command.
 * Returns 1 when the payload was a kitty graphics command.
 */
int gfx_apc(const char *buf, size_t len, int screen, int cx, int cy,
            int termcols, GfxResult *res);

/* Draw all placements; called from xfinishdraw(). */
void gfx_draw(void *dpy, unsigned long drawable, void *visual, int depth,
              int cw, int ch, int borderpx, int winw, int winh,
              unsigned short br, unsigned short bg, unsigned short bb);

/*
 * Draw the images referenced by Unicode placeholder cells on one terminal
 * line. `line` is a Glyph array. Called from the line redraw path so the
 * images follow scrolling and are erased with the cells that hold them.
 * When `only_x` is non-negative only that column is composited, which is used
 * to restore one cell after the cursor was drawn over it.
 */
void gfx_draw_line_placeholders(void *dpy, unsigned long drawable, void *visual,
                                int depth, int cw, int ch, int borderpx,
                                int winw, int winh,
                                unsigned short br, unsigned short bg,
                                unsigned short bb, const void *line,
                                int y, int x1, int x2, int only_x);

#endif
