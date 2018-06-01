/*
 * vim:ts=4:sw=4:expandtab
 *
 * © 2015 Ingo Bürk and contributors (see also: LICENSE)
 *
 * draw.c: Utility for drawing.
 *
 */
#include "libi3.h"

#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <ctype.h>

/* The default visual_type to use if none is specified when creating the surface. Must be defined globally. */
extern xcb_visualtype_t *visual_type;

/* Forward declarations */
static void draw_util_set_source_color(surface_t *surface, color_t color);

#define RETURN_UNLESS_SURFACE_INITIALIZED(surface)                               \
    do {                                                                         \
        if ((surface)->id == XCB_NONE) {                                         \
            ELOG("Surface %p is not initialized, skipping drawing.\n", surface); \
            return;                                                              \
        }                                                                        \
    } while (0)

/*
 * Initialize the surface to represent the given drawable.
 *
 */
void draw_util_surface_init(xcb_connection_t *conn, surface_t *surface, xcb_drawable_t drawable,
                            xcb_visualtype_t *visual, int width, int height) {
    surface->id = drawable;
    surface->visual_type = ((visual == NULL) ? visual_type : visual);
    surface->width = width;
    surface->height = height;

    surface->gc = xcb_generate_id(conn);
    xcb_void_cookie_t gc_cookie = xcb_create_gc_checked(conn, surface->gc, surface->id, 0, NULL);

    xcb_generic_error_t *error = xcb_request_check(conn, gc_cookie);
    if (error != NULL) {
        ELOG("Could not create graphical context. Error code: %d. Please report this bug.\n", error->error_code);
    }

    surface->surface = cairo_xcb_surface_create(conn, surface->id, surface->visual_type, width, height);
    surface->cr = cairo_create(surface->surface);
}

/*
 * Destroys the surface.
 *
 */
void draw_util_surface_free(xcb_connection_t *conn, surface_t *surface) {
    xcb_free_gc(conn, surface->gc);
    cairo_surface_destroy(surface->surface);
    cairo_destroy(surface->cr);

    /* We need to explicitly set these to NULL to avoid assertion errors in
     * cairo when calling this multiple times. This can happen, for example,
     * when setting the border of a window to none and then closing it. */
    surface->surface = NULL;
    surface->cr = NULL;
}

/*
 * Resize the surface to the given size.
 *
 */
void draw_util_surface_set_size(surface_t *surface, int width, int height) {
    surface->width = width;
    surface->height = height;
    cairo_xcb_surface_set_size(surface->surface, width, height);
}

/* Helper to parse hex color when `*pcolor` is known to be valid */
static color_t _draw_util_parse_valid_hex_color(const char **pcolor) {
    const char *color = *pcolor;
    char alpha[2];
    if (strlen(color) >= strlen("#rrggbbaa") && isxdigit(color[7])) {
        alpha[0] = color[7];
        alpha[1] = color[8];
        *pcolor += strlen("#rrggbbaa");
    } else {
        alpha[0] = alpha[1] = 'F';
        *pcolor += strlen("#rrggbb");
    }

    char groups[4][3] = {
        {color[1], color[2], '\0'},
        {color[3], color[4], '\0'},
        {color[5], color[6], '\0'},
        {alpha[0], alpha[1], '\0'}};

    return (color_t){
        .red = strtol(groups[0], NULL, 16) / 255.0,
        .green = strtol(groups[1], NULL, 16) / 255.0,
        .blue = strtol(groups[2], NULL, 16) / 255.0,
        .alpha = strtol(groups[3], NULL, 16) / 255.0,
        .colorpixel = get_colorpixel(color)};
}

/*
 * Parses the given color in hex format to an internal color representation.
 * Note that the input must begin with a hash sign, e.g., "#3fbc59".
 *
 */
color_t draw_util_hex_to_color(const char *color) {
    if (strlen(color) < 6 || color[0] != '#') {
        ELOG("Could not parse color: %s\n", color);
        return draw_util_hex_to_color("#A9A9A9");
    }
    return _draw_util_parse_valid_hex_color(&color);
}

/*
 * Like `draw_util_hex_to_color` but meant to be re-entrant to parse multiple
 * space separated colors.
 *
 * Outputs the color in `outcolor`, advances `pcolor`, and returns false with no
 * changes if it wasn't a color (e.g. found invalid character, or empty string)
 *
 * Additionally, it skips leading white-spaces, and parses "-" as valid and leaves
 * `outcolor` untouched.
 *
 */
bool draw_util_prase_next_hex_to_color(const char **pcolor, color_t *outcolor) {
    const char *color = *pcolor;
    if (color == NULL)
        return false;
    while (isspace(color[0])) {
        ++color;
    }
    /* Skip */
    if (color[0] == '-') {
        ++color;
        *pcolor = color;
        return true;
    }
    if (strlen(color) < 6 || color[0] != '#') {
        ELOG("Could not parse next color: %s\n", color);
        return false;
    }
    *pcolor = color;
    *outcolor = _draw_util_parse_valid_hex_color(pcolor);
    return true;
}

/*
 * Set the given color as the source color on the surface.
 *
 */
static void draw_util_set_source_color(surface_t *surface, color_t color) {
    RETURN_UNLESS_SURFACE_INITIALIZED(surface);

    cairo_set_source_rgba(surface->cr, color.red, color.green, color.blue, color.alpha);
}

/*
 * Draw the given text using libi3.
 * This function also marks the surface dirty which is needed if other means of
 * drawing are used. This will be the case when using XCB to draw text.
 *
 */
void draw_util_text(i3String *text, surface_t *surface, color_t fg_color, color_t bg_color, int x, int y, int max_width) {
    RETURN_UNLESS_SURFACE_INITIALIZED(surface);

    /* Flush any changes before we draw the text as this might use XCB directly. */
    CAIRO_SURFACE_FLUSH(surface->surface);

    set_font_colors(surface->gc, fg_color, bg_color);
    draw_text(text, surface->id, surface->gc, surface->visual_type, x, y, max_width);

    /* Notify cairo that we (possibly) used another way to draw on the surface. */
    cairo_surface_mark_dirty(surface->surface);
}

/*
 * Draws a filled rectangle.
 * This function is a convenience wrapper and takes care of flushing the
 * surface as well as restoring the cairo state.
 *
 */
void draw_util_rectangle(surface_t *surface, color_t color, double x, double y, double w, double h) {
    RETURN_UNLESS_SURFACE_INITIALIZED(surface);

    cairo_save(surface->cr);

    /* Using the SOURCE operator will copy both color and alpha information directly
     * onto the surface rather than blending it. This is a bit more efficient and
     * allows better color control for the user when using opacity. */
    cairo_set_operator(surface->cr, CAIRO_OPERATOR_SOURCE);
    draw_util_set_source_color(surface, color);

    cairo_rectangle(surface->cr, x, y, w, h);
    cairo_fill(surface->cr);

    /* Make sure we flush the surface for any text drawing operations that could follow.
     * Since we support drawing text via XCB, we need this. */
    CAIRO_SURFACE_FLUSH(surface->surface);

    cairo_restore(surface->cr);
}

/*
 * Clears a surface with the given color.
 *
 */
void draw_util_clear_surface(surface_t *surface, color_t color) {
    RETURN_UNLESS_SURFACE_INITIALIZED(surface);

    cairo_save(surface->cr);

    /* Using the SOURCE operator will copy both color and alpha information directly
     * onto the surface rather than blending it. This is a bit more efficient and
     * allows better color control for the user when using opacity. */
    cairo_set_operator(surface->cr, CAIRO_OPERATOR_SOURCE);
    draw_util_set_source_color(surface, color);

    cairo_paint(surface->cr);

    /* Make sure we flush the surface for any text drawing operations that could follow.
     * Since we support drawing text via XCB, we need this. */
    CAIRO_SURFACE_FLUSH(surface->surface);

    cairo_restore(surface->cr);
}

/*
 * Copies a surface onto another surface.
 *
 */
void draw_util_copy_surface(surface_t *src, surface_t *dest, double src_x, double src_y,
                            double dest_x, double dest_y, double width, double height) {
    RETURN_UNLESS_SURFACE_INITIALIZED(src);
    RETURN_UNLESS_SURFACE_INITIALIZED(dest);

    cairo_save(dest->cr);

    /* Using the SOURCE operator will copy both color and alpha information directly
     * onto the surface rather than blending it. This is a bit more efficient and
     * allows better color control for the user when using opacity. */
    cairo_set_operator(dest->cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(dest->cr, src->surface, dest_x - src_x, dest_y - src_y);

    cairo_rectangle(dest->cr, dest_x, dest_y, width, height);
    cairo_fill(dest->cr);

    /* Make sure we flush the surface for any text drawing operations that could follow.
     * Since we support drawing text via XCB, we need this. */
    CAIRO_SURFACE_FLUSH(src->surface);
    CAIRO_SURFACE_FLUSH(dest->surface);

    cairo_restore(dest->cr);
}
