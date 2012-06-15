/*
Copyright 2011 Canonical Ltd.

Authors:
    Conor Curran <conor.curran@canonical.com>
    Mirco Müller <mirco.mueller@canonical.com>
 
This program is free software: you can redistribute it and/or modify it 
under the terms of the GNU General Public License version 3, as published 
by the Free Software Foundation.

This program is distributed in the hope that it will be useful, but 
WITHOUT ANY WARRANTY; without even the implied warranties of 
MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR 
PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along 
with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <glib.h>
#include <math.h>
#include <libindicator/indicator-image-helper.h>
#include "user-widget.h"
#include "dbus-shared-names.h"


typedef struct _UserWidgetPrivate UserWidgetPrivate;

struct _UserWidgetPrivate
{
  DbusmenuMenuitem* twin_item;
  GtkWidget* user_image;
  gboolean using_personal_icon;
  GtkWidget* user_name;
  GtkWidget* container;
  GtkWidget* tick_icon;
  gboolean logged_in;
  gboolean sessions_active;
};

#define USER_WIDGET_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), USER_WIDGET_TYPE, UserWidgetPrivate))

typedef struct
{
  double r;
  double g;
  double b;
} CairoColorRGB;

/* Prototypes */
static void user_widget_class_init    (UserWidgetClass *klass);
static void user_widget_init          (UserWidget *self);
static void user_widget_dispose       (GObject *object);
static void user_widget_finalize      (GObject *object);

static void user_widget_set_twin_item (UserWidget* self,
                                       DbusmenuMenuitem* twin_item);

static void _color_shade (const CairoColorRGB *a,
                          float k,
                          CairoColorRGB *b);                                         

static void draw_album_border (GtkWidget *widget, gboolean selected);
                                         
static gboolean user_widget_primitive_draw_cb_gtk_3 (GtkWidget *image,
                                                         cairo_t* cr,
                                                         gpointer user_data);
static gboolean user_widget_draw_usericon_gtk_3 (GtkWidget *widget,
                                                 cairo_t* cr,
                                                 gpointer user_data);
                                                         
G_DEFINE_TYPE (UserWidget, user_widget, GTK_TYPE_MENU_ITEM);

static void
user_widget_class_init (UserWidgetClass *klass)
{
  GObjectClass * gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (UserWidgetPrivate));

  gobject_class->dispose = user_widget_dispose;
  gobject_class->finalize = user_widget_finalize;
}

static void
user_widget_init (UserWidget *self)
{
  self->priv = USER_WIDGET_GET_PRIVATE(self);

  UserWidgetPrivate * priv = self->priv;
  
  priv->user_image = NULL;
  priv->user_name  = NULL;
  priv->logged_in = FALSE;
  priv->sessions_active = FALSE;
  priv->container = NULL;
  priv->tick_icon = NULL;
  
  // Create the UI elements.  
  priv->user_image = gtk_image_new ();
  gtk_misc_set_alignment(GTK_MISC(priv->user_image), 0.0, 0.0);
  gtk_misc_set_padding (GTK_MISC(priv->user_image),0, 4.0);
  
  priv->user_name = gtk_label_new ("");

  priv->container = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  priv->tick_icon = gtk_image_new_from_icon_name ("account-logged-in",
                                                   GTK_ICON_SIZE_MENU);
  gtk_misc_set_alignment(GTK_MISC(priv->tick_icon), 1.0, 0.5);
  
  // Pack it together
  gtk_box_pack_start (GTK_BOX (priv->container),
                      priv->user_image,
                      FALSE,
                      FALSE,
                      0);   
  gtk_box_pack_start (GTK_BOX (priv->container),
                      priv->user_name,
                      FALSE,
                      FALSE,
                      3);                       
  gtk_box_pack_end (GTK_BOX(priv->container),
                    priv->tick_icon,
                    FALSE,
                    FALSE, 5);
  
  gtk_widget_show_all (priv->container);  
  gtk_container_add (GTK_CONTAINER (self), priv->container);    
  gtk_widget_show_all (priv->tick_icon);
  gtk_widget_set_no_show_all (priv->tick_icon, TRUE);
  gtk_widget_hide (priv->tick_icon);
  
  
  // Fetch the drawing context.
  g_signal_connect_after (GTK_WIDGET(self), "draw", 
                          G_CALLBACK(user_widget_primitive_draw_cb_gtk_3),
                          GTK_WIDGET(self));

  g_signal_connect_after (GTK_WIDGET(priv->user_image), "draw", 
                          G_CALLBACK(user_widget_draw_usericon_gtk_3),
                          GTK_WIDGET(self));
}

static void
user_widget_dispose (GObject *object)
{
  G_OBJECT_CLASS (user_widget_parent_class)->dispose (object);
}

// TODO tidy up image and name
static void
user_widget_finalize (GObject *object)
{
  G_OBJECT_CLASS (user_widget_parent_class)->finalize (object);
}


/*****************************************************************/

// TODO handle drawing of green check mark
static gboolean
user_widget_primitive_draw_cb_gtk_3 (GtkWidget *widget,
                                     cairo_t* cr,
                                     gpointer user_data)
{
  g_return_val_if_fail(IS_USER_WIDGET(user_data), FALSE);
  UserWidget* meta = USER_WIDGET(user_data);
  UserWidgetPrivate * priv = USER_WIDGET_GET_PRIVATE(meta);  

  // Draw dot only when user is the current user.
  if (!dbusmenu_menuitem_property_get_bool (priv->twin_item,
                                            USER_ITEM_PROP_IS_CURRENT_USER)){
    return FALSE;                                           
  }
  
  GtkStyle *style;
  gdouble x, y;  
  style = gtk_widget_get_style (widget);
  
  GtkAllocation allocation;
  gtk_widget_get_allocation (widget, &allocation);
  x = allocation.x + 13;        
  y = allocation.height / 2;
  
  cairo_arc (cr, x, y, 3.0, 0.0, 2 * G_PI);;
  
  cairo_set_source_rgb (cr, style->fg[gtk_widget_get_state(widget)].red/65535.0,
                            style->fg[gtk_widget_get_state(widget)].green/65535.0,
                            style->fg[gtk_widget_get_state(widget)].blue/65535.0);
  cairo_fill (cr);                                             

  return FALSE;  
}

static gboolean
user_widget_draw_usericon_gtk_3 (GtkWidget *widget,
                                 cairo_t* cr,
                                 gpointer user_data)
{
  g_return_val_if_fail(IS_USER_WIDGET(user_data), FALSE);
  UserWidget* meta = USER_WIDGET(user_data);
  UserWidgetPrivate * priv = USER_WIDGET_GET_PRIVATE(meta);  

  if (priv->using_personal_icon)
    {
      draw_album_border (widget, FALSE);  
    }
  
  return FALSE;
}

static void
draw_album_border(GtkWidget *widg, gboolean selected)
{
  cairo_t *cr;  
  cr = gdk_cairo_create (gtk_widget_get_window (widg));
  gtk_style_context_add_class (gtk_widget_get_style_context (widg),
                               "menu");
  
  GtkStyle *style;
  style = gtk_widget_get_style (widg);
  
  GtkAllocation alloc;
  gtk_widget_get_allocation (widg, &alloc);
  gint offset = 0;
  gint v_offset = 4;
  
  alloc.width = alloc.width + (offset * 2);
  alloc.height = alloc.height - v_offset - 2;
  alloc.x = alloc.x - offset;
  alloc.y = alloc.y + v_offset/2 +1;

  CairoColorRGB bg_normal, fg_normal;

  bg_normal.r = style->bg[0].red/65535.0;
  bg_normal.g = style->bg[0].green/65535.0;
  bg_normal.b = style->bg[0].blue/65535.0;

  gint state = selected ? 5 : 0;
  
  fg_normal.r = style->fg[state].red/65535.0;
  fg_normal.g = style->fg[state].green/65535.0;
  fg_normal.b = style->fg[state].blue/65535.0;

  CairoColorRGB dark_top_color;
  CairoColorRGB light_bottom_color;
  CairoColorRGB background_color;
  
  _color_shade ( &bg_normal, 0.93, &background_color );
  _color_shade ( &bg_normal, 0.23, &dark_top_color );
  _color_shade ( &fg_normal, 0.55, &light_bottom_color );
  
  cairo_rectangle (cr,
                   alloc.x, alloc.y,
                   alloc.width, alloc.height);

  cairo_set_line_width (cr, 1.0);
  
  cairo_clip ( cr );

  cairo_move_to (cr, alloc.x, alloc.y );
  cairo_line_to (cr, alloc.x + alloc.width,
                alloc.y );
  cairo_line_to ( cr, alloc.x + alloc.width,
                  alloc.y + alloc.height );
  cairo_line_to ( cr, alloc.x, alloc.y + alloc.height );
  cairo_line_to ( cr, alloc.x, alloc.y);
  cairo_close_path (cr);

  cairo_set_source_rgba ( cr,
                          background_color.r,
                          background_color.g,
                          background_color.b,
                          1.0 );
  
  cairo_stroke ( cr );
  
  cairo_move_to (cr, alloc.x, alloc.y );
  cairo_line_to (cr, alloc.x + alloc.width,
                alloc.y );

  cairo_close_path (cr);
  cairo_set_source_rgba ( cr,
                          dark_top_color.r,
                          dark_top_color.g,
                          dark_top_color.b,
                          1.0 );
  
  cairo_stroke ( cr );
  
  cairo_move_to ( cr, alloc.x + alloc.width,
                  alloc.y + alloc.height );
  cairo_line_to ( cr, alloc.x, alloc.y + alloc.height );

  cairo_close_path (cr);
  cairo_set_source_rgba ( cr,
                         light_bottom_color.r,
                         light_bottom_color.g,
                         light_bottom_color.b,
                         1.0);
  
  cairo_stroke ( cr );
  cairo_destroy (cr);   
}

static void
_color_rgb_to_hls (gdouble *r,
                   gdouble *g,
                   gdouble *b)
{
  gdouble min;
  gdouble max;
  gdouble red;
  gdouble green;
  gdouble blue;
  gdouble h = 0;
  gdouble l;
  gdouble s;
  gdouble delta;

  red = *r;
  green = *g;
  blue = *b;

  if (red > green)
  {
    if (red > blue)
      max = red;
    else
      max = blue;

    if (green < blue)
      min = green;
    else
    min = blue;
  }
  else
  {
    if (green > blue)
      max = green;
    else
    max = blue;

    if (red < blue)
      min = red;
    else
      min = blue;
  }
  l = (max+min)/2;
  if (fabs (max-min) < 0.0001)
  {
    h = 0;
    s = 0;
  }
  else
  {
    if (l <= 0.5)
    s = (max-min)/(max+min);
    else
    s = (max-min)/(2-max-min);

    delta = (max -min) != 0 ? (max -min) : 1;
    
    if(delta == 0)
      delta = 1;
    if (red == max)
      h = (green-blue)/delta;
    else if (green == max)
      h = 2+(blue-red)/delta;
    else if (blue == max)
      h = 4+(red-green)/delta;

    h *= 60;
    if (h < 0.0)
      h += 360;
  }

  *r = h;
  *g = l;
  *b = s;
}

static void
_color_hls_to_rgb (gdouble *h,
                   gdouble *l, 
                   gdouble *s)
{
  gdouble hue;
  gdouble lightness;
  gdouble saturation;
  gdouble m1, m2;
  gdouble r, g, b;

  lightness = *l;
  saturation = *s;

  if (lightness <= 0.5)
    m2 = lightness*(1+saturation);
  else
    m2 = lightness+saturation-lightness*saturation;

  m1 = 2*lightness-m2;

  if (saturation == 0)
  {
    *h = lightness;
    *l = lightness;
    *s = lightness;
  }
  else
  {
    hue = *h+120;
    while (hue > 360)
      hue -= 360;
    while (hue < 0)
      hue += 360;

    if (hue < 60)
      r = m1+(m2-m1)*hue/60;
    else if (hue < 180)
      r = m2;
    else if (hue < 240)
      r = m1+(m2-m1)*(240-hue)/60;
    else
      r = m1;

    hue = *h;
    while (hue > 360)
      hue -= 360;
    while (hue < 0)
      hue += 360;

    if (hue < 60)
      g = m1+(m2-m1)*hue/60;
    else if (hue < 180)
      g = m2;
    else if (hue < 240)
      g = m1+(m2-m1)*(240-hue)/60;
    else
      g = m1;

    hue = *h-120;
    while (hue > 360)
      hue -= 360;
    while (hue < 0)
      hue += 360;

    if (hue < 60)
      b = m1+(m2-m1)*hue/60;
    else if (hue < 180)
      b = m2;
    else if (hue < 240)
      b = m1+(m2-m1)*(240-hue)/60;
    else
      b = m1;

    *h = r;
    *l = g;
    *s = b;
  }
}

static void
_color_shade (const CairoColorRGB *a, float k, CairoColorRGB *b)
{
  double red;
  double green;
  double blue;

  red   = a->r;
  green = a->g;
  blue  = a->b;

  if (k == 1.0)
  {
    b->r = red;
    b->g = green;
    b->b = blue;
    return;
  }

  _color_rgb_to_hls (&red, &green, &blue);

  green *= k;
  if (green > 1.0)
    green = 1.0;
  else if (green < 0.0)
    green = 0.0;

  blue *= k;
  if (blue > 1.0)
    blue = 1.0;
  else if (blue < 0.0)
    blue = 0.0;

  _color_hls_to_rgb (&red, &green, &blue);

  b->r = red;
  b->g = green;
  b->b = blue;
}

/***
****
***/

static void
update_icon (UserWidget * self, DbusmenuMenuitem * mi)
{
  GdkPixbuf * pixbuf = NULL;

  /* first, try the menuitem's icon property */
  const gchar * icon_name = dbusmenu_menuitem_property_get (mi, USER_ITEM_PROP_ICON);
  if (icon_name)
    {
      GError* error = NULL;
      pixbuf = gdk_pixbuf_new_from_file_at_size (icon_name, 32, 32, &error);
      if (error != NULL)
        {
          g_warning ("Couldn't load the image \"%s\": %s", icon_name, error->message);
          g_clear_error (&error);
        }
    }

  /* as a fallback, try to use the default user icon */
  if (pixbuf != NULL)
    {
      self->priv->using_personal_icon = TRUE;
    }
  else
    {
      self->priv->using_personal_icon = FALSE;

      pixbuf = gtk_icon_theme_load_icon (gtk_icon_theme_get_default (),
                                         USER_ITEM_ICON_DEFAULT,
                                         32,
                                         GTK_ICON_LOOKUP_FORCE_SIZE,
                                         NULL);
    }

  if (pixbuf != NULL)
    {
      gtk_image_set_from_pixbuf (GTK_IMAGE(self->priv->user_image), pixbuf);
      g_object_unref (pixbuf);
    }
}

static void
update_logged_in (UserWidget * self, DbusmenuMenuitem * mi)
{
  const gboolean b = dbusmenu_menuitem_property_get_bool (mi, USER_ITEM_PROP_LOGGED_IN);

  g_debug ("User \"%s\" %s active sessions",
           dbusmenu_menuitem_property_get (mi, USER_ITEM_PROP_NAME),
           b ? "has" : "doesn't have");

  gtk_widget_set_visible (self->priv->tick_icon, b);
}

static void
update_name (UserWidget * self, DbusmenuMenuitem * mi)
{
  gtk_label_set_label (GTK_LABEL(self->priv->user_name),
                       dbusmenu_menuitem_property_get (mi, USER_ITEM_PROP_NAME));
}

static void 
user_widget_property_update (DbusmenuMenuitem  * mi,
                             const gchar       * property, 
                             GVariant          * value,
                             UserWidget        * self)
{
  g_return_if_fail (IS_USER_WIDGET (self)); 

  if (!g_strcmp0 (property, USER_ITEM_PROP_LOGGED_IN))
    {
      update_logged_in (self, mi);
    }
  else if (!g_strcmp0 (property, USER_ITEM_PROP_ICON))
    {
      update_icon (self, mi);
    }
  else if (!g_strcmp0 (property, USER_ITEM_PROP_NAME))
    {
      update_name (self, mi);
    }
  else
    {
      g_debug ("%s FIXME: unhandled property change %s", G_STRFUNC, property);
    }
}

static void
user_widget_set_twin_item (UserWidget * self, DbusmenuMenuitem * mi)
{
  self->priv->twin_item = mi;

  update_icon      (self, mi);
  update_name      (self, mi);
  update_logged_in (self, mi);

  g_signal_connect (G_OBJECT(mi), "property-changed", 
                    G_CALLBACK(user_widget_property_update), self);
}

 /**
 * transport_new:
 * @returns: a new #UserWidget.
 **/
GtkWidget* 
user_widget_new(DbusmenuMenuitem *item)
{
  GtkWidget* widget =  g_object_new(USER_WIDGET_TYPE, NULL);
  user_widget_set_twin_item ( USER_WIDGET(widget), item );
  return widget;                  
}
