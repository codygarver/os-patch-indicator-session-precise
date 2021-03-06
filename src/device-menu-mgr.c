/*
Copyright 2011 Canonical Ltd.

Authors:
    Conor Curran <conor.curran@canonical.com>

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

#include <config.h>
#include <libdbusmenu-glib/client.h>

#include "device-menu-mgr.h"
#include "settings-helper.h"
#include "dbus-shared-names.h"
#include "dbusmenu-shared.h"
#include "upower-client.h"

#include "udev-mgr.h"

#define UP_ADDRESS    "org.freedesktop.UPower"
#define UP_OBJECT     "/org/freedesktop/UPower"
#define UP_INTERFACE  "org.freedesktop.UPower"

#define EXTRA_LAUNCHER_DIR "/usr/share/indicators/session/applications"

struct _DeviceMenuMgr
{
	GObject parent_instance;
  DbusmenuMenuitem* root_item;
  SessionDbus* session_dbus_interface;  
  UdevMgr* udev_mgr;
};

static DBusGProxyCall * suspend_call = NULL;
static DBusGProxyCall * hibernate_call = NULL;

static DbusmenuMenuitem * hibernate_mi = NULL;
static DbusmenuMenuitem * suspend_mi = NULL;
static DbusmenuMenuitem * logout_mi = NULL;
static DbusmenuMenuitem * shutdown_mi = NULL;

static gboolean can_hibernate = TRUE;
static gboolean can_suspend = TRUE;
static gboolean allow_hibernate = FALSE;
static gboolean allow_suspend = TRUE;

static DBusGProxy * up_main_proxy = NULL;
static DBusGProxy * up_prop_proxy = NULL;

static void setup_up (DeviceMenuMgr* self);
static void machine_sleep_with_context (DeviceMenuMgr* self,
                                        gchar* type);
                                                                  
static void
machine_sleep_from_hibernate (DbusmenuMenuitem * mi,
                              guint timestamp,
                              gpointer userdata);
static void
machine_sleep_from_suspend (DbusmenuMenuitem * mi,
                            guint timestamp,
                            gpointer userdata);

G_DEFINE_TYPE (DeviceMenuMgr, device_menu_mgr, G_TYPE_OBJECT);

static void
device_menu_mgr_init (DeviceMenuMgr *self)
{
  self->root_item = dbusmenu_menuitem_new ();  
	setup_up(self);  
}

static void
device_menu_mgr_finalize (GObject *object)
{
	G_OBJECT_CLASS (device_menu_mgr_parent_class)->finalize (object);
}

// TODO refactor into one helper method for both menu mgrs.
static void
device_menu_mgr_class_init (DeviceMenuMgrClass *klass)
{
	GObjectClass* object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = device_menu_mgr_finalize;
}

static void
machine_sleep_from_suspend (DbusmenuMenuitem * mi,
                            guint timestamp,
                            gpointer userdata)
{
  DeviceMenuMgr* self = DEVICE_MENU_MGR (userdata);
  machine_sleep_with_context (self, "Suspend");
}

static void
machine_sleep_from_hibernate (DbusmenuMenuitem * mi,
                              guint timestamp,
                              gpointer userdata)
{
  DeviceMenuMgr* self = DEVICE_MENU_MGR (userdata);
  machine_sleep_with_context (self, "Hibernate");
}

/* Let's put this machine to sleep, with some info on how
   it should sleep.  */
static void
machine_sleep_with_context (DeviceMenuMgr* self, gchar* type)
{
	if (up_main_proxy == NULL) {
		g_warning("Can not %s as no upower proxy", type);
	}

	dbus_g_proxy_begin_call(up_main_proxy,
	                        type,
	                        NULL,
	                        NULL,
	                        NULL,
	                        G_TYPE_INVALID);

	return;
}

/* A response to getting the suspend property */
static void
suspend_prop_cb (DBusGProxy * proxy, DBusGProxyCall * call, gpointer userdata)
{
	suspend_call = NULL;
  
	GValue candoit = {0};
	GError * error = NULL;
	dbus_g_proxy_end_call(proxy, call, &error, G_TYPE_VALUE, &candoit, G_TYPE_INVALID);
	if (error != NULL) {
		g_warning("Unable to check suspend: %s", error->message);
		g_error_free(error);
		return;
	}
	g_debug("Got Suspend: %s", g_value_get_boolean(&candoit) ? "true" : "false");

	gboolean local_can_suspend = g_value_get_boolean(&candoit);
	if (local_can_suspend != can_suspend) {
		can_suspend = local_can_suspend;
	}
	return;
}

/* Response to getting the hibernate property */
static void
hibernate_prop_cb (DBusGProxy * proxy, DBusGProxyCall * call, gpointer userdata)
{
	hibernate_call = NULL;

	GValue candoit = {0};
	GError * error = NULL;
	dbus_g_proxy_end_call(proxy, call, &error, G_TYPE_VALUE, &candoit, G_TYPE_INVALID);
	if (error != NULL) {
		g_warning("Unable to check hibernate: %s", error->message);
		g_error_free(error);
		return;
	}
	g_debug("Got Hibernate: %s", g_value_get_boolean(&candoit) ? "true" : "false");

	gboolean local_can_hibernate = g_value_get_boolean(&candoit);
	if (local_can_hibernate != can_hibernate) {
		can_hibernate = local_can_hibernate;
	}
}

/* A signal that we need to recheck to ensure we can still
   hibernate and/or suspend */
static void
up_changed_cb (DBusGProxy * proxy, gpointer user_data)
{
	/* Start Async call to see if we can hibernate */
	if (suspend_call == NULL) {
		suspend_call = dbus_g_proxy_begin_call(up_prop_proxy,
		                                       "Get",
		                                       suspend_prop_cb,
		                                       user_data,
		                                       NULL,
		                                       G_TYPE_STRING,
		                                       UP_INTERFACE,
		                                       G_TYPE_STRING,
		                                       "CanSuspend",
		                                       G_TYPE_INVALID,
		                                       G_TYPE_VALUE,
		                                       G_TYPE_INVALID);
	}

	/* Start Async call to see if we can suspend */
	if (hibernate_call == NULL) {
		hibernate_call = dbus_g_proxy_begin_call(up_prop_proxy,
		                                         "Get",
		                                         hibernate_prop_cb,
		                                         user_data,
		                                         NULL,
		                                         G_TYPE_STRING,
		                                         UP_INTERFACE,
		                                         G_TYPE_STRING,
		                                         "CanHibernate",
		                                         G_TYPE_INVALID,
		                                         G_TYPE_VALUE,
		                                         G_TYPE_INVALID);
	}
}
/* Handle the callback from the allow functions to check and
   see if we're changing the value, and if so, rebuilding the
   menus based on that info. */
static void
allowed_suspend_cb (DBusGProxy *proxy,
                    gboolean OUT_allowed,
                    GError *error,
                    gpointer userdata)
{
	if (error != NULL) {
		g_warning("Unable to get information on what is allowed from UPower: %s",
               error->message);
		return;
	}
  
	if (OUT_allowed != allow_suspend) {
    allow_suspend = OUT_allowed;
  }
}

/* Handle the callback from the allow functions to check and
   see if we're changing the value, and if so, rebuilding the
   menus based on that info. */
static void
allowed_hibernate_cb (DBusGProxy *proxy,
                      gboolean OUT_allowed,
                      GError *error,
                      gpointer userdata)
{
	if (error != NULL) {
		g_warning("Unable to get information on what is allowed from UPower: %s",
               error->message);
		return;
	}
  
	if (OUT_allowed != allow_hibernate) {
    allow_hibernate = OUT_allowed;
  }
}

/* This function goes through and sets up what we need for
   DKp checking.  We're even setting up the calls for the props
   we need */
static void
setup_up (DeviceMenuMgr* self) {
	DBusGConnection * bus = dbus_g_bus_get(DBUS_BUS_SYSTEM, NULL);
	g_return_if_fail(bus != NULL);

	if (up_main_proxy == NULL) {
		up_main_proxy = dbus_g_proxy_new_for_name(bus,
		                                           UP_ADDRESS,
		                                           UP_OBJECT,
		                                           UP_INTERFACE);
	}
	g_return_if_fail(up_main_proxy != NULL);

	if (up_prop_proxy == NULL) {
		up_prop_proxy = dbus_g_proxy_new_for_name(bus,
                                              UP_ADDRESS,
                                              UP_OBJECT,
                                              DBUS_INTERFACE_PROPERTIES);
		/* Connect to changed signal */
		dbus_g_proxy_add_signal(up_main_proxy,
		                        "Changed",
		                        G_TYPE_INVALID);

		dbus_g_proxy_connect_signal(up_main_proxy,
		                            "Changed",
		                            G_CALLBACK(up_changed_cb),
		                            self,
		                            NULL);
	}
	g_return_if_fail(up_prop_proxy != NULL);


	/* Force an original "changed" event */
	up_changed_cb(up_main_proxy, self);

	/* Check to see if these are getting blocked by PolicyKit */
	org_freedesktop_UPower_suspend_allowed_async(up_main_proxy,
	                                             allowed_suspend_cb,
	                                             self);
	org_freedesktop_UPower_hibernate_allowed_async(up_main_proxy,
	                                               allowed_hibernate_cb,
	                                               self);

	return;
}

/* This is the function to show a dialog on actions that
   can destroy data.  Currently it just calls the GTK version
   but it seems that in the future it should figure out
   what's going on and something better. */
static void
show_dialog (DbusmenuMenuitem * mi, guint timestamp, gchar * type)
{

#ifdef HAVE_GTKLOGOUTHELPER
	gchar * helper = g_build_filename(LIBEXECDIR, "gtk-logout-helper", NULL);
#else
	gchar * helper = g_build_filename("gnome-session-quit", NULL);
#endif  /* HAVE_GTKLOGOUTHELPER */
	gchar * dialog_line = g_strdup_printf("%s --%s", helper, type);
	g_free(helper);

	g_debug("Showing dialog '%s'", dialog_line);

	GError * error = NULL;
	if (!g_spawn_command_line_async(dialog_line, &error)) {
		g_warning("Unable to show dialog: %s", error->message);
		g_error_free(error);
	}
	g_free(dialog_line);  
}

static void
device_menu_mgr_build_static_items (DeviceMenuMgr* self, gboolean greeter_mode)
{

  // Session control  
  if (!greeter_mode) {
    logout_mi = dbusmenu_menuitem_new();

    if (supress_confirmations()) {
      dbusmenu_menuitem_property_set (logout_mi,
                                      DBUSMENU_MENUITEM_PROP_LABEL,
                                      _("Log Out"));
    }
    else {
      dbusmenu_menuitem_property_set (logout_mi,
                                      DBUSMENU_MENUITEM_PROP_LABEL,
                                      _("Log Out\342\200\246"));
    }
    dbusmenu_menuitem_property_set_bool (logout_mi,
                                         DBUSMENU_MENUITEM_PROP_VISIBLE,
                                         show_logout());
    dbusmenu_menuitem_child_append(self->root_item, logout_mi);
    g_signal_connect( G_OBJECT(logout_mi),
                      DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                      G_CALLBACK(show_dialog), "logout");
  }

	if (can_suspend && allow_suspend) {
		suspend_mi = dbusmenu_menuitem_new();
		dbusmenu_menuitem_property_set (suspend_mi,
                                    DBUSMENU_MENUITEM_PROP_LABEL,
                                    _("Suspend"));
		dbusmenu_menuitem_child_append (self->root_item, suspend_mi);
		g_signal_connect( G_OBJECT(suspend_mi),
                      DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                      G_CALLBACK(machine_sleep_from_suspend),
                      self);
	}

	if (can_hibernate && allow_hibernate) {
		hibernate_mi = dbusmenu_menuitem_new();
		dbusmenu_menuitem_property_set (hibernate_mi,
                                    DBUSMENU_MENUITEM_PROP_LABEL,
                                    _("Hibernate"));
		dbusmenu_menuitem_child_append(self->root_item, hibernate_mi);
		g_signal_connect (G_OBJECT(hibernate_mi),
                      DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                      G_CALLBACK(machine_sleep_from_hibernate), self);
	}
  
	shutdown_mi = dbusmenu_menuitem_new();

	if (supress_confirmations()) {
		dbusmenu_menuitem_property_set (shutdown_mi,
                                    DBUSMENU_MENUITEM_PROP_LABEL,
                                    _("Shut Down"));
	}
  else {
		dbusmenu_menuitem_property_set (shutdown_mi,
                                    DBUSMENU_MENUITEM_PROP_LABEL,
                                    _("Shut Down\342\200\246"));
	}
	dbusmenu_menuitem_property_set_bool (shutdown_mi,
                                       DBUSMENU_MENUITEM_PROP_VISIBLE,
                                       show_shutdown());
	dbusmenu_menuitem_child_append (self->root_item, shutdown_mi);
	g_signal_connect (G_OBJECT(shutdown_mi),
                    DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
#ifdef HAVE_GTKLOGOUTHELPER
                    G_CALLBACK(show_dialog), "shutdown");
#else
                    G_CALLBACK(show_dialog), "power-off");
#endif  /* HAVE_GTKLOGOUTHELPER */

	RestartShutdownLogoutMenuItems * restart_shutdown_logout_mi = g_new0 (RestartShutdownLogoutMenuItems, 1);
	restart_shutdown_logout_mi->logout_mi = logout_mi;
	restart_shutdown_logout_mi->shutdown_mi = shutdown_mi;

	update_menu_entries(restart_shutdown_logout_mi);
}

DbusmenuMenuitem*
device_mgr_get_root_item (DeviceMenuMgr* self)
{
  return self->root_item;
}

/*
 * Clean Entry Point 
 */
DeviceMenuMgr* device_menu_mgr_new (SessionDbus* session_dbus, gboolean greeter_mode)
{
  DeviceMenuMgr* device_mgr = g_object_new (DEVICE_TYPE_MENU_MGR, NULL);
  device_mgr->session_dbus_interface = session_dbus;
  device_menu_mgr_build_static_items (device_mgr, greeter_mode);
  return device_mgr;
}
