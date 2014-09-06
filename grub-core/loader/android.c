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
#include <grub/lib/cpio.h>
#include <grub/lib/cmdline.h>

GRUB_MOD_LICENSE ("GPLv3+");

#define ALIGN(x,ps) (((x) + (ps)-1) & (~((ps)-1)))
#define CPIO_MAX_FILES 2048
#define MAX_RAMDISK_SIZE 20*1024*1024
#define INITRD " rdinit=/multiboot/init "
typedef void (*kernel_entry_t) (int, unsigned long, void *);

static grub_dl_t my_mod;
static char *linux_args;

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

struct multiboot_file
{
  void *data;
  char *name;
  grub_size_t size;
};

struct multiboot_filelist
{
  int num_files;
  struct multiboot_file *files;
  const char *prefix;
};

static int
android_multiboot_iterate_dir (const char *filename,
			       const struct grub_dirhook_info *info
			       __attribute__ ((unused)), void *data)
{
  struct multiboot_filelist *mblist = (struct multiboot_filelist *) data;
  if (info->dir)
    goto out;
  char *fullname;
  int namesize = grub_strlen (mblist->prefix) + grub_strlen (filename) + 1;
  grub_file_t file;

  // get full filename
  fullname = grub_malloc (namesize);
  grub_snprintf (fullname, namesize, "%s%s", mblist->prefix, filename);

  // open file
  file = grub_file_open (fullname);
  if (!file)
    {
      grub_printf ("Couldn't open %s\n", fullname);
      goto err_free_fullname;

    }

  // resize list
  mblist->files =
    grub_realloc (mblist->files,
		  ++mblist->num_files * sizeof (struct multiboot_file));
  struct multiboot_file *mbfile = &mblist->files[mblist->num_files - 1];
  mbfile->name = grub_strdup (filename);
  mbfile->size = grub_file_size (file);
  mbfile->data = grub_malloc (mbfile->size);
  if (!mbfile->data)
    {
      grub_printf ("Couldn't malloc %u bytes for %s\n", mbfile->size,
		   fullname);
      goto err_close_file;
    }

  // read file into buffer
  if (grub_file_read (file, mbfile->data, mbfile->size) !=
      (grub_ssize_t) mbfile->size)
    {
      if (!grub_errno)
	grub_error (GRUB_ERR_BAD_OS, N_("premature end of file %s"),
		    fullname);
      goto err_free_fullname;
    }

out:
  return 0;

err_close_file:
  grub_file_close (file);
err_free_fullname:
  grub_free (fullname);

  return grub_errno;
}

static grub_err_t
android_add_multiboot_files (struct multiboot_filelist *mblist)
{
  grub_fs_t fs;
  grub_device_t dev;

  // open root device
  dev = grub_device_open (NULL);
  if (!dev)
    {
      goto fail;
    }

  // get filesystem
  fs = grub_fs_probe (dev);
  if (!fs)
    {
      goto fail;
    }

  // add all files
  mblist->num_files = 0;
  mblist->files = NULL;
  mblist->prefix = "/boot/grub/multiboot/";
  if (fs->dir (dev, mblist->prefix, android_multiboot_iterate_dir, mblist))
    goto fail;

  return GRUB_ERR_NONE;

fail:
  return grub_errno;
}

static grub_err_t
android_patch_ramdisk (boot_img_hdr * hdr)
{
  static const char *mbdir = "multiboot";
  char *cpiobuf = NULL;
  int i;
  unsigned long x;
  grub_size_t cpiosize = 0;
  grub_file_t cpiofile = 0;
  struct multiboot_filelist mblist;

  // open file
  cpiofile =
    grub_memfile_open ((void *) hdr->ramdisk_addr, hdr->ramdisk_size);
  if (!cpiofile)
    goto err_out;
  cpiosize = grub_file_size (cpiofile);

  // create buffer
  cpiobuf = grub_malloc (cpiosize);
  if (!cpiobuf)
    goto err_close_cpiofile;

  // read file into buffer
  if (grub_file_read (cpiofile, cpiobuf, cpiosize) != (grub_ssize_t) cpiosize)
    {
      if (!grub_errno)
	grub_error (GRUB_ERR_BAD_OS, N_("premature end of ramdisk file"));
      goto err_free_buffer;
    }

  // parse CPIO
  unsigned long num = cpiosize;
  CPIO_OBJ cobjs[CPIO_MAX_FILES];
  if (cpio_load (cpiobuf, cobjs, &num))
    goto err_free_buffer;

  // load multiboot files
  if (android_add_multiboot_files (&mblist))
    goto err_free_buffer;

  // create folder
  x = num++;
  android_cpio_make_directory (&cobjs[x]);
  cobjs[x].name = grub_strdup (mbdir);
  cobjs[x].namesize = grub_strlen (cobjs[x].name) + 1;
  cobjs[x].data = NULL;
  cobjs[x].filesize = 0;
  cobjs[x].ignore = 0;

  // copy all files into the new ramdisk
  for (i = 0; i < mblist.num_files; i++)
    {
      char *path;
      int pathlen;
      struct multiboot_file *mbfile = &mblist.files[i];

      // create new file
      x = num++;
      android_cpio_make_executable_file (&cobjs[x]);

      // get full filename
      pathlen = grub_strlen (mbdir) + grub_strlen (mbfile->name) + 2;
      path = grub_malloc (pathlen);
      grub_snprintf (path, pathlen, "%s/%s", mbdir, mbfile->name);

      // set file name and contents
      cobjs[x].name = path;
      cobjs[x].namesize = grub_strlen (cobjs[x].name) + 1;
      cobjs[x].data = mbfile->data;
      cobjs[x].filesize = mbfile->size;
      cobjs[x].ignore = 0;
    }

  // create new ramdisk
  unsigned int size = MAX_RAMDISK_SIZE;
  if (cpio_write (cobjs, num, (void *) hdr->ramdisk_addr, &size))
    goto err_free_buffer;
  hdr->ramdisk_size = size;

  // cleanup
  grub_free (cpiobuf);
  grub_file_close (cpiofile);

  return GRUB_ERR_NONE;

err_free_buffer:
  grub_free (cpiobuf);
err_close_cpiofile:
  grub_file_close (cpiofile);
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
android_load (struct source *src
	      __attribute__ ((unused)), int argc, char *argv[])
{
  boot_img_hdr *hdr;
  struct tags_info info;
  int size_linux_args, size_bootimg_cmdline;

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

  // update addresses
  int is_arm64 = IS_ARM64 (hdr->kernel_addr + hdr->page_size);
  grub_uboot_boot_update_addresses (hdr, is_arm64);

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

  // allocate memory for cmdline
  size_linux_args = grub_loader_cmdline_size (argc, argv);
  size_bootimg_cmdline = grub_strlen ((const char *) hdr->cmdline);
  linux_args =
    grub_malloc (size_bootimg_cmdline + sizeof (INITRD) + size_linux_args);
  if (!linux_args)
    goto err_free_dt;

  // create cmdline
  grub_memcpy (linux_args, hdr->cmdline, size_bootimg_cmdline);
  grub_memcpy (linux_args + size_bootimg_cmdline, INITRD, sizeof (INITRD));
  grub_create_loader_cmdline (argc, argv,
			      linux_args + size_bootimg_cmdline +
			      sizeof (INITRD) - 1, size_linux_args);

  // create tags
  info.tags_addr = (void *) hdr->tags_addr;
  info.cmdline = (const char *) linux_args;
  info.ramdisk = (void *) hdr->ramdisk_addr;
  info.ramdisk_size = hdr->ramdisk_size;
  info.page_size = hdr->page_size;
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
  argc--;
  argv++;
  if (android_load (&src, argc, argv))
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
