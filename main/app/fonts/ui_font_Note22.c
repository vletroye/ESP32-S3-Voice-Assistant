/*******************************************************************************
 * Size: 22 px
 * Bpp: 4
 * Source: DejaVu Sans (Bitstream Vera / public domain license), shipped with LVGL
 * The notes (U+266A, U+266B) shown instead of the z z of the sleeping robot while the radio plays
 * Opts: --bpp 4 --size 22 --no-compress --no-prefilter --format lvgl --lv-include lvgl.h --font managed_components/lvgl__lvgl/scripts/built_in_font/DejaVuSans.ttf -r 0x266A -r 0x266B -o main/app/fonts/ui_font_Note22.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef UI_FONT_NOTE22
#define UI_FONT_NOTE22 1
#endif

#if UI_FONT_NOTE22

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+266A "♪" */
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xd6, 0x0, 0x0, 0x0, 0x0, 0xd, 0xfa, 0x30,
    0x0, 0x0, 0x0, 0xdd, 0xff, 0xd3, 0x0, 0x0,
    0xd, 0x43, 0xaf, 0xd0, 0x0, 0x0, 0xd4, 0x0,
    0x3e, 0x0, 0x0, 0xd, 0x40, 0x3, 0x80, 0x0,
    0x0, 0xd4, 0x0, 0x40, 0x0, 0x0, 0xd, 0x40,
    0x0, 0x0, 0x0, 0x0, 0xd4, 0x0, 0x0, 0x0,
    0x0, 0xd, 0x40, 0x0, 0x0, 0x0, 0x0, 0xd4,
    0x0, 0x0, 0x0, 0x0, 0xd, 0x40, 0x0, 0x0,
    0x4, 0x41, 0xd4, 0x0, 0x0, 0x1e, 0xff, 0xff,
    0x30, 0x0, 0x5, 0xff, 0xff, 0xf2, 0x0, 0x0,
    0xb, 0xff, 0xe6, 0x0, 0x0, 0x0, 0x0, 0x10,
    0x0, 0x0, 0x0,

    /* U+266B "♫" */
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xca, 0x61, 0x0, 0x0, 0x0, 0x0, 0xdf, 0xff,
    0xc8, 0x30, 0x0, 0x0, 0xda, 0xff, 0xff, 0xfc,
    0x0, 0x0, 0xd0, 0x4, 0x9e, 0xfd, 0x0, 0x0,
    0xd0, 0x0, 0x0, 0x2d, 0x0, 0x0, 0xd0, 0x0,
    0x0, 0xd, 0x0, 0x0, 0xd0, 0x0, 0x0, 0xd,
    0x0, 0x0, 0xd0, 0x0, 0x0, 0xd, 0x0, 0x0,
    0xd0, 0x0, 0x0, 0xd, 0x0, 0x0, 0xd0, 0x0,
    0x0, 0xd, 0x0, 0x0, 0xd0, 0x0, 0x0, 0xd,
    0x0, 0x0, 0xd0, 0x0, 0x0, 0xd, 0x3, 0x51,
    0xd0, 0x0, 0x0, 0xd, 0xaf, 0xff, 0xe0, 0x0,
    0x0, 0xd, 0xef, 0xff, 0xd0, 0x0, 0x12, 0xd,
    0x7f, 0xfe, 0x30, 0xa, 0xff, 0xdd, 0x0, 0x0,
    0x0, 0x1f, 0xff, 0xfb, 0x0, 0x0, 0x0, 0xb,
    0xff, 0xe4, 0x0, 0x0, 0x0, 0x0, 0x23, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 225, .box_w = 11, .box_h = 18, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 99, .adv_w = 316, .box_w = 12, .box_h = 20, .ofs_x = 4, .ofs_y = -3}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 9834, .range_length = 2, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LV_VERSION_CHECK(8, 0, 0)
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LV_VERSION_CHECK(8, 0, 0)
    .cache = &cache
#endif
};


/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LV_VERSION_CHECK(8, 0, 0)
const lv_font_t ui_font_Note22 = {
#else
lv_font_t ui_font_Note22 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 20,          /*The maximum line height required by the font*/
    .base_line = 3,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc           /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
};



#endif /*#if UI_FONT_NOTE22*/

