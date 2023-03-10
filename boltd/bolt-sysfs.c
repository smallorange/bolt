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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Christian J. Kellner <christian@kellner.me>
 */

#include "config.h"

#include "bolt-sysfs.h"

#include "bolt-error.h"
#include "bolt-io.h"
#include "bolt-log.h"
#include "bolt-names.h"
#include "bolt-str.h"

#include <errno.h>
#include <libudev.h>
#include <string.h>
#include <sys/stat.h>

typedef struct udev_device udev_device;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (udev_device, udev_device_unref);

void
bolt_ident_clear (BoltIdent *id)
{
  if (id->udev == NULL)
    return;

  udev_device_unref (id->udev);
  memset (id, 0, sizeof (BoltIdent));
}

static const char *
sysfs_get_sysattr_value (struct udev_device *dev,
                         const char         *attr,
                         GError            **error)
{
  const char *val;

  g_return_val_if_fail (dev != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  val = udev_device_get_sysattr_value (dev, attr);

  if (val == NULL)
    {
      int code = errno;
      const char *path = udev_device_get_syspath (dev);

      g_set_error (error, BOLT_ERROR, BOLT_ERROR_UDEV,
                   "could not get '%s' for %s: %s",
                   attr, path, g_strerror (code));
    }

  return val;
}

const char *
bolt_sysfs_device_get_unique_id (struct udev_device *dev,
                                 GError            **error)
{
  g_return_val_if_fail (dev != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return sysfs_get_sysattr_value (dev, BOLT_SYSFS_UNIQUE_ID, error);
}

gint64
bolt_sysfs_device_get_time (struct udev_device *udev,
                            BoltStatTime        st)
{
  const char *path;
  struct stat sb;
  gint64 ms = 0;
  int r;

  path = udev_device_get_syspath (udev);

  if (path == NULL)
    return 0;

  r = lstat (path, &sb);

  if (r == -1)
    return 0;

  switch (st)
    {
    case BOLT_ST_CTIME:
      ms = (gint64) sb.st_ctim.tv_sec;
      break;

    case BOLT_ST_ATIME:
      ms = (gint64) sb.st_atim.tv_sec;
      break;

    case BOLT_ST_MTIME:
      ms = (gint64) sb.st_mtim.tv_sec;
      break;
    }

  if (ms < 0)
    ms = 0;

  return ms;
}

gboolean
bolt_sysfs_device_is_domain (struct udev_device *udev,
                             GError            **error)
{
  const char *devtype = udev_device_get_devtype (udev);
  const char *subsystem = udev_device_get_subsystem (udev);
  gboolean is_domain;

  is_domain = bolt_streq (subsystem, "thunderbolt") &&
              bolt_streq (devtype, "thunderbolt_domain");

  if (!is_domain)
    {
      const char *syspath = udev_device_get_syspath (udev);
      g_set_error (error, BOLT_ERROR, BOLT_ERROR_UDEV,
                   "device '%s' is not a thunderbolt domain",
                   syspath);
    }

  return is_domain;
}

struct udev_device *
bolt_sysfs_domain_for_device (struct udev_device  *udev,
                              struct udev_device **host_out)
{
  struct udev_device *parent;
  struct udev_device *host;
  gboolean found;

  found = FALSE;
  parent = udev;
  do
    {
      host = parent;
      parent = udev_device_get_parent (host);
      if (!parent)
        break;

      found = bolt_sysfs_device_is_domain (parent, NULL);
    }
  while (!found);

  if (!found)
    return NULL;

  if (host_out)
    *host_out = host;

  return parent;
}

BoltSecurity
bolt_sysfs_security_for_device (struct udev_device *udev,
                                GError            **error)
{
  struct udev_device *parent = NULL;
  const char *v;
  BoltSecurity s;

  if (bolt_sysfs_device_is_domain (udev, NULL))
    parent = udev;
  else
    parent = bolt_sysfs_domain_for_device (udev, NULL);

  if (parent == NULL)
    {
      g_set_error_literal (error, BOLT_ERROR, BOLT_ERROR_UDEV,
                           "failed to determine domain device");
      return BOLT_SECURITY_UNKNOWN;
    }

  v = udev_device_get_sysattr_value (parent, "security");
  s = bolt_enum_from_string (BOLT_TYPE_SECURITY, v, error);

  return s;
}

static const char *
read_sysattr_name (struct udev_device *udev,
                   const char         *attr,
                   GError            **error)
{
  g_autofree char *s = NULL;
  const char *v;

  s = g_strdup_printf ("%s_name", attr);
  v = udev_device_get_sysattr_value (udev, s);

  if (v != NULL)
    return v;

  return sysfs_get_sysattr_value (udev, attr, error);
}

gboolean
bolt_sysfs_device_ident (struct udev_device *udev,
                         BoltIdent          *id,
                         GError            **error)
{
  const char *name;
  const char *vendor;

  g_return_val_if_fail (udev != NULL, FALSE);
  g_return_val_if_fail (id != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  vendor = read_sysattr_name (udev, "vendor", error);
  if (vendor == NULL)
    return FALSE;

  name = read_sysattr_name (udev, "device", error);
  if (name == NULL)
    return FALSE;

  id->udev = udev_device_ref (udev);
  id->name = name;
  id->vendor = vendor;

  return TRUE;
}

gboolean
bolt_sysfs_host_ident (struct udev_device *dev,
                       BoltIdent          *id,
                       GError            **error)
{
  g_autoptr(udev_device) dmi = NULL;
  struct udev *udev;
  const char *name;
  const char *vendor;
  const char *attr;
  gboolean ok;

  g_return_val_if_fail (dev != NULL, FALSE);
  g_return_val_if_fail (id != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  /* First check if the host controller has normal device ident,
   * which should be present for all controller that have a DROM. */
  ok = bolt_sysfs_device_ident (dev, id, NULL);
  if (ok)
    return TRUE;

  /* On embedded thunderbolt controllers without DROM, we
   * fall back to using the SMBIOS/DMI system information */

  udev = udev_device_get_udev (dev);
  dmi = udev_device_new_from_syspath (udev, BOLT_SYSFS_DMI_ID);

  if (dmi == NULL)
    {
      int code = errno;
      g_set_error (error, BOLT_ERROR, BOLT_ERROR_UDEV,
                   "could not open dmi/id device: %s",
                   g_strerror (code));
      return FALSE;
    }

  attr = BOLT_SYSFS_DMI_SYS_VENDOR;
  vendor = sysfs_get_sysattr_value (dmi, attr, error);

  if (vendor == NULL)
    return FALSE;

  /* Almost all systems are using the product_name attribute
   * for the human readable string, with exception of Lenovo,
   * that instead uses the product_version, so we special
   * case that. */
  attr = BOLT_SYSFS_DMI_PRODUCT_NAME;

  if (!g_ascii_strcasecmp (vendor, "lenovo"))
    {
      attr = BOLT_SYSFS_DMI_PRODUCT_VERSION;
      vendor = "Lenovo";
    }

  name = sysfs_get_sysattr_value (dmi, attr, error);

  if (name == NULL)
    return FALSE;

  id->udev = g_steal_pointer (&dmi);
  id->name = name;
  id->vendor = vendor;

  return TRUE;
}

static int
bolt_syfs_count_tb_devices (struct udev        *udev,
                            struct udev_device *parent,
                            GError            **error)
{
  struct udev_enumerate *e;
  struct udev_list_entry *l, *devices;
  int r, count = 0;

  e = udev_enumerate_new (udev);

  udev_enumerate_add_match_subsystem (e, "thunderbolt");
  udev_enumerate_add_match_property (e, "DEVTYPE", "thunderbolt_device");

  if (parent)
    udev_enumerate_add_match_parent (e, parent);

  r = udev_enumerate_scan_devices (e);
  if (r < 0)
    {
      g_set_error (error, BOLT_ERROR, BOLT_ERROR_UDEV,
                   "failed to scan udev: %s",
                   g_strerror (-r));
      return r;
    }

  devices = udev_enumerate_get_list_entry (e);
  udev_list_entry_foreach (l, devices)
    count++;

  udev_enumerate_unref (e);

  return count;
}

int
bolt_sysfs_count_hosts (struct udev *udev,
                        GError     **error)
{
  struct udev_enumerate *e;
  struct udev_list_entry *l, *devices;
  int r, count = 0;

  e = udev_enumerate_new (udev);

  udev_enumerate_add_match_subsystem (e, "thunderbolt");
  udev_enumerate_add_match_property (e, "DEVTYPE", "thunderbolt_domain");

  r = udev_enumerate_scan_devices (e);
  if (r < 0)
    {
      g_set_error (error, BOLT_ERROR, BOLT_ERROR_UDEV,
                   "failed to scan udev: %s",
                   g_strerror (-r));
      return r;
    }

  devices = udev_enumerate_get_list_entry (e);
  udev_list_entry_foreach (l, devices)
    {
      struct udev_device *udevice = NULL;
      const char *syspath;
      int n;

      syspath = udev_list_entry_get_name (l);
      udevice = udev_device_new_from_syspath (udev, syspath);

      if (udevice == NULL)
        continue;

      n = bolt_syfs_count_tb_devices (udev, udevice, NULL);
      if (n > 0)
        count++;

      udev_device_unref (udevice);
    }

  udev_enumerate_unref (e);

  return count;
}

gboolean
bolt_sysfs_nhi_id_for_domain (struct udev_device *udev,
                              guint32            *id,
                              GError            **error)
{
  struct udev_device *parent;
  const char *str;
  gboolean ok;

  ok = bolt_sysfs_device_is_domain (udev, error);
  if (!ok)
    return FALSE;

  parent = udev_device_get_parent (udev);

  if (parent == NULL)
    {
      g_set_error (error, BOLT_ERROR, BOLT_ERROR_UDEV,
                   "failed to get parent for domain: %s",
                   g_strerror (errno));
      return FALSE;
    }

  str = udev_device_get_sysattr_value (parent, "device");
  if (str == NULL)
    {
      g_set_error (error, BOLT_ERROR, BOLT_ERROR_UDEV,
                   "failed to get PCI id for NHI device: %s",
                   g_strerror (errno));
      return FALSE;
    }

  return bolt_str_parse_as_uint32 (str, id, error);
}

static gint
sysfs_get_sysattr_value_as_int (struct udev_device *udev,
                                const char         *attr)
{
  const char *str;
  gboolean ok;
  gint val;

  g_return_val_if_fail (udev != NULL, FALSE);

  str = udev_device_get_sysattr_value (udev, attr);
  if (str == NULL)
    return -errno;

  ok = bolt_str_parse_as_int (str, &val, NULL);

  if (!ok)
    return -errno;

  return val;
}

static gssize
sysfs_get_sysattr_size (struct udev_device *udev,
                        const char         *attr)
{
  const char *str;

  g_return_val_if_fail (udev != NULL, FALSE);

  str = udev_device_get_sysattr_value (udev, attr);
  if (str == NULL)
    return -errno;

  return strlen (str);
}

void
bolt_sysfs_read_link_speed (struct udev_device *udev,
                            BoltLinkSpeed      *speed)
{
  struct
  {
    const char *name;
    guint32    *item;
  } entries[] = {
    {BOLT_SYSFS_RX_LANES, &speed->rx.lanes},
    {BOLT_SYSFS_RX_SPEED, &speed->rx.speed},
    {BOLT_SYSFS_TX_LANES, &speed->tx.lanes},
    {BOLT_SYSFS_TX_SPEED, &speed->tx.speed},
  };

  for (unsigned i = 0; i < G_N_ELEMENTS (entries); i++)
    {
      const char *name = entries[i].name;
      int res = sysfs_get_sysattr_value_as_int (udev, name);

      *entries[i].item = res > 0 ? res : 0;
    }
}

gboolean
bolt_sysfs_info_for_device (struct udev_device *udev,
                            gboolean            full,
                            BoltDevInfo        *info,
                            GError            **error)
{
  struct udev_device *parent;
  int auth;
  int gen;

  g_return_val_if_fail (udev != NULL, FALSE);
  g_return_val_if_fail (info != NULL, FALSE);

  info->keysize = -1;
  info->ctim = -1;
  info->full = FALSE;
  info->parent = NULL;
  info->generation = 0;
  info->syspath = NULL;

  auth = sysfs_get_sysattr_value_as_int (udev, "authorized");
  info->authorized = auth;

  if (auth < 0)
    {
      int code = g_io_error_from_errno (errno);
      g_set_error (error, G_IO_ERROR, code,
                   "could not read 'authorized': %s",
                   g_strerror (errno));
      return FALSE;
    }

  info->keysize = sysfs_get_sysattr_size (udev, "key");
  info->boot = sysfs_get_sysattr_value_as_int (udev, "boot");

  if (full == FALSE)
    return TRUE;

  info->full = TRUE;
  info->ctim = bolt_sysfs_device_get_time (udev, BOLT_ST_CTIME);
  info->syspath = udev_device_get_syspath (udev);

  parent = udev_device_get_parent (udev);

  if (parent != NULL)
    info->parent = udev_device_get_sysattr_value (parent, "unique_id");

  gen = sysfs_get_sysattr_value_as_int (udev, BOLT_SYSFS_GENERATION);
  if (gen > 0)
    info->generation = gen;

  bolt_sysfs_read_link_speed (udev, &info->linkspeed);

  return TRUE;
}

gboolean
bolt_sysfs_read_boot_acl (struct udev_device *udev,
                          GStrv              *out,
                          GError            **error)
{
  g_auto(GStrv) acl = NULL;
  const char *val;

  g_return_val_if_fail (udev != NULL, FALSE);
  g_return_val_if_fail (out != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  val = udev_device_get_sysattr_value (udev, "boot_acl");

  if (val)
    acl = g_strsplit (val, ",", 1024);
  else if (errno != ENOENT)
    return bolt_error_for_errno (error, errno, "%m");

  /* if the attribute exists but is empty, return NULL */
  if (!bolt_strv_isempty (acl))
    *out = g_steal_pointer (&acl);
  else
    *out = NULL;

  return TRUE;
}

gboolean
bolt_sysfs_write_boot_acl (const char *device,
                           GStrv       acl,
                           GError    **error)
{
  g_autofree char *val = NULL;
  g_autofree char *path = NULL;

  g_return_val_if_fail (device != NULL, FALSE);
  g_return_val_if_fail (acl != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  val = g_strjoinv (",", acl);
  path = g_build_filename (device, "boot_acl", NULL);

  return bolt_file_write_all (path, val, -1, error);
}

gboolean
bolt_sysfs_read_iommu (struct udev_device *udev,
                       gboolean           *out,
                       GError            **error)
{
  int val = 0;

  g_return_val_if_fail (udev != NULL, FALSE);
  g_return_val_if_fail (out != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  val = sysfs_get_sysattr_value_as_int (udev, BOLT_SYSFS_IOMMU);
  if (val < 0 && val != -ENOENT)
    return bolt_error_for_errno (error, errno, "failed to read %s: %s",
                                 BOLT_SYSFS_IOMMU, g_strerror (-val));

  *out = val > 0;

  return TRUE;
}

/* NHI PCI id related */
static struct
{
  guint32  pci_id;
  gboolean stable; /* Does the UUID change on reboot */
} nhi_table[] = {
  {0x157d, TRUE},  // WIN_RIDGE_2C_NHI
  {0x15bf, TRUE},  // ALPINE_RIDGE_LP_NHI
  {0x15d2, TRUE},  // ALPINE_RIDGE_C_4C_NHI
  {0x15d9, TRUE},  // ALPINE_RIDGE_C_2C_NHI
  {0x15dc, TRUE},  // ALPINE_RIDGE_LP_USBONLY_NHI
  {0x15dd, TRUE},  // ALPINE_RIDGE_USBONLY_NH
  {0x15de, TRUE},  // ALPINE_RIDGE_C_USBONLY_NHI
  {0x15e8, TRUE},  // TITAN_RIDGE_2C_NHI
  {0x15eb, TRUE},  // TITAN_RIDGE_4C_NHI
  {0x8a0d, FALSE}, // ICL_NHI1
  {0x8a17, FALSE}, // ICL_NHI0
  {0x9a1b, FALSE}, // TGL_NHI0
  {0x9a1d, FALSE}, // TGL_NHI1
};

gboolean
bolt_nhi_uuid_is_stable (guint32   pci_id,
                         gboolean *stability,
                         GError  **error)
{
  g_return_val_if_fail (stability != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  for (gsize i = 0; i < G_N_ELEMENTS (nhi_table); i++)
    {
      if (pci_id == nhi_table[i].pci_id)
        {
          *stability = nhi_table[i].stable;
          return TRUE;
        }
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
               "unknown NHI PCI id '0x%04x'", pci_id);

  return FALSE;
}
