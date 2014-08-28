/* linux.c - boot Linux */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2013  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/dl.h>
#include <grub/file.h>
#include <grub/loader.h>
#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/command.h>
#include <grub/cache.h>
#include <grub/cpu/linux.h>
#include <grub/linux.h>
#include <grub/android.h>

GRUB_MOD_LICENSE ("GPLv3+");

#define ALIGN(x,ps) (((x) + (ps)-1) & (~((ps)-1)))
typedef void (*kernel_entry_t) (int, unsigned long, void *);

static grub_dl_t my_mod;

static struct boot_info
{
  boot_img_hdr *hdr;
} bootinfo;

/*
 * SOURCE ABSTRACTION
 */

struct source;
typedef grub_err_t (source_read) (struct source * src, grub_off_t offset,
				  grub_size_t len, void *buf);
typedef grub_err_t (source_free) (struct source * src);
struct source
{
  source_read *read;
  source_free *free;
  grub_size_t size;
  void *priv;
};

static grub_err_t
disk_read (struct source *src, grub_off_t offset, grub_size_t len, void *buf)
{
  grub_disk_t disk = (grub_disk_t) src->priv;

  if (grub_disk_read (disk, 0, offset, len, buf))
    {
      if (!grub_errno)
	grub_error (GRUB_ERR_BAD_OS, N_("premature end of disk"));

      return grub_errno;
    }

  return GRUB_ERR_NONE;
}

static grub_err_t
disk_free (struct source *src)
{
  grub_disk_t disk = (grub_disk_t) src->priv;
  grub_disk_close (disk);
  return GRUB_ERR_NONE;
}

static grub_err_t
file_read (struct source *src, grub_off_t offset, grub_size_t len, void *buf)
{
  grub_file_t file = (grub_file_t) src->priv;
  grub_file_seek (file, offset);

  if (grub_file_read (file, buf, len) != (grub_ssize_t) len)
    {
      if (!grub_errno)
	grub_error (GRUB_ERR_BAD_OS, N_("premature end of file"));
      return grub_errno;
    }

  return GRUB_ERR_NONE;
}

static grub_err_t
file_free (struct source *src)
{
  grub_file_t file = (grub_file_t) src->priv;
  grub_file_close (file);
  return GRUB_ERR_NONE;
}

/*
 * RAMDISK PATCHING
 */

static grub_size_t
ramdisk_get_uncompressed_size (void *addr, grub_size_t sz)
{
  unsigned *ptr = (unsigned *) (void *) ((((char *) addr) + sz) - 4);
  return (grub_size_t) * ptr;
}

static grub_err_t
android_patch_ramdisk (boot_img_hdr * hdr)
{
  char *cpiobuf = NULL;
  grub_size_t cpiosize =
    ramdisk_get_uncompressed_size ((void *) hdr->ramdisk_addr,
				   hdr->ramdisk_size);

  // create buffer
  cpiobuf = grub_malloc (cpiosize);
  if (!cpiobuf)
    goto err_out;

  // read file into buffer
  unsigned int real_size = (unsigned int) hdr->ramdisk_size;
  if (grub_uboot_tool_gunzip
      (cpiobuf, cpiosize, (void *) hdr->ramdisk_addr, &real_size)
      || real_size != cpiosize)
    {
      grub_error (GRUB_ERR_BAD_OS, N_("premature end of ramdisk file"));
      goto err_free_buffer;
    }

  // copy back
  grub_memcpy ((void *) hdr->ramdisk_addr, cpiobuf, cpiosize);
  hdr->ramdisk_size = cpiosize;

  // cleanup
  grub_free (cpiobuf);

  return GRUB_ERR_NONE;

err_free_buffer:
  grub_free (cpiobuf);
err_out:
  return grub_errno;
}

/*
 * ANDROID STUFF
 */
static grub_err_t
android_boot (void)
{
  kernel_entry_t linuxmain;

  if (!bootinfo.hdr)
    return grub_error (GRUB_ERR_BUG, "Invalid boot header");

  if (bootinfo.hdr->second_size > 0)
    linuxmain = (kernel_entry_t) bootinfo.hdr->second_addr;
  else
    linuxmain = (kernel_entry_t) bootinfo.hdr->kernel_addr;

  grub_arm_disable_caches_mmu ();
  grub_uboot_boot_prepare ();
  grub_printf
    ("Booting kernel @ %p, ramdisk @ 0x%08x (%d), tags/device tree @ 0x%08x\n",
     linuxmain, bootinfo.hdr->ramdisk_addr, bootinfo.hdr->ramdisk_size,
     bootinfo.hdr->tags_addr);
  linuxmain (0, grub_uboot_get_machine_type (),
	     (void *) bootinfo.hdr->tags_addr);

  return grub_error (GRUB_ERR_BAD_OS, "Linux call returned");
}

static grub_err_t
android_load (struct source *src __attribute__ ((unused)))
{
  boot_img_hdr *hdr;
  struct tags_info info;

  // init
  memset (&info, 0, sizeof (info));

  // read header
  hdr = (boot_img_hdr *) grub_malloc (sizeof (boot_img_hdr) + 100);
  if (!hdr)
    goto err_out;
  if (src->read (src, 0, sizeof (boot_img_hdr) + 100, hdr))
    goto err_free_hdr;

  // check magic
  if (grub_memcmp (hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE))
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, N_("Invalid magic in boot header"));
      goto err_free_hdr;
    }

  // get image sizes
  grub_size_t kernel_size = ALIGN (hdr->kernel_size, hdr->page_size);
  grub_size_t ramdisk_size = ALIGN (hdr->ramdisk_size, hdr->page_size);
  grub_size_t second_size = ALIGN (hdr->second_size, hdr->page_size);
  grub_size_t dt_size = ALIGN (hdr->dt_size, hdr->page_size);

  // load kernel
  grub_off_t offset = hdr->page_size;
  if (src->read (src, offset, kernel_size, (void *) hdr->kernel_addr))
    goto err_free_hdr;

  // load ramdisk
  offset += kernel_size;
  if (src->read (src, offset, ramdisk_size, (void *) hdr->ramdisk_addr))
    goto err_free_hdr;

  // load second
  offset += ramdisk_size;
  if (second_size > 0)
    {
      if (src->read (src, offset, second_size, (void *) hdr->second_addr))
	goto err_free_hdr;
    }

  // set bootinfo
  bootinfo.hdr = hdr;

  // load devicetree
  offset += second_size;
  if (dt_size > 0)
    {
      info.dt = grub_malloc (dt_size);
      info.dt_size = dt_size;
      if (src->read (src, offset, dt_size, info.dt))
	goto err_remove_bootinfo;
    }

  // patch ramdisk
  if (android_patch_ramdisk (hdr))
    goto err_free_dt;

  // create tags
  info.tags_addr = (void *) hdr->tags_addr;
  info.cmdline = (const char *) hdr->cmdline;
  info.ramdisk = (void *) hdr->ramdisk_addr;
  info.ramdisk_size = hdr->ramdisk_size;
  if (grub_uboot_boot_create_tags (&info))
    {
      grub_error (GRUB_ERR_BUG, N_("Could not create tags."));
      goto err_free_dt;
    }


  if (info.dt)
    grub_free (info.dt);
  return GRUB_ERR_NONE;

err_free_dt:
  if (info.dt)
    grub_free (info.dt);
err_remove_bootinfo:
  bootinfo.hdr = NULL;
err_free_hdr:
  grub_free (hdr);
err_out:
  if (!grub_errno)
    grub_error (GRUB_ERR_BUG, N_("%s: Unknown error."), __func__);
  return grub_errno;
}

static grub_err_t
android_unload (void)
{
  grub_dl_unref (my_mod);

  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_android (grub_command_t cmd __attribute__ ((unused)),
		  int argc, char *argv[])
{
  grub_dl_ref (my_mod);
  struct source src;
  int namelen = grub_strlen (argv[0]);

  if (argc == 0)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));

  if ((argv[0][0] == '(') && (argv[0][namelen - 1] == ')'))
    {
      // open disk
      argv[0][namelen - 1] = 0;
      grub_disk_t disk = grub_disk_open (&argv[0][1]);
      if (!disk)
	goto dl_unref;

      src.read = &disk_read;
      src.free = &disk_free;
      src.size = grub_disk_get_size (disk) * GRUB_DISK_SECTOR_SIZE;
      src.priv = disk;
    }
  else
    {
      // open file
      grub_file_t file = grub_file_open (argv[0]);
      if (!file)
	goto dl_unref;

      src.read = &file_read;
      src.free = &file_free;
      src.size = grub_file_size (file);
      src.priv = file;
    }

  // load android image
  if (android_load (&src))
    goto source_free;

  // close source
  src.free (&src);

  // set loader
  grub_loader_set (android_boot, android_unload, 0);

  return GRUB_ERR_NONE;

source_free:
  src.free (&src);
dl_unref:
  grub_dl_unref (my_mod);
  if (!grub_errno)
    grub_error (GRUB_ERR_BUG, N_("%s: Unknown error."), __func__);
  return grub_errno;
}

static grub_command_t cmd_android;

GRUB_MOD_INIT (android)
{
  cmd_android = grub_register_command ("android", grub_cmd_android,
				       0, N_("Boot Android Image."));
  my_mod = mod;
}

GRUB_MOD_FINI (android)
{
  grub_unregister_command (cmd_android);
}
