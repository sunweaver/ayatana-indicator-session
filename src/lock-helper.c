/*
A small helper for locking the screen.

Copyright 2009 Canonical Ltd.

Authors:
    Ted Gould <ted@canonical.com>

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

#include <glib/gi18n.h>
#include <gconf/gconf-client.h>
#include <dbus/dbus-glib.h>
#include "lock-helper.h"

#define GCONF_DIR  "/apps/gnome-screensaver"
#define GCONF_KEY  GCONF_DIR "/lock_enabled"

static DBusGProxy * gss_proxy = NULL;
static GMainLoop * gss_mainloop = NULL;
static guint cookie = 0;
static DBusGProxyCall * cookie_call = NULL;

static gboolean is_guest = FALSE;

static GConfClient * gconf_client = NULL;

void build_gss_proxy (void);

/* Checks to see if there is an error and reports
   it.  Not much else we can do. */
static void
unthrottle_return (DBusGProxy * proxy, DBusGProxyCall * call, gpointer data)
{
	GError * error = NULL;
	dbus_g_proxy_end_call(proxy, call, &error,
	                      G_TYPE_INVALID);

	if (error != NULL) {
		g_warning("Unable to unthrottle: %s", error->message);
	}
	return;
}

/* Sends an unthrottle if we're throttled. */
void
screensaver_unthrottle (void)
{
	g_return_if_fail(cookie != 0);

	build_gss_proxy();
	g_return_if_fail(gss_proxy != NULL);

	dbus_g_proxy_begin_call(gss_proxy, "UnThrottle",
	                        unthrottle_return, NULL,
	                        NULL,
	                        G_TYPE_UINT, cookie,
	                        G_TYPE_INVALID);

	cookie = 0;
	return;
}

/* Gets there return cookie from the throttle command
   and sets things valid */
static void
throttle_return (DBusGProxy * proxy, DBusGProxyCall * call, gpointer data)
{
	GError * error = NULL;
	cookie_call = NULL;

	dbus_g_proxy_end_call(proxy, call, &error,
	                      G_TYPE_UINT, &cookie,
	                      G_TYPE_INVALID);

	if (error != NULL) {
		g_warning("Unable to throttle the screensaver: %s", error->message);
		return;
	}


	if (cookie == 0) {
		g_warning("We didn't get a throttle cookie!");
	}

	return;
}

/* Throttling the screensaver by using the screen saver
   command. */
void
screensaver_throttle (gchar * reason)
{
	g_return_if_fail(cookie_call == NULL);
	g_return_if_fail(will_lock_screen());

	if (cookie != 0) {
		screensaver_unthrottle();
	}

	build_gss_proxy();
	g_return_if_fail(gss_proxy != NULL);

	cookie_call = dbus_g_proxy_begin_call(gss_proxy, "Throttle",
	                                      throttle_return, NULL,
	                                      NULL,
	                                      G_TYPE_STRING, "Session Menu",
	                                      G_TYPE_STRING, reason,
	                                      G_TYPE_INVALID);

	return;
}

/* This is our logic on whether the screen should be locked
   or not.  It effects everything else. */
gboolean
will_lock_screen (void)
{
	if (is_guest) {
		return FALSE;
	}

	if (gconf_client == NULL) {
		gconf_client = gconf_client_get_default();
	}

	return gconf_client_get_bool (gconf_client, GCONF_KEY, NULL);
}

/* When the screensave go active, if we've got a mainloop
   running we should quit it. */
static void
gss_active_changed (DBusGProxy * proxy, gboolean active, gpointer data)
{
	if (active && gss_mainloop != NULL) {
		g_main_loop_quit(gss_mainloop);
	}

	return;
}

/* Build the gss proxy and set up it's signals */
void
build_gss_proxy (void)
{
	if (gss_proxy == NULL) {
		DBusGConnection * session_bus = dbus_g_bus_get(DBUS_BUS_SESSION, NULL);
		g_return_if_fail(session_bus != NULL);

		gss_proxy = dbus_g_proxy_new_for_name(session_bus,
		                                      "org.gnome.ScreenSaver",
		                                      "/",
		                                      "org.gnome.ScreenSaver");
		g_return_if_fail(gss_proxy != NULL);

		dbus_g_proxy_add_signal(gss_proxy, "ActiveChanged", G_TYPE_BOOLEAN, G_TYPE_INVALID);
		dbus_g_proxy_connect_signal(gss_proxy, "ActiveChanged", G_CALLBACK(gss_active_changed), NULL, NULL);
	}

	return;
}

/* This is a timeout, we only want to wait for the screen to
   lock for a little bit, but not forever. */
static gboolean
activate_timeout (gpointer data)
{
	guint * address = (guint *)data;
	*address = 0;

	if (gss_mainloop != NULL) {
		g_main_loop_quit(gss_mainloop);
	}
	
	return FALSE;
}

/* Handle errors from activating the screensaver */
static void
active_cb (DBusGProxy * proxy, DBusGProxyCall * call, gpointer user_data)
{
	GError * error = NULL;

	dbus_g_proxy_end_call(proxy, call, &error, G_TYPE_INVALID);

	if (error != NULL) {
		g_warning("Unable to activate screensaver: %s", error->message);
		g_error_free(error);
	}

	return;
}

/* A fun little function to actually lock the screen.  If,
   that's what you want, let's do it! */
void
lock_screen (DbusmenuMenuitem * mi, guint timestamp, gpointer data)
{
	g_debug("Lock Screen");

	build_gss_proxy();
	g_return_if_fail(gss_proxy != NULL);

	if (will_lock_screen()) {
		dbus_g_proxy_call_no_reply(gss_proxy,
		                           "Lock",
		                           G_TYPE_INVALID,
		                           G_TYPE_INVALID);
	} else {
		dbus_g_proxy_begin_call(gss_proxy,
		                        "SetActive",
		                        active_cb, NULL,
		                        NULL,
		                        G_TYPE_BOOLEAN, TRUE,
		                        G_TYPE_INVALID);
	}

	if (gss_mainloop == NULL) {
		gss_mainloop = g_main_loop_new(NULL, FALSE);
	}

	guint timer = g_timeout_add_seconds(1, activate_timeout, &timer);

	g_main_loop_run(gss_mainloop);

	if (timer != 0) {
		g_source_remove(timer);
	}

	return;
}

/* Do what it takes to make the lock screen function work
   and be happy. */
gboolean
lock_screen_setup (gpointer data)
{
	if (!g_strcmp0(g_get_user_name(), "guest")) {
		is_guest = TRUE;
	}

	return FALSE;
}

/* When the GConf key changes we need to adjust the text on
   what we're going to do with the menu item */
static void
lockscreen_update (GConfClient *client, guint cnxn_id, GConfEntry  *entry, gpointer data) {
	DbusmenuMenuitem * mi = (DbusmenuMenuitem*) data;
	const gchar * key = gconf_entry_get_key (entry);

	if(g_strcmp0 (key, GCONF_KEY) == 0) {
		if (will_lock_screen()) {
			dbusmenu_menuitem_property_set(mi, DBUSMENU_MENUITEM_PROP_LABEL, _("Lock Screen"));
		} else {
			dbusmenu_menuitem_property_set(mi, DBUSMENU_MENUITEM_PROP_LABEL, _("Start Screensaver"));
		}
	}
}

/* Notification handler for lock menuitems. */
static guint lock_notify = 0;

/* Sets the menu item to be updating.  There can
   only be one.  So we clear and reset if we get
   another. */
void
lock_screen_update_item (DbusmenuMenuitem * mi)
{
	if (gconf_client == NULL) {
		gconf_client = gconf_client_get_default();
	}

	if (lock_notify == 0) {
		gconf_client_add_dir (gconf_client,
		                      GCONF_DIR,
		                      GCONF_CLIENT_PRELOAD_ONELEVEL,
		                      NULL);
	}

	if (lock_notify != 0) {
		gconf_client_notify_remove(gconf_client, lock_notify);
		lock_notify = 0;
	}

	lock_notify = gconf_client_notify_add(gconf_client,
	                                      GCONF_KEY,
	                                      lockscreen_update,
	                                      mi,
	                                      NULL,
	                                      NULL);

	return;
}