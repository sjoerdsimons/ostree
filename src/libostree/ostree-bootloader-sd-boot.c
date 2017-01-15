/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Collabora Ltd
 *
 * Based on ot-bootloader-syslinux.c by Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Javier Martinez Canillas <javier.martinez@collabora.co.uk>
 */

#include "config.h"

#include "ostree-sysroot-private.h"
#include "ostree-bootloader-sd-boot.h"
#include "otutil.h"

#include <string.h>

struct _OstreeBootloaderSdBoot
{
  GObject       parent_instance;

  OstreeSysroot  *sysroot;
  GFile          *config_path;
};

typedef GObjectClass OstreeBootloaderSdBootClass;

static void _ostree_bootloader_sd_boot_bootloader_iface_init (OstreeBootloaderInterface *iface);
G_DEFINE_TYPE_WITH_CODE (OstreeBootloaderSdBoot, _ostree_bootloader_sd_boot, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_BOOTLOADER, _ostree_bootloader_sd_boot_bootloader_iface_init));

static gboolean
_ostree_bootloader_sd_boot_query (OstreeBootloader *bootloader,
                                gboolean         *out_is_active,
                                GCancellable     *cancellable,
                                GError          **error) 
{
  OstreeBootloaderSdBoot *self = OSTREE_BOOTLOADER_SD_BOOT (bootloader);

  *out_is_active = g_file_query_file_type (self->config_path, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_DIRECTORY;
  return TRUE;
}

static const char *
_ostree_bootloader_sd_boot_get_name (OstreeBootloader *bootloader)
{
  return "sd-boot";
}

static gboolean
clean_bootversion (OstreeBootloaderSdBoot *self,
                   int                     bootversion,
                   GCancellable           *cancellable,
                   GError                **error)
{
  int fd;
  g_autofree gchar *prefix = g_strdup_printf ("ostree-%d-", bootversion);
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

  if (!glnx_opendirat (-1,
                     g_file_get_path(self->config_path),
                     TRUE,
                     &fd, error))
    return FALSE;

  if (!glnx_dirfd_iterator_init_take_fd (fd, &dfd_iter, error))
    return FALSE;

  for (;;)
    {
      struct dirent *d;

      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &d, cancellable, error))
        return FALSE;

      if (d == NULL)
        break;

      if (g_str_has_prefix (d->d_name, prefix)
          && g_str_has_suffix (d->d_name, ".conf"))
        {
          /* Ignore failure, not much we can do about it */
          if (unlinkat(fd, d->d_name, 0) != 0)
            g_warning("Couldn't unlink %s", d->d_name);
        }
    }

  return TRUE;
}

static gboolean
clean_bootdata (OstreeBootloaderSdBoot *self,
                GHashTable             *active_bootdata,
                GCancellable           *cancellable,
                GError                **error)
{
  int fd;
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

  if (!glnx_opendirat (ostree_sysroot_get_fd (self->sysroot),
                       "boot/efi/ostree",
                       TRUE,
                       &fd, error))
    return FALSE;

  if (!glnx_dirfd_iterator_init_take_fd (fd, &dfd_iter, error))
    return FALSE;

  for (;;)
    {
      struct dirent *d;

      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &d, cancellable, error))
        return FALSE;

      if (d == NULL)
        break;

      if (!g_hash_table_contains (active_bootdata, d->d_name))
        {
          /* Ignore failure, not much we can do about it */
          if (!glnx_shutil_rm_rf_at (fd, d->d_name, cancellable, NULL))
            g_warning("Couldn't rm -rf %s", d->d_name);
        }
    }

  return TRUE;
}

static gboolean
do_copy_if_needed (const char   *relpath,
                   int           bootfd,
                   int           efifd,
                   GCancellable *cancellable,
                   GError      **error)
{
  g_autofree gchar *dirname = NULL;
  struct stat stbuf;
  int ret;

  if (!relpath)
    return TRUE;

  relpath = relpath + (relpath[0] == '/' ? 1 : 0);
  dirname = g_path_get_dirname (relpath);

  ret = fstatat (efifd, relpath, &stbuf, 0);
  if (ret == 0)
    return TRUE;

  if (errno != ENOENT)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  if (!glnx_shutil_mkdir_p_at (efifd, dirname, 0775, cancellable, error))
    return FALSE;

  return glnx_file_copy_at (bootfd, relpath, NULL,
                            efifd, relpath,
                            0,
                            cancellable, error);
}


static gboolean
deploy_boot_data (OstreeBootloaderSdBoot *self,
                  OstreeBootconfigParser *config,
                  GCancellable           *cancellable,
                  GError                **error)
{
  const char *li = ostree_bootconfig_parser_get (config, "linux");
  const char *initrd = ostree_bootconfig_parser_get (config, "initrd");
  int bootfd = -1, efifd = -1;
  gboolean ret = TRUE;

  if (!glnx_opendirat (ostree_sysroot_get_fd (self->sysroot),
                       "boot",
                       TRUE,
                       &bootfd,
                       error))
    return FALSE;

  if (!glnx_opendirat (ostree_sysroot_get_fd (self->sysroot),
                       "boot/efi",
                       TRUE,
                       &efifd,
                       error))
    goto out;


  if (!do_copy_if_needed (li, bootfd, efifd, cancellable, error))
    {
      ret = FALSE;
      goto out;
    }

  if (!do_copy_if_needed (initrd, bootfd, efifd, cancellable, error))
    {
      ret = FALSE;
      goto out;
    }

out:
  if (bootfd < 0)
    close (bootfd);

  if (efifd < 0)
    close (efifd);

  return ret;
}

static gboolean
write_out_boot_config (OstreeBootloaderSdBoot *self,
                       OstreeBootconfigParser *config,
                       int                     bootversion,
                       GCancellable           *cancellable,
                       GError                **error)
{
  const gchar *version = ostree_bootconfig_parser_get (config, "version");
  gboolean ret;
  g_autofree gchar *name = NULL;
  int fd;

  /* Can't get the osname at this point unfortunately */
  name = g_strdup_printf ("ostree-%d-%s.conf", bootversion, version);

  if (!glnx_opendirat (-1,
                       g_file_get_path(self->config_path),
                       TRUE,
                       &fd, error))
      return FALSE;

  ret = ostree_bootconfig_parser_write_at(config, fd, name, cancellable, error);
  close(fd);

  return ret;
}

static gboolean
_ostree_bootloader_sd_boot_write_config (OstreeBootloader          *bootloader,
                                  int                    bootversion,
                                  GCancellable          *cancellable,
                                  GError               **error)
{
  OstreeBootloaderSdBoot *self = OSTREE_BOOTLOADER_SD_BOOT (bootloader);
  g_autoptr(GPtrArray) boot_loader_configs = NULL;
  g_autoptr(GHashTable) active_bootdata = NULL;
  int i;

  active_bootdata = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /* Clean out stale entries for the bootversion we're deploying now */
  if (!clean_bootversion (self, bootversion, cancellable, error))
    return FALSE;

  if (!_ostree_sysroot_read_boot_loader_configs (self->sysroot, bootversion, &boot_loader_configs,
                                                 cancellable, error))
    return FALSE;

  for (i = 0; i < boot_loader_configs->len; i++)
    {
      OstreeBootconfigParser *config = boot_loader_configs->pdata[i];
      const char *li = ostree_bootconfig_parser_get (config, "linux");
      g_autofree gchar *dirname = g_path_get_dirname (li);
      gchar *base = g_path_get_basename (dirname);

      g_hash_table_replace (active_bootdata, base, base);

      if (!deploy_boot_data (self, config, cancellable, error))
        return FALSE;

      if (!write_out_boot_config (self, config, bootversion, cancellable, error))
        return FALSE;
    }

  /* clean out current bootversion */
  if (!clean_bootversion(self, bootversion == 0 ? 1 : 0, cancellable, error))
    return FALSE;

  if (!clean_bootdata(self, active_bootdata, cancellable, error))
    return FALSE;

  return TRUE;
}

static void
_ostree_bootloader_sd_boot_finalize (GObject *object)
{
  OstreeBootloaderSdBoot *self = OSTREE_BOOTLOADER_SD_BOOT (object);

  g_clear_object (&self->sysroot);
  g_clear_object (&self->config_path);

  G_OBJECT_CLASS (_ostree_bootloader_sd_boot_parent_class)->finalize (object);
}

void
_ostree_bootloader_sd_boot_init (OstreeBootloaderSdBoot *self)
{
}

static void
_ostree_bootloader_sd_boot_bootloader_iface_init (OstreeBootloaderInterface *iface)
{
  iface->query = _ostree_bootloader_sd_boot_query;
  iface->get_name = _ostree_bootloader_sd_boot_get_name;
  iface->write_config = _ostree_bootloader_sd_boot_write_config;
}

void
_ostree_bootloader_sd_boot_class_init (OstreeBootloaderSdBootClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = _ostree_bootloader_sd_boot_finalize;
}

OstreeBootloaderSdBoot *
_ostree_bootloader_sd_boot_new (OstreeSysroot *sysroot)
{
  OstreeBootloaderSdBoot *self = g_object_new (OSTREE_TYPE_BOOTLOADER_SD_BOOT, NULL);
  self->sysroot = g_object_ref (sysroot);
  self->config_path = g_file_resolve_relative_path (self->sysroot->path, "boot/efi/loader/entries");
  return self;
}
