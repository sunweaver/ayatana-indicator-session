/*
 * Copyright 2013 Canonical Ltd.
 *
 * Authors:
 *   Charles Kerr <charles.kerr@canonical.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <locale.h>

#include <glib/gi18n.h>
#include <gio/gio.h>

#include "backend.h"
#include "service.h"

/* FIXME: remove -test */
#define BUS_NAME "com.canonical.indicator.session-test"
#define BUS_PATH "/com/canonical/indicator/session"

#define ICON_DEFAULT "system-devices-panel"
#define ICON_INFO    "system-devices-panel-information"
#define ICON_ALERT   "system-devices-panel-alert"

G_DEFINE_TYPE (IndicatorSessionService,
               indicator_session_service,
               G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_REPLACE,
  PROP_MAX_USERS,
  PROP_LAST
};

static GParamSpec * properties[PROP_LAST];

enum
{
  SECTION_HEADER    = (1<<0),
  SECTION_ADMIN     = (1<<1),
  SECTION_SETTINGS  = (1<<2),
  SECTION_SWITCH    = (1<<3),
  SECTION_SESSION   = (1<<4)
};

enum
{
  FORM_DESKTOP,
  FORM_GREETER,
  N_FORMS
};

static const char * const menu_names[N_FORMS] =
{
  "desktop",
  "desktop_greeter"
};

struct FormMenuInfo
{
  /* the root level -- the header is the only child of this */
  GMenu * menu;

  /* parent of the sections. This is the header's submenu */
  GMenu * submenu;

  guint export_id;
};

struct _IndicatorSessionServicePrivate
{
  guint own_id;
  guint max_users;
  IndicatorSessionUsers * backend_users;
  IndicatorSessionGuest * backend_guest;
  IndicatorSessionActions * backend_actions;
  GSettings * indicator_settings;
  GSettings * keybinding_settings;
  GSimpleActionGroup * actions;
  guint actions_export_id;
  struct FormMenuInfo menus[N_FORMS];
  GSimpleAction * header_action;
  GSimpleAction * user_switcher_action;
  GSimpleAction * guest_switcher_action;
  GHashTable * users;
  guint rebuild_id;
  int rebuild_flags;
  GDBusConnection * conn;

  gboolean replace;
};

typedef IndicatorSessionServicePrivate priv_t;

static const char * get_current_real_name (IndicatorSessionService * self);

/***
****
***/

static void rebuild_now (IndicatorSessionService * self, int section);
static void rebuild_soon (IndicatorSessionService * self, int section);

static inline void
rebuild_header_soon (IndicatorSessionService * self)
{
  rebuild_soon (self, SECTION_HEADER);
}
static inline void
rebuild_switch_section_soon (IndicatorSessionService * self)
{
  rebuild_soon (self, SECTION_SWITCH);
}
static inline void
rebuild_session_section_soon (IndicatorSessionService * self)
{
  rebuild_soon (self, SECTION_SESSION);
}
static inline void
rebuild_settings_section_soon (IndicatorSessionService * self)
{
  rebuild_soon (self, SECTION_SETTINGS);
}

/***
****
***/

static void
update_header_action (IndicatorSessionService * self)
{
  gchar * a11y;
  gboolean need_attn;
  gboolean show_name;
  GVariant * variant;
  const gchar * real_name;
  const gchar * label;
  const gchar * iconstr;
  const priv_t * const p = self->priv;

  g_return_if_fail (p->header_action != NULL);

  if (indicator_session_actions_has_online_account_error (p->backend_actions))
    {
      need_attn = TRUE;
      iconstr = ICON_ALERT;
    }
  else
    {
      need_attn = FALSE;
      iconstr = ICON_DEFAULT;
    }

  show_name = g_settings_get_boolean (p->indicator_settings,
                                      "show-real-name-on-panel");

  real_name = get_current_real_name (self);
  label = show_name && real_name ? real_name : "";

  if (*label && need_attn)
    {
      /* Translators: the name of the menu ("System"), then the user's name,
         then a hint that something in this menu requires user attention */
      a11y = g_strdup_printf (_("System, %s (Attention Required)"), real_name);
    }
  else if (*label)
    {
      /* Translators: the name of the menu ("System"), then the user's name */
      a11y = g_strdup_printf (_("System, %s"), label);
    }
  else if (need_attn)
    {
      a11y = g_strdup  (_("System (Attention Required)"));
    }
  else
    {
      a11y = g_strdup (_("System"));
    }

  variant = g_variant_new ("(sssb)", label, iconstr, a11y, TRUE);
  g_simple_action_set_state (p->header_action, variant);
  g_free (a11y);
}

/***
****  USERS
***/

static GMenuModel * create_switch_section (IndicatorSessionService * self);

static void
add_user (IndicatorSessionService * self, const gchar * key)
{
  IndicatorSessionUser * u;

  /* update our user table */
  u = indicator_session_users_get_user (self->priv->backend_users, key);
  g_hash_table_insert (self->priv->users, g_strdup(key), u);

  /* enqueue rebuilds for the affected sections */
  rebuild_switch_section_soon (self);
  if (u->is_current_user)
    rebuild_header_soon (self);
}

static void
on_user_added (IndicatorSessionUsers * backend_users G_GNUC_UNUSED,
               const char            * key,
               gpointer                gself)
{
  add_user (INDICATOR_SESSION_SERVICE(gself), key);
}

static void
on_user_changed (IndicatorSessionUsers * backend_users G_GNUC_UNUSED,
                 const char            * key,
                 gpointer                gself)
{
  add_user (INDICATOR_SESSION_SERVICE(gself), key);
}

static void
on_user_removed (IndicatorSessionUsers * backend_users G_GNUC_UNUSED,
                 const char            * key,
                 gpointer                gself)
{
  IndicatorSessionService * self = INDICATOR_SESSION_SERVICE (gself);
  g_return_if_fail (self != NULL);

  /* update our user table */
  g_hash_table_remove (self->priv->users, key);

  /* enqueue rebuilds for the affected sections */
  rebuild_switch_section_soon (self);
}

static const char *
get_current_real_name (IndicatorSessionService * self)
{
  GHashTableIter iter;
  gpointer key, value;

  /* is it the guest? */
  if (indicator_session_guest_is_active (self->priv->backend_guest))
    return _("Guest");

  /* is it a user? */
  g_hash_table_iter_init (&iter, self->priv->users);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      IndicatorSessionUser * user = value;
      if (user->is_current_user)
        return user->real_name;
    }

  return "";
}

/***
****
***/

static GMenuModel *
create_admin_section (void)
{
  GMenu * menu;
  GMenuItem * item;

  menu = g_menu_new ();

  item = g_menu_item_new (_("About This Computer"), "indicator.about");
  g_menu_append_item (menu, item);
  g_object_unref (G_OBJECT(item));

  item = g_menu_item_new (_("Ubuntu Help"), "indicator.help");
  g_menu_append_item (menu, item);
  g_object_unref (G_OBJECT(item));

  return G_MENU_MODEL (menu);
}

static GMenuModel *
create_settings_section (IndicatorSessionService * self)
{
  GMenu * menu;
  GMenuItem * item;
  priv_t * p = self->priv;

  menu = g_menu_new ();

  item = g_menu_item_new (_("System Settings\342\200\246"), "indicator.settings");
  g_menu_append_item (menu, item);
  g_object_unref (G_OBJECT(item));

  if (indicator_session_actions_has_online_account_error (p->backend_actions))
    {
      item = g_menu_item_new (_("Online Accounts\342\200\246"), "indicator.online-accounts");
      g_menu_append_item (menu, item);
      g_object_unref (G_OBJECT(item));
    }

  return G_MENU_MODEL (menu);
}

/**
 * The switch-to-guest action's state is a dictionary with these entries:
 *   - "is-active" (boolean)
 *   - "is-logged-in" (boolean)
 */
static GVariant *
create_guest_switcher_state (IndicatorSessionService * self)
{
  GVariant * val;
  GVariantBuilder * b;
  IndicatorSessionGuest * const g = self->priv->backend_guest;

  b = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
  val = g_variant_new_boolean (indicator_session_guest_is_active (g));
  g_variant_builder_add (b, "{sv}", "is-active", val);
  val = g_variant_new_boolean (indicator_session_guest_is_logged_in (g));
  g_variant_builder_add (b, "{sv}", "is-logged-in", val);
  return g_variant_builder_end (b);
}

/**
 * The switch-to-user action's state is a dictionary with these entries: 
 *  - "active-user" (username string)
 *  - "logged-in-users" (array of username strings)
 */
static GVariant *
create_user_switcher_state (IndicatorSessionService * self)
{
  GVariantBuilder * a;
  GVariantBuilder * b;
  GVariant * val;
  GHashTableIter ht_iter;
  gpointer ht_key, ht_value;
  const char * current_user;

  current_user = "";
  a = g_variant_builder_new (G_VARIANT_TYPE("as"));
  g_hash_table_iter_init (&ht_iter, self->priv->users);
  while (g_hash_table_iter_next (&ht_iter, &ht_key, &ht_value))
    {
      const IndicatorSessionUser * u = ht_value;

      if (u->is_current_user)
        current_user = u->user_name;

      if (u->is_logged_in)
        g_variant_builder_add (a, "s", u->user_name);
    }

  b = g_variant_builder_new (G_VARIANT_TYPE("a{sv}"));
  val = g_variant_new_string (current_user);
  g_variant_builder_add (b, "{sv}", "active-user", val);
  val = g_variant_builder_end (a);
  g_variant_builder_add (b, "{sv}", "logged-in-users", val);
  return g_variant_builder_end (b);
}

static void
update_switch_actions (IndicatorSessionService * self)
{
  g_simple_action_set_state (self->priv->guest_switcher_action,
                             create_guest_switcher_state (self));

  g_simple_action_set_state (self->priv->user_switcher_action,
                             create_user_switcher_state (self));
}

static gboolean
use_ellipsis (IndicatorSessionService * self)
{
  /* does the backend support confirmation prompts? */
  if (!indicator_session_actions_can_prompt (self->priv->backend_actions))
    return FALSE;

  /* has the user disabled prompts? */
  if (g_settings_get_boolean (self->priv->indicator_settings,
                              "suppress-logout-restart-shutdown"))
    return FALSE;

  return TRUE;
}

/* lower index == more useful.
   When there are too many users for the menu,
   we use this to decide which to cull. */
static int
compare_users_by_usefulness (gconstpointer ga, gconstpointer gb)
{
  const IndicatorSessionUser * a = *(const IndicatorSessionUser**)ga;
  const IndicatorSessionUser * b = *(const IndicatorSessionUser**)gb;

  if (a->is_current_user != b->is_current_user)
    return a->is_current_user ? -1 : 1;

  if (a->is_logged_in != b->is_logged_in)
    return a->is_logged_in ? -1 : 1;

  if (a->login_frequency != b->login_frequency)
    return a->login_frequency > b->login_frequency ? -1 : 1;

  return 0;
}

/* sorting them for display in the menu */
static int
compare_users_by_label (gconstpointer ga, gconstpointer gb)
{
  int i;
  const IndicatorSessionUser * a = *(const IndicatorSessionUser**)ga;
  const IndicatorSessionUser * b = *(const IndicatorSessionUser**)gb;

  if ((i = g_strcmp0 (a->real_name, b->real_name)))
    return i;

  return g_strcmp0 (a->user_name, b->user_name);
}

static GMenuModel *
create_switch_section (IndicatorSessionService * self)
{
  gchar * str;
  GMenu * menu;
  GMenuItem * item;
  guint i;
  gpointer guser;
  GHashTableIter iter;
  GPtrArray * users;
  const priv_t * const p = self->priv;
  const gboolean ellipsis = use_ellipsis (self);

  menu = g_menu_new ();

  /* lockswitch */
  if (indicator_session_users_is_live_session (p->backend_users))
    {
      const char * action = "indicator.switch-to-screensaver";
      item = g_menu_item_new (_("Start Screen Saver"), action);
    }
  else if (indicator_session_guest_is_active (p->backend_guest))
    {
      const char * action = "indicator.switch-to-greeter";
      item = g_menu_item_new (ellipsis ? _("Switch Account\342\200\246")
                                       : _("Switch Account"), action);
    }
  else if (g_hash_table_size (p->users) == 1)
    {
      const char * action = "indicator.switch-to-greeter";
      item = g_menu_item_new (ellipsis ? _("Lock\342\200\246")
                                       : _("Lock"), action);
    }
  else
    {
      const char * action = "indicator.switch-to-greeter";
      item = g_menu_item_new (ellipsis ? _("Lock/Switch Account\342\200\246")
                                       : _("Lock/Switch Account"), action);
    }
  str = g_settings_get_string (p->keybinding_settings, "screensaver");
  g_menu_item_set_attribute (item, "accel", "s", str);
  g_free (str);
  g_menu_append_item (menu, item);
  g_object_unref (G_OBJECT(item));
 
  if (indicator_session_guest_is_allowed (p->backend_guest))
    {
      item = g_menu_item_new (_("Guest Session"), "indicator.switch-to-guest");
      g_menu_append_item (menu, item);
      g_object_unref (G_OBJECT(item));
    }

  /* build an array of all the users we know of */
  users = g_ptr_array_new ();
  g_hash_table_iter_init (&iter, p->users);
  while (g_hash_table_iter_next (&iter, NULL, &guser))
    g_ptr_array_add (users, guser);

  /* if there are too many users, cull out the less interesting ones */
  if (users->len > p->max_users)
    {
      g_ptr_array_sort (users, compare_users_by_usefulness);
      g_ptr_array_set_size (users, p->max_users);
    }

  /* sort the users by name */
  g_ptr_array_sort (users, compare_users_by_label);

  /* add the users */
  for (i=0; i<users->len; ++i)
    {
      const IndicatorSessionUser * u = g_ptr_array_index (users, i);
      item = g_menu_item_new (u->real_name, NULL);
      g_menu_item_set_action_and_target (item, "indicator.switch-to-user", "s", u->user_name);
      g_menu_append_item (menu, item);
      g_object_unref (G_OBJECT(item));
    }

  /* cleanup */
  g_ptr_array_free (users, TRUE);
  return G_MENU_MODEL (menu);
}

static GMenuModel *
create_session_section (IndicatorSessionService * self)
{
  GMenu * menu;
  GMenuItem * item;
  const priv_t * const p = self->priv;
  GSettings * const s = p->indicator_settings;
  const gboolean ellipsis = use_ellipsis (self);

  menu = g_menu_new ();

  if (indicator_session_actions_can_logout (p->backend_actions) && !g_settings_get_boolean (s, "suppress-logout-menuitem"))
    {
      item = g_menu_item_new (ellipsis ? _("Log Out\342\200\246")
                                       : _("Log Out"), "indicator.logout");
      g_menu_append_item (menu, item);
      g_object_unref (G_OBJECT(item));
    }

  if (indicator_session_actions_can_suspend (p->backend_actions))
    {
      item = g_menu_item_new (_("Suspend"), "indicator.suspend");
      g_menu_append_item (menu, item);
      g_object_unref (G_OBJECT(item));
    }

  if (indicator_session_actions_can_hibernate (p->backend_actions))
    {
      item = g_menu_item_new (_("Hibernate"), "indicator.hibernate");
      g_menu_append_item (menu, item);
      g_object_unref (G_OBJECT(item));
    }

  if (!g_settings_get_boolean (s, "suppress-restart-menuitem"))
    {
      item = g_menu_item_new (ellipsis ? _("Restart\342\200\246")
                                       : _("Restart"), "indicator.restart");
      g_menu_append_item (menu, item);
      g_object_unref (G_OBJECT(item));
    }

  if (!g_settings_get_boolean (s, "suppress-shutdown-menuitem"))
    {
      item = g_menu_item_new (ellipsis ? _("Shutdown\342\200\246")
                                       : _("Shutdown"), "indicator.shutdown");
      g_menu_append_item (menu, item);
      g_object_unref (G_OBJECT(item));
    }

  return G_MENU_MODEL (menu);
}

static void
create_menu (IndicatorSessionService * self, int form_factor)
{
  GMenu * menu;
  GMenu * submenu;
  GMenuItem * header;
  GMenuModel * sections[16];
  int i;
  int n = 0;

  g_assert (0<=form_factor && form_factor<N_FORMS);
  g_assert (self->priv->menus[form_factor].menu == NULL);

  if (form_factor == FORM_DESKTOP)
    {
      sections[n++] = create_admin_section ();
      sections[n++] = create_settings_section (self);
      sections[n++] = create_switch_section (self);
      sections[n++] = create_session_section (self);
    }
  else if (form_factor == FORM_GREETER)
    {
      sections[n++] = create_session_section (self);
    }

  /* add sections to the submenu */
  submenu = g_menu_new ();
  for (i=0; i<n; ++i)
    {
      g_menu_append_section (submenu, NULL, sections[i]);
      g_object_unref (G_OBJECT(sections[i]));
    }

  /* add submenu to the header */
  header = g_menu_item_new (NULL, "indicator._header");
  g_menu_item_set_attribute (header, "x-canonical-type", "s", "com.canonical.indicator.root");
  g_menu_item_set_submenu (header, G_MENU_MODEL (submenu));
  g_object_unref (G_OBJECT(submenu));

  /* add header to the menu */
  menu = g_menu_new ();
  g_menu_append_item (menu, header);
  g_object_unref (G_OBJECT(header));

  self->priv->menus[form_factor].menu = menu;
  self->priv->menus[form_factor].submenu = submenu;
}

/***
****  GActions
***/

static IndicatorSessionActions *
get_backend_actions (gpointer gself)
{
  return INDICATOR_SESSION_SERVICE(gself)->priv->backend_actions;
}

static void
on_about_activated (GSimpleAction * a      G_GNUC_UNUSED,
                    GVariant      * param  G_GNUC_UNUSED,
                    gpointer        gself)
{
  indicator_session_actions_about (get_backend_actions(gself));
}

static void
on_help_activated (GSimpleAction  * a      G_GNUC_UNUSED,
                   GVariant       * param  G_GNUC_UNUSED,
                   gpointer         gself)
{
  indicator_session_actions_help (get_backend_actions(gself));
}

static void
on_settings_activated (GSimpleAction * a      G_GNUC_UNUSED,
                       GVariant      * param  G_GNUC_UNUSED,
                       gpointer        gself)
{
  indicator_session_actions_settings (get_backend_actions(gself));
}

static void
on_logout_activated (GSimpleAction * a      G_GNUC_UNUSED,
                     GVariant      * param  G_GNUC_UNUSED,
                     gpointer        gself)
{
  indicator_session_actions_logout (get_backend_actions(gself));
}

static void
on_suspend_activated (GSimpleAction * a      G_GNUC_UNUSED,
                      GVariant      * param  G_GNUC_UNUSED,
                      gpointer        gself)
{
  indicator_session_actions_suspend (get_backend_actions(gself));
}

static void
on_hibernate_activated (GSimpleAction * a      G_GNUC_UNUSED,
                        GVariant      * param  G_GNUC_UNUSED,
                        gpointer        gself)
{
  indicator_session_actions_hibernate (get_backend_actions(gself));
}

static void
on_restart_activated (GSimpleAction * action G_GNUC_UNUSED,
                      GVariant      * param  G_GNUC_UNUSED,
                      gpointer        gself)
{
  indicator_session_actions_restart (get_backend_actions(gself));
}

static void
on_shutdown_activated (GSimpleAction * a     G_GNUC_UNUSED,
                       GVariant      * param G_GNUC_UNUSED,
                       gpointer        gself)
{
  indicator_session_actions_shutdown (get_backend_actions(gself));
}

static void
on_guest_activated (GSimpleAction * a     G_GNUC_UNUSED,
                    GVariant      * param G_GNUC_UNUSED,
                    gpointer        gself)
{
  indicator_session_actions_switch_to_guest (get_backend_actions(gself));
}

static void
on_screensaver_activated (GSimpleAction * a      G_GNUC_UNUSED,
                          GVariant      * param  G_GNUC_UNUSED,
                          gpointer        gself)
{
  indicator_session_actions_switch_to_screensaver (get_backend_actions(gself));
}

static void
on_greeter_activated (GSimpleAction * a      G_GNUC_UNUSED,
                      GVariant      * param  G_GNUC_UNUSED,
                      gpointer        gself)
{
  indicator_session_actions_switch_to_greeter (get_backend_actions(gself));
}

static void
on_user_activated (GSimpleAction * a         G_GNUC_UNUSED,
                   GVariant      * param,
                   gpointer        gself)
{
  const char * username = g_variant_get_string (param, NULL);
  indicator_session_actions_switch_to_username (get_backend_actions(gself),
                                                username);
}

static void
init_gactions (IndicatorSessionService * self)
{
  GVariant * v;
  GSimpleAction * a;
  priv_t * p = self->priv;

  GActionEntry entries[] = {
    { "about",                  on_about_activated        },
    { "help",                   on_help_activated         },
    { "settings",               on_settings_activated     },
    { "logout",                 on_logout_activated       },
    { "suspend",                on_suspend_activated      },
    { "hibernate",              on_hibernate_activated    },
    { "restart",                on_restart_activated      },
    { "shutdown",               on_shutdown_activated     },
    { "switch-to-screensaver",  on_screensaver_activated  },
    { "switch-to-greeter",      on_greeter_activated      }
  };

  p->actions = g_simple_action_group_new ();

  g_action_map_add_action_entries (G_ACTION_MAP(p->actions),
                                   entries,
                                   G_N_ELEMENTS(entries),
                                   self);

  /* add switch-to-guest action */
  v = create_guest_switcher_state (self);
  a = g_simple_action_new_stateful ("switch-to-guest", NULL, v);
  g_signal_connect (a, "activate", G_CALLBACK(on_guest_activated), self);
  g_simple_action_group_insert (p->actions, G_ACTION(a));
  p->guest_switcher_action = a;

  /* add switch-to-user action... parameter is the uesrname */
  v = create_user_switcher_state (self);
  a = g_simple_action_new_stateful ("switch-to-user", G_VARIANT_TYPE_STRING, v);
  g_signal_connect (a, "activate", G_CALLBACK(on_user_activated), self);
  g_simple_action_group_insert (p->actions, G_ACTION(a));
  p->user_switcher_action = a;

  /* add the header action */
  v = g_variant_new ("(sssb)", "label", ICON_DEFAULT, "a11y", TRUE);
  a = g_simple_action_new_stateful ("_header", NULL, v);
  g_simple_action_group_insert (p->actions, G_ACTION(a));
  p->header_action = a;

  rebuild_now (self, SECTION_HEADER);
}

/***
****
***/

static void
replace_section (GMenu * parent, int pos, GMenuModel * new_section)
{
  g_menu_remove (parent, pos);
  g_menu_insert_section (parent, pos, NULL, new_section);
  g_object_unref (G_OBJECT(new_section));
}

static void
rebuild_now (IndicatorSessionService * self, int sections)
{
  priv_t * p = self->priv;
  struct FormMenuInfo * desktop = &p->menus[FORM_DESKTOP];
  struct FormMenuInfo * greeter = &p->menus[FORM_GREETER];

  if (sections & SECTION_HEADER)
    {
      update_header_action (self);
    }

  if (sections & SECTION_ADMIN)
    {
      replace_section (desktop->submenu, 0, create_admin_section());
    }

  if (sections & SECTION_SETTINGS)
    {
      replace_section (desktop->submenu, 1, create_settings_section(self));
    }

  if (sections & SECTION_SWITCH)
    {
      replace_section (desktop->submenu, 2, create_switch_section(self));
      update_switch_actions (self);
    }

  if (sections & SECTION_SESSION)
    {
      replace_section (desktop->submenu, 3, create_session_section(self));
      replace_section (greeter->submenu, 0, create_session_section(self));
    }
}

static int
rebuild_timeout_func (IndicatorSessionService * self)
{
  priv_t * p = self->priv;
  rebuild_now (self, p->rebuild_flags);
  p->rebuild_flags = 0;
  p->rebuild_id = 0;
  return G_SOURCE_REMOVE;
}

static void
rebuild_soon (IndicatorSessionService * self, int section)
{
  priv_t * p = self->priv;

  p->rebuild_flags |= section;

  if (p->rebuild_id == 0)
    {
      static const int REBUILD_INTERVAL_MSEC = 500;

      p->rebuild_id = g_timeout_add (REBUILD_INTERVAL_MSEC,
                                     (GSourceFunc)rebuild_timeout_func,
                                     self);
    }
}

/***
**** GDBus
***/

static void
on_bus_acquired (GDBusConnection * connection,
                 const gchar     * name,
                 gpointer          gself)
{
  int i;
  guint id;
  GError * err = NULL;
  IndicatorSessionService * self = INDICATOR_SESSION_SERVICE(gself);
  priv_t * p = self->priv;

  g_debug ("bus acquired: %s", name);

  p->conn = g_object_ref (G_OBJECT (connection));

  /* export the actions */
  if ((id = g_dbus_connection_export_action_group (connection,
                                                   BUS_PATH,
                                                   G_ACTION_GROUP (p->actions),
                                                   &err)))
    {
      p->actions_export_id = id;
    }
  else
    {
      g_warning ("cannot export action group: %s", err->message);
      g_clear_error (&err);
    }

  /* export the menus */
  for (i=0; i<N_FORMS; ++i)
    {
      char * path = g_strdup_printf ("%s/%s", BUS_PATH, menu_names[i]);
      struct FormMenuInfo * menu = &p->menus[i];

      if (menu->menu == NULL)
        create_menu (self, i);

      if ((id = g_dbus_connection_export_menu_model (connection,
                                                     path,
                                                     G_MENU_MODEL (menu->menu),
                                                     &err)))
        {
          menu->export_id = id;
        }
      else
        {
          g_warning ("cannot export %s menu: %s", menu_names[i], err->message);
          g_clear_error (&err);
        }

      g_free (path);
    }
}

static void
unexport (IndicatorSessionService * self)
{
  int i;
  priv_t * p = self->priv;

  /* unexport the menus */
  for (i=0; i<N_FORMS; ++i)
    {
      guint * id = &self->priv->menus[i].export_id;

      if (*id)
        {
          g_dbus_connection_unexport_menu_model (p->conn, *id);
          *id = 0;
        }
    }

  /* unexport the actions */
  if (p->actions_export_id)
    {
      g_dbus_connection_unexport_action_group (p->conn, p->actions_export_id);
      p->actions_export_id = 0;
    }
}

static void
on_name_lost (GDBusConnection * connection G_GNUC_UNUSED,
              const gchar     * name,
              gpointer          gself)
{
  g_debug ("%s %s name lost %s", G_STRLOC, G_STRFUNC, name);

  unexport (INDICATOR_SESSION_SERVICE (gself));
}

/***
****
***/

static void
/* cppcheck-suppress unusedFunction */
indicator_session_service_init (IndicatorSessionService * self)
{
  int i;
  GStrv keys;
  priv_t * p;
  gpointer gp;

  /* init our priv pointer */
  p = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                   INDICATOR_TYPE_SESSION_SERVICE,
                                   IndicatorSessionServicePrivate);
  p->indicator_settings = g_settings_new ("com.canonical.indicator.session");
  p->keybinding_settings = g_settings_new ("org.gnome.settings-daemon.plugins.media-keys");
  self->priv = p;

  /* init the backend objects */
  backend_get (g_cancellable_new (), &p->backend_actions,
                                     &p->backend_users,
                                     &p->backend_guest);

  /* init our key-to-User table */
  p->users = g_hash_table_new_full (g_str_hash,
                                    g_str_equal,
                                    g_free,
                                    (GDestroyNotify)indicator_session_user_free);
  keys = indicator_session_users_get_keys (p->backend_users);
  for (i=0; keys && keys[i]; ++i)
    add_user (self, keys[i]);
  g_strfreev (keys);

  init_gactions (self);

  /* watch for changes in backend_users */
  gp = p->backend_users;
  g_signal_connect (gp, INDICATOR_SESSION_USERS_SIGNAL_USER_ADDED,
                    G_CALLBACK(on_user_added), self);
  g_signal_connect (gp, INDICATOR_SESSION_USERS_SIGNAL_USER_CHANGED,
                    G_CALLBACK(on_user_changed), self);
  g_signal_connect (gp, INDICATOR_SESSION_USERS_SIGNAL_USER_REMOVED,
                    G_CALLBACK(on_user_removed), self);
  g_signal_connect_swapped (gp, "notify::is-live-session",
                            G_CALLBACK(rebuild_switch_section_soon), self);

  /* watch for changes in backend_guest */
  gp = p->backend_guest;
  g_signal_connect_swapped (gp, "notify::guest-is-active-session",
                            G_CALLBACK(rebuild_header_soon), self);
  g_signal_connect_swapped (gp, "notify",
                            G_CALLBACK(rebuild_switch_section_soon), self);

  /* watch for updates in backend_actions */
  gp = p->backend_actions;
  g_signal_connect_swapped (gp, "notify",
                            G_CALLBACK(rebuild_switch_section_soon), self);
  g_signal_connect_swapped (gp, "notify",
                            G_CALLBACK(rebuild_session_section_soon), self);
  g_signal_connect_swapped (gp, "notify::has-online-account-error",
                            G_CALLBACK(rebuild_header_soon), self);
  g_signal_connect_swapped (gp, "notify::has-online-account-error",
                            G_CALLBACK(rebuild_settings_section_soon), self);

  /* watch for changes in the indicator's settings */
  gp = p->indicator_settings;
  g_signal_connect_swapped (gp, "changed::suppress-logout-restart-shutdown",
                            G_CALLBACK(rebuild_switch_section_soon), self);
  g_signal_connect_swapped (gp, "changed::suppress-logout-restart-shutdown",
                            G_CALLBACK(rebuild_session_section_soon), self);
  g_signal_connect_swapped (gp, "changed::suppress-logout-menuitem",
                            G_CALLBACK(rebuild_session_section_soon), self);
  g_signal_connect_swapped (gp, "changed::suppress-restart-menuitem",
                            G_CALLBACK(rebuild_session_section_soon), self);
  g_signal_connect_swapped (gp, "changed::suppress-shutdown-menuitem",
                            G_CALLBACK(rebuild_session_section_soon), self);
  g_signal_connect_swapped (gp, "changed::show-real-name-on-panel",
                            G_CALLBACK(rebuild_header_soon), self);

  /* watch for changes to the lock keybinding */
  gp = p->keybinding_settings;
  g_signal_connect_swapped (gp, "changed::screensaver",
                            G_CALLBACK(rebuild_switch_section_soon), self);
}

static void
my_constructed (GObject * o)
{
  GBusNameOwnerFlags owner_flags;
  IndicatorSessionService * self = INDICATOR_SESSION_SERVICE(o);

  /* own the name in constructed() instead of init() so that
     we'll know the value of the 'replace' property */
  owner_flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
  if (self->priv->replace)
    owner_flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  self->priv->own_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                       BUS_NAME,
                                       owner_flags,
                                       on_bus_acquired,
                                       NULL,
                                       on_name_lost,
                                       self,
                                       NULL);
}

/***
****  GObject plumbing: properties
***/

static void
my_get_property (GObject     * o,
                  guint         property_id,
                  GValue      * value,
                  GParamSpec  * pspec)
{
  IndicatorSessionService * self = INDICATOR_SESSION_SERVICE (o);
 
  switch (property_id)
    {
      case PROP_REPLACE:
        g_value_set_boolean (value, self->priv->replace);
        break;

      case PROP_MAX_USERS:
        g_value_set_uint (value, self->priv->max_users);
        break;
 
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (o, property_id, pspec);
    }
}

static void
my_set_property (GObject       * o,
                 guint           property_id,
                 const GValue  * value,
                 GParamSpec    * pspec)
{
  IndicatorSessionService * self = INDICATOR_SESSION_SERVICE (o);

  switch (property_id)
    {
      case PROP_REPLACE:
        self->priv->replace = g_value_get_boolean (value);
        break;

      case PROP_MAX_USERS:
        self->priv->max_users = g_value_get_uint (value);
        rebuild_switch_section_soon (self);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (o, property_id, pspec);
    }
}

/***
****  GObject plumbing: life cycle
***/

static void
my_dispose (GObject * o)
{
  int i;
  IndicatorSessionService * self = INDICATOR_SESSION_SERVICE(o);
  priv_t * p = self->priv;

  unexport (self);

  if (p->rebuild_id)
    {
      g_source_remove (p->rebuild_id);
      p->rebuild_id = 0;
    }

  if (p->own_id)
    {
      g_bus_unown_name (p->own_id);
      p->own_id = 0;
    }

  g_clear_pointer (&p->users, g_hash_table_destroy);
  g_clear_object (&p->backend_users);
  g_clear_object (&p->backend_guest);
  g_clear_object (&p->backend_actions);
  g_clear_object (&p->indicator_settings);
  g_clear_object (&p->keybinding_settings);
  g_clear_object (&p->actions);

  for (i=0; i<N_FORMS; ++i)
    g_clear_object (&p->menus[i].menu);

  g_clear_object (&p->header_action);
  g_clear_object (&p->user_switcher_action);
  g_clear_object (&p->guest_switcher_action);
  g_clear_object (&p->conn);

  G_OBJECT_CLASS (indicator_session_service_parent_class)->dispose (o);
}

static void
/* cppcheck-suppress unusedFunction */
indicator_session_service_class_init (IndicatorSessionServiceClass * klass)
{
  GObjectClass * object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = my_dispose;
  object_class->constructed = my_constructed;
  object_class->get_property = my_get_property;
  object_class->set_property = my_set_property;

  g_type_class_add_private (klass, sizeof (IndicatorSessionServicePrivate));

  properties[PROP_0] = NULL;

  properties[PROP_REPLACE] = g_param_spec_boolean ("replace",
                                                   "Replace Service",
                                                   "Replace existing service",
                                                   FALSE,
                                                   G_PARAM_READWRITE |
                                                   G_PARAM_CONSTRUCT_ONLY |
                                                   G_PARAM_STATIC_STRINGS);

  properties[PROP_MAX_USERS] = g_param_spec_uint ("max-users",
                                                  "Max Users",
                                                  "Max visible users",
                                                  0, INT_MAX, 12,
                                                  G_PARAM_READWRITE |
                                                  G_PARAM_CONSTRUCT |
                                                  G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, properties);
}

IndicatorSessionService *
indicator_session_service_new (gboolean replace)
{
  GObject * o = g_object_new (INDICATOR_TYPE_SESSION_SERVICE,
                              "replace", replace,
                              NULL);

  return INDICATOR_SESSION_SERVICE (o);
}
