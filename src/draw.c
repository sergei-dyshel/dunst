#include "draw.h"

#include <X11/Xutil.h> // Temporary
#include <assert.h>
#include <cairo.h>
#include <cairo-xlib.h> // Temporary
#include <math.h>
#include <pango/pango-attributes.h>
#include <pango/pango-font.h>
#include <pango/pango-layout.h>
#include <pango/pango-types.h>
#include <pango/pangocairo.h>
#include <stdlib.h>

#include "dunst.h"
#include "icon.h"
#include "markup.h"
#include "notification.h"
#include "log.h"
#include "queues.h"
#include "x11/x.h"

typedef struct {
        PangoLayout *l;
        color_t fg;
        color_t bg;
        color_t frame;
        char *text;
        PangoAttrList *attr;
        cairo_surface_t *icon;
        notification *n;
} colored_layout;

cairo_surface_t *root_surface;
cairo_t *c_context;

PangoFontDescription *pango_fdesc;

void draw_setup(void)
{
        x_setup();

        root_surface = x_create_cairo_surface();
        c_context = cairo_create(root_surface);
        pango_fdesc = pango_font_description_from_string(settings.font);
}

static color_t color_hex_to_double(int hexValue)
{
        color_t color;
        color.r = ((hexValue >> 16) & 0xFF) / 255.0;
        color.g = ((hexValue >> 8) & 0xFF) / 255.0;
        color.b = ((hexValue) & 0xFF) / 255.0;

        return color;
}

static color_t string_to_color(const char *str)
{
        char *end;
        long int val = strtol(str+1, &end, 16);
        if (*end != '\0' && *(end+1) != '\0') {
                LOG_W("Invalid color string: '%s'", str);
        }

        return color_hex_to_double(val);
}

static double apply_delta(double base, double delta)
{
        base += delta;
        if (base > 1)
                base = 1;
        if (base < 0)
                base = 0;

        return base;
}

static color_t calculate_foreground_color(color_t bg)
{
        double c_delta = 0.1;
        color_t color = bg;

        /* do we need to darken or brighten the colors? */
        bool darken = (bg.r + bg.g + bg.b) / 3 > 0.5;

        int signedness = darken ? -1 : 1;

        color.r = apply_delta(color.r, c_delta * signedness);
        color.g = apply_delta(color.g, c_delta * signedness);
        color.b = apply_delta(color.b, c_delta * signedness);

        return color;
}

static color_t get_separator_color(colored_layout *cl, colored_layout *cl_next)
{
        switch (settings.sep_color) {
        case FRAME:
                if (cl_next->n->urgency > cl->n->urgency)
                        return cl_next->frame;
                else
                        return cl->frame;
        case CUSTOM:
                return string_to_color(settings.sep_custom_color_str);
        case FOREGROUND:
                return cl->fg;
        case AUTO:
                return calculate_foreground_color(cl->bg);
        default:
                LOG_E("Unknown separator color type.");
        }
}

static void setup_pango_layout(PangoLayout *layout, int width)
{
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_width(layout, width * PANGO_SCALE);
        pango_layout_set_font_description(layout, pango_fdesc);
        pango_layout_set_spacing(layout, settings.line_height * PANGO_SCALE);

        PangoAlignment align;
        switch (settings.align) {
        case left:
        default:
                align = PANGO_ALIGN_LEFT;
                break;
        case center:
                align = PANGO_ALIGN_CENTER;
                break;
        case right:
                align = PANGO_ALIGN_RIGHT;
                break;
        }
        pango_layout_set_alignment(layout, align);

}

static void free_colored_layout(void *data)
{
        colored_layout *cl = data;
        g_object_unref(cl->l);
        pango_attr_list_unref(cl->attr);
        g_free(cl->text);
        if (cl->icon) cairo_surface_destroy(cl->icon);
        g_free(cl);
}

static bool have_dynamic_width(void)
{
        return (settings.geometry.width_set && settings.geometry.w == 0);
}

static struct dimensions calculate_dimensions(GSList *layouts)
{
        struct dimensions dim = { 0 };

        screen_info *scr = get_active_screen();
        if (have_dynamic_width()) {
                /* dynamic width */
                dim.w = 0;
        } else if (settings.geometry.width_set) {
                /* fixed width */
                if (settings.geometry.negative_width) {
                        dim.w = scr->w - settings.geometry.w;
                } else {
                        dim.w = settings.geometry.w;
                }
        } else {
                /* across the screen */
                dim.w = scr->w;
        }

        dim.h += 2 * settings.frame_width;
        dim.h += (g_slist_length(layouts) - 1) * settings.separator_height;

        int text_width = 0, total_width = 0;
        for (GSList *iter = layouts; iter; iter = iter->next) {
                colored_layout *cl = iter->data;
                int w=0,h=0;
                pango_layout_get_pixel_size(cl->l, &w, &h);
                if (cl->icon) {
                        h = MAX(cairo_image_surface_get_height(cl->icon), h);
                        w += cairo_image_surface_get_width(cl->icon) + settings.h_padding;
                }
                h = MAX(settings.notification_height, h + settings.padding * 2);
                dim.h += h;
                text_width = MAX(w, text_width);

                if (have_dynamic_width() || settings.shrink) {
                        /* dynamic width */
                        total_width = MAX(text_width + 2 * settings.h_padding, total_width);

                        /* subtract height from the unwrapped text */
                        dim.h -= h;

                        if (total_width > scr->w) {
                                /* set width to screen width */
                                dim.w = scr->w - settings.geometry.x * 2;
                        } else if (have_dynamic_width() || (total_width < settings.geometry.w && settings.shrink)) {
                                /* set width to text width */
                                dim.w = total_width + 2 * settings.frame_width;
                        }

                        /* re-setup the layout */
                        w = dim.w;
                        w -= 2 * settings.h_padding;
                        w -= 2 * settings.frame_width;
                        if (cl->icon) w -= cairo_image_surface_get_width(cl->icon) + settings.h_padding;
                        setup_pango_layout(cl->l, w);

                        /* re-read information */
                        pango_layout_get_pixel_size(cl->l, &w, &h);
                        if (cl->icon) {
                                h = MAX(cairo_image_surface_get_height(cl->icon), h);
                                w += cairo_image_surface_get_width(cl->icon) + settings.h_padding;
                        }
                        h = MAX(settings.notification_height, h + settings.padding * 2);
                        dim.h += h;
                        text_width = MAX(w, text_width);
                }
        }

        if (dim.w <= 0) {
                dim.w = text_width + 2 * settings.h_padding;
                dim.w += 2 * settings.frame_width;
        }

        return dim;
}

static PangoLayout *create_layout(cairo_t *c)
{
        screen_info *screen = get_active_screen();

        PangoContext *context = pango_cairo_create_context(c);
        pango_cairo_context_set_resolution(context, get_dpi_for_screen(screen));

        PangoLayout *layout = pango_layout_new(context);

        g_object_unref(context);

        return layout;
}

static colored_layout *init_shared(cairo_t *c, notification *n)
{
        colored_layout *cl = g_malloc(sizeof(colored_layout));
        cl->l = create_layout(c);

        if (!settings.word_wrap) {
                PangoEllipsizeMode ellipsize;
                switch (settings.ellipsize) {
                case start:
                        ellipsize = PANGO_ELLIPSIZE_START;
                        break;
                case middle:
                        ellipsize = PANGO_ELLIPSIZE_MIDDLE;
                        break;
                case end:
                        ellipsize = PANGO_ELLIPSIZE_END;
                        break;
                default:
                        assert(false);
                }
                pango_layout_set_ellipsize(cl->l, ellipsize);
        }

        if (settings.icon_position != icons_off) {
                cl->icon = icon_get_for_notification(n);
        } else {
                cl->icon = NULL;
        }

        if (cl->icon && cairo_surface_status(cl->icon) != CAIRO_STATUS_SUCCESS) {
                cairo_surface_destroy(cl->icon);
                cl->icon = NULL;
        }

        cl->fg = string_to_color(n->colors[ColFG]);
        cl->bg = string_to_color(n->colors[ColBG]);
        cl->frame = string_to_color(n->colors[ColFrame]);

        cl->n = n;

        struct dimensions dim = calculate_dimensions(NULL);
        int width = dim.w;

        if (have_dynamic_width()) {
                setup_pango_layout(cl->l, -1);
        } else {
                width -= 2 * settings.h_padding;
                width -= 2 * settings.frame_width;
                if (cl->icon) width -= cairo_image_surface_get_width(cl->icon) + settings.h_padding;
                setup_pango_layout(cl->l, width);
        }

        return cl;
}

static colored_layout *create_layout_for_xmore(cairo_t *c, notification *n, int qlen)
{
        colored_layout *cl = init_shared(c, n);
        cl->text = g_strdup_printf("(%d more)", qlen);
        cl->attr = NULL;
        pango_layout_set_text(cl->l, cl->text, -1);
        return cl;
}

static colored_layout *create_layout_from_notification(cairo_t *c, notification *n)
{

        colored_layout *cl = init_shared(c, n);

        /* markup */
        GError *err = NULL;
        pango_parse_markup(n->text_to_render, -1, 0, &(cl->attr), &(cl->text), NULL, &err);

        if (!err) {
                pango_layout_set_text(cl->l, cl->text, -1);
                pango_layout_set_attributes(cl->l, cl->attr);
        } else {
                /* remove markup and display plain message instead */
                n->text_to_render = markup_strip(n->text_to_render);
                cl->text = NULL;
                cl->attr = NULL;
                pango_layout_set_text(cl->l, n->text_to_render, -1);
                if (n->first_render) {
                        LOG_W("Unable to parse markup: %s", err->message);
                }
                g_error_free(err);
        }


        pango_layout_get_pixel_size(cl->l, NULL, &(n->displayed_height));
        if (cl->icon) n->displayed_height = MAX(cairo_image_surface_get_height(cl->icon), n->displayed_height);
        n->displayed_height = MAX(settings.notification_height, n->displayed_height + settings.padding * 2);

        n->first_render = false;
        return cl;
}

static GSList *create_layouts(cairo_t *c)
{
        GSList *layouts = NULL;

        int qlen = queues_length_waiting();
        bool xmore_is_needed = qlen > 0 && settings.indicate_hidden;

        notification *last = NULL;
        for (const GList *iter = queues_get_displayed();
                        iter; iter = iter->next)
        {
                notification *n = iter->data;
                last = n;

                notification_update_text_to_render(n);

                if (!iter->next && xmore_is_needed && settings.geometry.h == 1) {
                        char *new_ttr = g_strdup_printf("%s (%d more)", n->text_to_render, qlen);
                        g_free(n->text_to_render);
                        n->text_to_render = new_ttr;
                }
                layouts = g_slist_append(layouts,
                                create_layout_from_notification(c, n));
        }

        if (xmore_is_needed && settings.geometry.h != 1) {
                /* append xmore message as new message */
                layouts = g_slist_append(layouts,
                        create_layout_for_xmore(c, last, qlen));
        }

        return layouts;
}

static void free_layouts(GSList *layouts)
{
        g_slist_free_full(layouts, free_colored_layout);
}

static struct dimensions render_layout(cairo_t *c, colored_layout *cl, colored_layout *cl_next, struct dimensions dim, bool first, bool last)
{
        int h;
        int h_text = 0;
        pango_layout_get_pixel_size(cl->l, NULL, &h);
        if (cl->icon) {
                h_text = h;
                h = MAX(cairo_image_surface_get_height(cl->icon), h);
        }

        int bg_x = 0;
        int bg_y = dim.y;
        int bg_width = dim.w;
        int bg_height = MAX(settings.notification_height, (2 * settings.padding) + h);
        double bg_half_height = settings.notification_height/2.0;
        int pango_offset = (int) floor(h/2.0);

        if (first) bg_height += settings.frame_width;
        if (last) bg_height += settings.frame_width;
        else bg_height += settings.separator_height;

        cairo_set_source_rgb(c, cl->frame.r, cl->frame.g, cl->frame.b);
        cairo_rectangle(c, bg_x, bg_y, bg_width, bg_height);
        cairo_fill(c);

        /* adding frame */
        bg_x += settings.frame_width;
        if (first) {
                dim.y += settings.frame_width;
                bg_y += settings.frame_width;
                bg_height -= settings.frame_width;
                if (!last) bg_height -= settings.separator_height;
        }
        bg_width -= 2 * settings.frame_width;
        if (last)
                bg_height -= settings.frame_width;

        cairo_set_source_rgb(c, cl->bg.r, cl->bg.g, cl->bg.b);
        cairo_rectangle(c, bg_x, bg_y, bg_width, bg_height);
        cairo_fill(c);

        bool use_padding = settings.notification_height <= (2 * settings.padding) + h;
        if (use_padding)
                dim.y += settings.padding;
        else
                dim.y += (int) (ceil(bg_half_height) - pango_offset);

        if (cl->icon && settings.icon_position == icons_left) {
                cairo_move_to(c, settings.frame_width + cairo_image_surface_get_width(cl->icon) + 2 * settings.h_padding, bg_y + settings.padding + h/2 - h_text/2);
        } else if (cl->icon && settings.icon_position == icons_right) {
                cairo_move_to(c, settings.frame_width + settings.h_padding, bg_y + settings.padding + h/2 - h_text/2);
        } else {
                cairo_move_to(c, settings.frame_width + settings.h_padding, bg_y + settings.padding);
        }

        cairo_set_source_rgb(c, cl->fg.r, cl->fg.g, cl->fg.b);
        pango_cairo_update_layout(c, cl->l);
        pango_cairo_show_layout(c, cl->l);
        if (use_padding)
                dim.y += h + settings.padding;
        else
                dim.y += (int)(floor(bg_half_height) + pango_offset);

        if (settings.separator_height > 0 && !last) {
                color_t sep_color = get_separator_color(cl, cl_next);
                cairo_set_source_rgb(c, sep_color.r, sep_color.g, sep_color.b);

                if (settings.sep_color == FRAME)
                        // Draw over the borders on both sides to avoid
                        // the wrong color in the corners.
                        cairo_rectangle(c, 0, dim.y, dim.w, settings.separator_height);
                else
                        cairo_rectangle(c, settings.frame_width, dim.y + settings.frame_width, dim.w - 2 * settings.frame_width, settings.separator_height);

                cairo_fill(c);
                dim.y += settings.separator_height;
        }
        cairo_move_to(c, settings.h_padding, dim.y);

        if (cl->icon) {
                unsigned int image_width = cairo_image_surface_get_width(cl->icon),
                             image_height = cairo_image_surface_get_height(cl->icon),
                             image_x,
                             image_y = bg_y + settings.padding + h/2 - image_height/2;

                if (settings.icon_position == icons_left) {
                        image_x = settings.frame_width + settings.h_padding;
                } else {
                        image_x = bg_width - settings.h_padding - image_width + settings.frame_width;
                }

                cairo_set_source_surface(c, cl->icon, image_x, image_y);
                cairo_rectangle(c, image_x, image_y, image_width, image_height);
                cairo_fill(c);
        }

        return dim;
}

/**
 * Calculates the position the window should be placed at given its width and
 * height and stores them in \p ret_x and \p ret_y.
 */
static void calc_window_pos(int width, int height, int *ret_x, int *ret_y)
{
        screen_info *scr = get_active_screen();

        if (settings.geometry.negative_x) {
                *ret_x = (scr->x + (scr->w - width)) + settings.geometry.x;
        } else {
                *ret_x = scr->x + settings.geometry.x;
        }

        if (settings.geometry.negative_y) {
                *ret_y = scr->y + (scr->h + settings.geometry.y) - height;
        } else {
                *ret_y = scr->y + settings.geometry.y;
        }
}

void draw(void)
{

        GSList *layouts = create_layouts(c_context);

        struct dimensions dim = calculate_dimensions(layouts);
        int width = dim.w;
        int height = dim.h;
        int win_x, win_y;

        cairo_t *c;
        cairo_surface_t *image_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        c = cairo_create(image_surface);

        calc_window_pos(dim.w, dim.h, &win_x, &win_y);
        x_win_move(win_x, win_y, width, height);
        cairo_xlib_surface_set_size(root_surface, width, height);

        cairo_move_to(c, 0, 0);

        bool first = true;
        for (GSList *iter = layouts; iter; iter = iter->next) {
                if (iter->next)
                        dim = render_layout(c, iter->data, iter->next->data, dim, first, iter->next == NULL);
                else
                        dim = render_layout(c, iter->data, NULL, dim, first, iter->next == NULL);

                first = false;
        }

        cairo_set_source_surface(c_context, image_surface, 0, 0);
        cairo_paint(c_context);
        cairo_show_page(c_context);

        XFlush(xctx.dpy);

        cairo_destroy(c);
        cairo_surface_destroy(image_surface);
        free_layouts(layouts);
}

void draw_free(void)
{
        cairo_destroy(c_context);
        cairo_surface_destroy(root_surface);

        x_free();
}
/* vim: set tabstop=8 shiftwidth=8 expandtab textwidth=0: */
