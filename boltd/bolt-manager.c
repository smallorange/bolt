/*
 * Copyright © 2017 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Christian J. Kellner <christian@kellner.me>
 */

#include "config.h"

#include "bolt-device.h"
#include "bolt-error.h"
#include "bolt-manager.h"
#include "bolt-store.h"

#include <libudev.h>
#include <string.h>

typedef struct udev_monitor udev_monitor;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (udev_monitor, udev_monitor_unref);

typedef struct udev_device udev_device;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (udev_device, udev_device_unref);


static void     bolt_manager_initable_iface_init (GInitableIface *iface);


static gboolean bolt_manager_initialize (GInitable    *initable,
                                         GCancellable *cancellable,
                                         GError      **error);

/*  */

static BoltDevice *  bolt_manager_get_device_by_syspath (BoltManager *mgr,
                                                         const char  *sysfs);

static BoltDevice *  bolt_manager_get_device_by_uid (BoltManager *mgr,
                                                     const char  *uid);

static BoltDevice *  bolt_manager_get_parent (BoltManager *mgr,
                                              BoltDevice  *dev);

static GPtrArray *   bolt_manager_get_children (BoltManager *mgr,
                                                BoltDevice  *target);

static void          handle_udev_device_added (BoltManager        *mgr,
                                               struct udev_device *udev);

static void          handle_udev_device_changed (BoltManager        *mgr,
                                                 BoltDevice         *dev,
                                                 struct udev_device *udev);

static void          hanlde_udev_device_removed (BoltManager *mgr,
                                                 BoltDevice  *dev);

static void          handle_udev_device_attached (BoltManager        *mgr,
                                                  BoltDevice         *dev,
                                                  struct udev_device *udev);

static void          handle_udev_device_detached (BoltManager *mgr,
                                                  BoltDevice  *dev);


/* dbus method calls */
static gboolean handle_list_devices (BoltDBusManager       *object,
                                     GDBusMethodInvocation *invocation,
                                     gpointer               user_data);

struct _BoltManager
{
  BoltDBusManagerSkeleton object;

  /* udev */
  struct udev         *udev;
  struct udev_monitor *udev_monitor;
  struct udev_monitor *kernel_monitor;
  GSource             *udev_source;
  GSource             *kernel_source;

  /* state */
  BoltStore *store;
  GPtrArray *devices;
};

enum {
  PROP_0,

  PROP_VERSION,

  PROP_LAST
};

G_DEFINE_TYPE_WITH_CODE (BoltManager,
                         bolt_manager,
                         BOLT_DBUS_TYPE_MANAGER_SKELETON,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                bolt_manager_initable_iface_init));


static void
bolt_manager_finalize (GObject *object)
{
  BoltManager *mgr = BOLT_MANAGER (object);

  if (mgr->udev_monitor)
    {
      udev_monitor_unref (mgr->udev_monitor);
      mgr->udev_monitor = NULL;

      g_source_destroy (mgr->udev_source);
      g_source_unref (mgr->udev_source);
      mgr->udev_source = NULL;
    }

  if (mgr->kernel_monitor)
    {
      udev_monitor_unref (mgr->kernel_monitor);
      mgr->kernel_monitor = NULL;

      g_source_destroy (mgr->kernel_source);
      g_source_unref (mgr->kernel_source);
      mgr->kernel_source = NULL;
    }

  if (mgr->udev)
    {
      udev_unref (mgr->udev);
      mgr->udev = NULL;
    }

  g_clear_object (&mgr->store);
  g_ptr_array_free (mgr->devices, TRUE);

  G_OBJECT_CLASS (bolt_manager_parent_class)->finalize (object);
}


static void
bolt_manager_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  switch (prop_id)
    {
    case PROP_VERSION:
      g_value_set_string (value, PACKAGE_VERSION);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
bolt_manager_set_property (GObject      *object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{

  switch (prop_id)
    {
    case PROP_VERSION:
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
bolt_manager_init (BoltManager *mgr)
{
  mgr->devices = g_ptr_array_new_with_free_func (g_object_unref);
  mgr->store = bolt_store_new (BOLT_DBDIR);

  g_signal_connect (mgr, "handle-list-devices", G_CALLBACK (handle_list_devices), NULL);
}

static void
bolt_manager_class_init (BoltManagerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = bolt_manager_finalize;
  gobject_class->get_property = bolt_manager_get_property;
  gobject_class->set_property = bolt_manager_set_property;

  g_object_class_override_property (gobject_class,
                                    PROP_VERSION,
                                    "version");
}

static void
bolt_manager_initable_iface_init (GInitableIface *iface)
{
  iface->init = bolt_manager_initialize;
}

static gboolean
setup_monitor (BoltManager   *mgr,
               const char    *name,
               GSourceFunc    callback,
               udev_monitor **monitor_out,
               GSource      **watch_out,
               GError       **error)
{
  g_autoptr(udev_monitor) monitor = NULL;
  g_autoptr(GIOChannel) channel = NULL;
  GSource *watch;
  int fd;
  int res;

  monitor = udev_monitor_new_from_netlink (mgr->udev, name);
  if (monitor == NULL)
    {
      g_set_error_literal (error, BOLT_ERROR, BOLT_ERROR_UDEV,
                           "udev: could not create monitor");
      return FALSE;
    }

  udev_monitor_set_receive_buffer_size (monitor, 128 * 1024 * 1024);

  res = udev_monitor_filter_add_match_subsystem_devtype (monitor, "thunderbolt", NULL);
  if (res < 0)
    {
      g_set_error_literal (error, BOLT_ERROR, BOLT_ERROR_UDEV,
                           "udev: could not add match for 'thunderbolt' to monitor");
      return FALSE;
    }

  res = udev_monitor_enable_receiving (monitor);
  if (res < 0)
    {
      g_set_error_literal (error, BOLT_ERROR, BOLT_ERROR_UDEV,
                           "udev: could not enable monitoring");
      return FALSE;
    }

  fd = udev_monitor_get_fd (monitor);

  if (fd < 0)
    {
      g_set_error_literal (error, BOLT_ERROR, BOLT_ERROR_UDEV,
                           "udev: could not obtain fd for monitoring");
      return FALSE;
    }

  channel = g_io_channel_unix_new (fd);
  watch   = g_io_create_watch (channel, G_IO_IN);

  g_source_set_callback (watch, callback, mgr, NULL);
  g_source_attach (watch, g_main_context_get_thread_default ());

  *monitor_out = udev_monitor_ref (monitor);
  *watch_out   = watch;

  return TRUE;
}

static gboolean
handle_uevent_kernel (GIOChannel  *source,
                      GIOCondition condition,
                      gpointer     user_data)
{
  BoltManager *mgr = BOLT_MANAGER (user_data);

  g_autoptr(udev_device) device = NULL;
  const char *action;

  device = udev_monitor_receive_device (mgr->kernel_monitor);

  if (device == NULL)
    return G_SOURCE_CONTINUE;

  action = udev_device_get_action (device);
  if (action == NULL)
    return G_SOURCE_CONTINUE;

  g_debug ("uevent [KERNEL]: %s", action);

  //if (g_str_equal (action, "add"))
  //  manager_devices_add_from_udev (mgr, device);

  return G_SOURCE_CONTINUE;
}

static gboolean
handle_uevent_udev (GIOChannel  *source,
                    GIOCondition condition,
                    gpointer     user_data)
{
  g_autoptr(BoltDevice) dev = NULL;
  g_autoptr(udev_device) device = NULL;
  BoltManager *mgr;
  const char *action;

  mgr = BOLT_MANAGER (user_data);
  device = udev_monitor_receive_device (mgr->udev_monitor);

  if (device == NULL)
    return G_SOURCE_CONTINUE;

  action = udev_device_get_action (device);
  if (action == NULL)
    return G_SOURCE_CONTINUE;

  g_debug ("uevent [ UDEV ]: %s", action);

  if (g_str_equal (action, "add") ||
      g_str_equal (action, "change"))
    {
      const char *uid;

      /* filter sysfs devices (e.g. the domain) that don't have
       * the unique_id attribute */
      uid = udev_device_get_sysattr_value (device, "unique_id");
      if (uid == NULL)
        return G_SOURCE_CONTINUE;

      dev = bolt_manager_get_device_by_uid (mgr, uid);

      if (!dev)
        handle_udev_device_added (mgr, device);
      else if (!bolt_device_is_connected (dev))
        handle_udev_device_attached (mgr, dev, device);
      else
        handle_udev_device_changed (mgr, dev, device);
    }
  else if (g_strcmp0 (action, "remove") == 0)
    {
      const char *syspath;
      const char *name;

      syspath = udev_device_get_syspath (device);
      if (syspath == NULL)
        {
          g_warning ("udev device without syspath");
          return G_SOURCE_CONTINUE;
        }

      /* filter out the domain controller */
      name = udev_device_get_sysname (device);
      if (name && g_str_has_prefix (name, "domain"))
        return G_SOURCE_CONTINUE;

      dev = bolt_manager_get_device_by_syspath (mgr, syspath);

      /* if we don't have any records of the device,
       *  then we don't care */
      if (!dev)
        return G_SOURCE_CONTINUE;

      if (bolt_device_get_store (dev) > 0)
        handle_udev_device_detached (mgr, dev);
      else
        hanlde_udev_device_removed (mgr, dev);
    }

  return G_SOURCE_CONTINUE;
}

static gboolean
bolt_manager_initialize (GInitable    *initable,
                         GCancellable *cancellable,
                         GError      **error)
{
  g_auto(GStrv) ids = NULL;
  BoltManager *mgr;
  struct udev_enumerate *enumerate;
  struct udev_list_entry *l, *devices;
  gboolean ok;

  mgr = BOLT_MANAGER (initable);

  mgr->udev = udev_new ();
  if (mgr->udev == NULL)
    {
      g_set_error_literal (error, BOLT_ERROR, BOLT_ERROR_UDEV,
                           "udev: could not create udev handle");
      return FALSE;
    }

  ok = setup_monitor (mgr, "kernel",
                      (GSourceFunc) handle_uevent_kernel,
                      &mgr->kernel_monitor, &mgr->kernel_source,
                      error);

  if (!ok)
    return FALSE;

  ok = setup_monitor (mgr, "udev",
                      (GSourceFunc) handle_uevent_udev,
                      &mgr->udev_monitor, &mgr->udev_source,
                      error);

  if (!ok)
    return FALSE;

  /* TODO: error checking */
  enumerate = udev_enumerate_new (mgr->udev);
  udev_enumerate_add_match_subsystem (enumerate, "thunderbolt");
  /* only devices (i.e. not the domain controller) */
  udev_enumerate_add_match_sysattr (enumerate, "unique_id", NULL);


  ids = bolt_store_list_uids (mgr->store, error);
  if (ids == NULL)
    {
      g_prefix_error (error, "failed to list devices in store");
      return FALSE;
    }

  g_debug ("Loading devices from store");
  for (guint i = 0; i < g_strv_length (ids); i++)
    {
      g_autoptr(GError) err = NULL;
      BoltDevice *dev = NULL;
      const char *uid = ids[i];

      dev = bolt_store_get_device (mgr->store, uid, &err);
      if (dev == NULL)
        {
          g_warning ("[%s] failed to load from store: %s",
                     uid, err->message);
          continue;
        }

      bolt_device_set_manager (dev, mgr);
      g_ptr_array_add (mgr->devices, dev);
    }

  g_debug ("Enumerating devices from udev");
  udev_enumerate_scan_devices (enumerate);
  devices = udev_enumerate_get_list_entry (enumerate);

  for (l = devices; l; l = udev_list_entry_get_next (l))
    {
      g_autoptr(udev_device) udevice = NULL;
      g_autoptr(BoltDevice) dev = NULL;
      const char *uid;
      const char *syspath;

      syspath = udev_list_entry_get_name (l);
      udevice = udev_device_new_from_syspath (mgr->udev, syspath);

      if (udevice == NULL)
        continue;

      uid = udev_device_get_sysattr_value (udevice, "unique_id");
      g_assert (uid); /* cant really happen due to the match rule */

      dev = bolt_manager_get_device_by_uid (mgr, uid);
      if (dev)
        handle_udev_device_attached (mgr, dev, udevice);
      else
        handle_udev_device_added (mgr, udevice);
    }

  return TRUE;
}

static BoltDevice *
bolt_manager_get_device_by_syspath (BoltManager *mgr,
                                    const char  *sysfs)
{

  g_return_val_if_fail (sysfs != NULL, NULL);

  for (guint i = 0; i < mgr->devices->len; i++)
    {
      BoltDevice *dev = g_ptr_array_index (mgr->devices, i);
      const char *have = bolt_device_get_syspath (dev);

      if (have && g_str_equal (have, sysfs))
        return g_object_ref (dev);

    }

  return NULL;
}

static BoltDevice *
bolt_manager_get_device_by_uid (BoltManager *mgr,
                                const char  *uid)
{
  for (guint i = 0; i < mgr->devices->len; i++)
    {
      BoltDevice *dev = g_ptr_array_index (mgr->devices, i);

      if (!g_strcmp0 (bolt_device_get_uid (dev), uid))
        return g_object_ref (dev);

    }

  return NULL;
}


static BoltDevice *
bolt_manager_get_parent (BoltManager *mgr,
                         BoltDevice  *dev)
{
  g_autofree char *path = NULL;
  const char *syspath;
  const char *start;
  char *pos;

  syspath = bolt_device_get_syspath (dev);
  g_return_val_if_fail (syspath != NULL, NULL);

  path = g_strdup (syspath);
  start = path + strlen ("/sys");

  pos = strrchr (start, '/');
  if (!pos || pos < start + 2)
    return NULL;

  *pos = '\0';

  return bolt_manager_get_device_by_syspath (mgr, path);
}

static GPtrArray *
bolt_manager_get_children (BoltManager *mgr,
                           BoltDevice  *target)
{
  GPtrArray *res;

  res = g_ptr_array_new_with_free_func (g_object_unref);

  for (guint i = 0; i < mgr->devices->len; i++)
    {
      g_autoptr(BoltDevice) parent = NULL;
      BoltDevice *dev = g_ptr_array_index (mgr->devices, i);

      parent = bolt_manager_get_parent (mgr, dev);
      if (parent != target)
        continue;

      g_ptr_array_add (res, g_object_ref (dev));
    }

  return res;
}

/* device authorization */
static void
authorize_device_finish (BoltDevice *dev,
                         gboolean    ok,
                         GError    **error,
                         gpointer    user_data)
{
  const char *uid;

  uid = bolt_device_get_uid (dev);

  if (ok)
    g_info ("[%s] authorized", uid);
  else
    g_warning ("[%s] authorization failed: %s",
               uid, (*error)->message);
}

static gboolean
authorize_device_idle (gpointer user_data)
{
  g_autoptr(GError) error = NULL;
  BoltDevice *dev = BOLT_DEVICE (user_data);
  const char *uid = bolt_device_get_uid (dev);
  gboolean ok;

  g_info ("[%s] authorizing", uid);
  ok = bolt_device_authorize (dev,
                              authorize_device_finish,
                              NULL,
                              &error);
  if (!ok)
    g_warning ("[%s] failed to initiate authorization: %s",
               uid, error->message);

  g_object_unref (dev);
  return G_SOURCE_REMOVE;
}

static void
maybe_authorize_device (BoltManager *mgr,
                        BoltDevice  *dev)
{
  BoltStatus status = bolt_device_get_status (dev);
  BoltPolicy policy = bolt_device_get_policy (dev);
  const char *uid = bolt_device_get_uid (dev);
  guint store;

  g_debug ("[%s] checking possible authorization: %s (%x)",
           uid, bolt_policy_to_string (policy), status);

  if (bolt_status_is_authorized (status) ||
      policy != BOLT_POLICY_AUTO)
    return;

  store = bolt_device_get_store (dev);
  /* sanity check, because we already checked the policy */
  g_return_if_fail (store > 0);

  g_idle_add (authorize_device_idle, g_object_ref (dev));
}

/* udev callbacks */
static void
handle_udev_device_added (BoltManager        *mgr,
                          struct udev_device *udev)
{
  g_autoptr(GError) err = NULL;
  GDBusConnection *bus;
  BoltDevice *dev;
  const char *opath;
  const char *uid;
  const char *syspath;

  dev = bolt_device_new_for_udev (mgr, udev, &err);
  if (dev == NULL)
    {
      g_warning ("Could not create device for udev: %s", err->message);
      return;
    }

  g_ptr_array_add (mgr->devices, dev);

  uid = bolt_device_get_uid (dev);
  syspath = udev_device_get_syspath (udev);
  g_info ("[%s] added (%s)", uid, syspath);

  /* if we have a valid dbus connection */
  bus = g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (mgr));
  if (bus == NULL)
    return;

  opath = bolt_device_export (dev, bus, &err);
  if (opath == NULL)
    g_warning ("[%s] error exporting: %s", uid, err->message);
  else
    g_debug ("[%s] exporting device: %s", uid, opath);

  bolt_dbus_manager_emit_device_added (BOLT_DBUS_MANAGER (mgr), opath);
}

static void
handle_udev_device_changed (BoltManager        *mgr,
                            BoltDevice         *dev,
                            struct udev_device *udev)
{
  g_autoptr(GPtrArray) children = NULL;
  BoltStatus after;
  const char *uid;

  uid = bolt_device_get_uid (dev);
  after = bolt_device_update_from_udev (dev, udev);

  g_info ("[%s] device changed: %x", uid, after);

  if (!bolt_status_is_authorized (after))
    return;

  children = bolt_manager_get_children (mgr, dev);

  for (guint i = 0; i < children->len; i++)
    {
      BoltDevice *child = g_ptr_array_index (children, i);
      maybe_authorize_device (mgr, child);
    }
}

static void
hanlde_udev_device_removed (BoltManager *mgr,
                            BoltDevice  *dev)
{
  const char *opath;
  const char *uid;
  const char *syspath;

  uid = bolt_device_get_uid (dev);
  syspath = bolt_device_get_syspath (dev);
  g_info ("[%s] removed (%s)", uid, syspath);

  g_ptr_array_remove_fast (mgr->devices, dev);

  opath = bolt_device_get_object_path (dev);

  if (opath == NULL)
    return;

  bolt_dbus_manager_emit_device_removed (BOLT_DBUS_MANAGER (mgr), opath);
  bolt_device_unexport (dev);
  g_debug ("[%s] unexported", uid);
}

static void
handle_udev_device_attached (BoltManager        *mgr,
                             BoltDevice         *dev,
                             struct udev_device *udev)
{
  g_autoptr(BoltDevice) parent = NULL;
  const char *uid;
  const char *syspath;
  BoltStatus status;

  status = bolt_device_connected (dev, udev);

  uid = bolt_device_get_uid (dev);
  syspath = bolt_device_get_syspath (dev);
  g_info ("[%s] connected: %x (%s)", uid, status, syspath);

  if (status != BOLT_STATUS_CONNECTED)
    return;

  parent = bolt_manager_get_parent (mgr, dev);
  if (parent)
    {
      const char *pid = bolt_device_get_uid (parent);
      status = bolt_device_get_status (parent);
      if (!bolt_status_is_authorized (status))
        {
          g_debug ("[%s] parent [%s] not authorized", uid, pid);
          return;
        }
    }
  else
    {
      g_warning ("[%s] could not find parent", uid);
    }

  maybe_authorize_device (mgr, dev);
}

static void
handle_udev_device_detached (BoltManager *mgr,
                             BoltDevice  *dev)
{
  const char *uid;
  const char *syspath;

  uid = bolt_device_get_uid (dev);
  syspath = bolt_device_get_syspath (dev);
  g_info ("[%s] disconnected (%s)", uid, syspath);

  bolt_device_disconnected (dev);
}


/* dbus methods */
static gboolean
handle_list_devices (BoltDBusManager       *obj,
                     GDBusMethodInvocation *inv,
                     gpointer               user_data)
{
  BoltManager *mgr = BOLT_MANAGER (obj);
  const char **devs;

  devs = g_newa (const char *, mgr->devices->len);

  for (guint i = 0; i < mgr->devices->len; i++)
    {
      BoltDevice *d = g_ptr_array_index (mgr->devices, i);
      devs[i] = bolt_device_get_object_path (d);
    }

  bolt_dbus_manager_complete_list_devices (obj, inv, devs);
  return TRUE;
}

/* public methods */

gboolean
bolt_manager_export (BoltManager     *mgr,
                     GDBusConnection *connection,
                     GError         **error)
{
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (mgr),
                                         connection,
                                         "/org/freedesktop/Bolt",
                                         error))
    return FALSE;

  for (guint i = 0; i < mgr->devices->len; i++)
    {
      g_autoptr(GError) err  = NULL;
      BoltDevice *dev = g_ptr_array_index (mgr->devices, i);
      const char *uid;
      const char *opath;

      uid = bolt_device_get_uid (dev);
      opath = bolt_device_export (dev, connection, error);
      if (opath == NULL)
        g_warning ("[%s] error exporting: %s", uid, err->message);
      else
        g_debug ("[%s] exporting device: %s", uid, opath);
    }

  return TRUE;
}

BoltStore *
bolt_manager_get_store (BoltManager *mgr)
{
  return mgr->store;
}
