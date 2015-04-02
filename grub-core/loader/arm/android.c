/* android.c - boot Android */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2015  Free Software Foundation, Inc.
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
#include <grub/cpu/atags.h>
#include <grub/linux.h>
#include <grub/android.h>
#include <grub/lib/cpio.h>
#include <grub/lib/cmdline.h>
#include <grub/env.h>

GRUB_MOD_LICENSE ("GPLv3+");

typedef void (*kernel_entry_t) (int, unsigned long, void *);

#define ALIGN(x,ps) (((x) + (ps)-1) & (~((ps)-1)))
#define CMDLINE_GRUBDIR " multiboot.grubdir="
#define CMDLINE_INITRD " rdinit=/init.multiboot"

#define GRUB_EFI_PAGE_SHIFT	12
#define BYTES_TO_PAGES(bytes)   (((bytes) + 0xfff) >> GRUB_EFI_PAGE_SHIFT)
#define PAGES_TO_BYTES(pages)	((pages) << GRUB_EFI_PAGE_SHIFT)

#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

static grub_efi_guid_t global = GRUB_EFI_GLOBAL_VARIABLE_GUID;
static const char *cpio_name_mbinit = "/init.multiboot";
static const char *cpio_name_grubrd = "/grub_ramdisk";

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

static grub_err_t
android_read_grub_file (const char *name, void **buf, grub_size_t * size)
{
  *buf = NULL;
  *size = 0;

  // get prefix
  const char *prefix = grub_env_get ("prefix");
  if (!prefix)
    goto err_out;

  // build filename
  char *fname = grub_xasprintf ("%s/%s", prefix, name);
  if (!fname)
    goto err_out;

  // open file
  grub_file_t f = grub_file_open (fname);
  if (!f)
    goto err_free_filename;

  // allocate buffer
  *size = grub_file_size (f);
  *buf = grub_malloc (*size);
  if (!(*buf))
    goto err_close_file;

  // read file
  if (grub_file_read (f, *buf, *size) != (grub_ssize_t) * size)
    goto free_buf;

  // cleanup
  grub_file_close (f);
  grub_free (fname);

  return GRUB_ERR_NONE;

free_buf:
  grub_free (*buf);
err_close_file:
  grub_file_close (f);
err_free_filename:
  grub_free (fname);
err_out:
  *buf = NULL;
  *size = 0;
  return grub_errno;
}

static grub_err_t
android_read_grub_disk (void **buf, grub_size_t * size)
{
  *buf = NULL;
  *size = 0;

  // if the rootdev is a partition, we assume that it's a ramdisk
  // this would result in unwanted behaviour if a device contains
  // a filesystem only without a partition table.
  const char *rootdev = grub_env_get ("root");
  if (grub_strchr (rootdev, ','))
    goto err_out;

  // skip if this is not a cd (we register ramdisks as cd's)
  if (grub_strncmp (rootdev, "cd", 2))
    goto err_out;

  // open file
  grub_disk_t d = grub_disk_open (rootdev);
  if (!d)
    goto err_out;

  // allocate buffer
  *size = grub_disk_get_size (d) * GRUB_DISK_SECTOR_SIZE;
  *buf = grub_malloc (*size);
  if (!(*buf))
    goto err_close_disk;

  // read file
  if (grub_disk_read (d, 0, 0, *size, *buf))
    goto free_buf;

  // cleanup
  grub_disk_close (d);

  return GRUB_ERR_NONE;

free_buf:
  grub_free (*buf);
err_close_disk:
  grub_disk_close (d);
err_out:
  *buf = NULL;
  *size = 0;
  return grub_errno;
}

static grub_err_t
android_patch_ramdisk (boot_img_hdr * hdr)
{
  char *cpiobuf = NULL;
  grub_size_t cpiosize = 0;
  grub_file_t cpiofile = 0;

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
      goto err_free_buffer;
    }

  // check header
  if (!cpio_is_valid (cpiobuf))
    {
      grub_error (GRUB_ERR_BAD_OS, N_("Invalid Ramdisk format"));
      goto err_free_buffer;
    }

  // load mbinit
  void *mbinit = NULL;
  grub_size_t mbinit_size = 0;
  if (android_read_grub_file ("multiboot/sbin/init", &mbinit, &mbinit_size))
    goto err_free_buffer;

  // load grub ramdisk (if available)
  void *grubrd = NULL;
  grub_size_t grubrd_size = 0;
  if (android_read_grub_disk (&grubrd, &grubrd_size))
    goto err_free_mbinit;

  // calculate new size
  grub_uint32_t newsize = cpiosize;
  if (mbinit && mbinit_size)
    newsize +=
      cpio_predict_obj_size (grub_strlen (cpio_name_mbinit), mbinit_size);
  if (grubrd && grubrd_size)
    newsize +=
      cpio_predict_obj_size (grub_strlen (cpio_name_grubrd), grubrd_size);

  // get highest address in use by bootimg images
  grub_uint32_t addr_max = (hdr->kernel_addr + hdr->kernel_size);
  addr_max = MAX (addr_max, hdr->ramdisk_addr);	// allow to override old ramdisk
  addr_max = MAX (addr_max, hdr->second_addr + hdr->second_size);
  addr_max = MAX (addr_max, hdr->tags_addr + hdr->dt_size);

  // get lowest address in use by bootimg images
  grub_uint32_t addr_min = (hdr->kernel_addr);
  addr_min = MIN (addr_min, hdr->ramdisk_addr);
  addr_min = MIN (addr_min, hdr->second_addr);
  addr_min = MIN (addr_min, hdr->tags_addr);

  // allocate memory
  hdr->ramdisk_addr =
    (grub_uint32_t) grub_efi_allocate_loader_memory (addr_max - addr_min,
						     newsize + 4096);
  if (!hdr->ramdisk_addr)
    goto err_free_grubrd;

  // align memory to page size
  hdr->ramdisk_addr = ALIGN (hdr->ramdisk_addr, 4096);
  if (hdr->ramdisk_addr < addr_max)
    {
      grub_error (GRUB_ERR_BAD_OS, N_("Invalid ramdisk address 0x%x\n"),
		  hdr->ramdisk_addr);
      goto err_free_grubrd;
    }

  // copy old ramdisk to the new location
  grub_memcpy ((void *) hdr->ramdisk_addr, cpiobuf, cpiosize);

  // add our files to the ramdisk
  cpio_newc_header_t *cpiohd = (void *) hdr->ramdisk_addr;
  cpiohd = cpio_get_last (cpiohd);

  if (mbinit && mbinit_size)
    cpiohd = cpio_create_obj (cpiohd, cpio_name_mbinit, mbinit, mbinit_size);
  if (grubrd && grubrd_size)
    cpiohd = cpio_create_obj (cpiohd, cpio_name_grubrd, grubrd, grubrd_size);
  cpiohd = cpio_create_obj (cpiohd, CPIO_TRAILER, NULL, 0);

  // set new ramdisk size
  hdr->ramdisk_size = ((grub_uint32_t) cpiohd) - hdr->ramdisk_addr;

  // cleanup
  if (grubrd && grubrd_size)
    grub_free (grubrd);
  grub_free (mbinit);
  grub_free (cpiobuf);
  grub_file_close (cpiofile);

  return GRUB_ERR_NONE;

err_free_grubrd:
  if (grubrd && grubrd_size)
    grub_free (grubrd);
err_free_mbinit:
  grub_free (mbinit);
err_free_buffer:
  grub_free (cpiobuf);
err_close_cpiofile:
  grub_file_close (cpiofile);
err_out:
  return grub_errno;
}

/*
 * ANDROID LOADING
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

  grub_dprintf
    ("loader",
     "Booting kernel @ %p (%u), ramdisk @ 0x%08x (%u), tags/device tree @ 0x%08x (%u)\n",
     linuxmain, bootinfo.hdr->kernel_size, bootinfo.hdr->ramdisk_addr,
     bootinfo.hdr->ramdisk_size, bootinfo.hdr->tags_addr,
     bootinfo.hdr->dt_size);

  // mach type override
  grub_uint32_t mach_type = grub_arm_firmware_get_machine_type ();
  grub_size_t uefi_machine_type_sz;
  grub_uint32_t *uefi_machine_type =
    grub_efi_get_variable ("android_machine_type", &global,
			   &uefi_machine_type_sz);
  if (uefi_machine_type && uefi_machine_type_sz == sizeof (grub_uint32_t))
    mach_type = *uefi_machine_type;
  if (uefi_machine_type)
    grub_free (uefi_machine_type);

#ifdef GRUB_MACHINE_EFI
  {
    grub_err_t err;
    err = grub_efi_prepare_platform ();
    if (err != GRUB_ERR_NONE)
      return err;
  }
#endif

  grub_arm_disable_caches_mmu ();

  linuxmain (0, mach_type, (void *) bootinfo.hdr->tags_addr);

  return grub_error (GRUB_ERR_BAD_OS, "Linux call returned");
}

static void *
mmap_iteration_cb (void *pdata, grub_efi_physical_address_t addr,
		   grub_efi_uint64_t size)
{
  struct tag *tag = pdata;

  tag = tag_next (tag);
  tag->hdr.tag = ATAG_MEM;
  tag->hdr.size = tag_size (tag_mem32);
  tag->u.mem.size = (grub_uint32_t) size;
  tag->u.mem.start = (grub_uint32_t) addr;

  return tag;
}

static grub_err_t
android_generate_atags (boot_img_hdr * hdr, const char *cmdline)
{
  // CORE
  struct tag *tag = (void *) hdr->tags_addr;
  tag->hdr.tag = ATAG_CORE;
  tag->hdr.size = sizeof (struct tag_header) >> 2;	// we don't have a rootdev

  // initrd
  tag = tag_next (tag);
  tag->hdr.tag = ATAG_INITRD2;
  tag->hdr.size = tag_size (tag_initrd);
  tag->u.initrd.start = hdr->ramdisk_addr;
  tag->u.initrd.size = hdr->ramdisk_size;

  // mmap
  tag = grub_efi_iterate_memory_map (tag, mmap_iteration_cb);

  // cmdline
  tag = tag_next (tag);
  tag->hdr.tag = ATAG_CMDLINE;
  tag->hdr.size = (grub_strlen (cmdline) + 3 +
		   sizeof (struct tag_header)) >> 2;
  grub_strcpy (tag->u.cmdline.cmdline, cmdline);

  // end
  tag = tag_next (tag);
  tag->hdr.tag = ATAG_NONE;
  tag->hdr.size = 0;

  return GRUB_ERR_NONE;
}

static grub_err_t
android_load (struct source *src, int argc, char *argv[], int multiboot)
{
  boot_img_hdr *hdr;
  grub_size_t size_linux_args, size_bootimg_cmdline, size_initrd,
    size_grubdir_key, size_grubdir_val;

  grub_dprintf ("loader", "Loading android\n");

  //
  // parse header
  //

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
  //grub_size_t dt_size = ALIGN (hdr->dt_size, hdr->page_size);

  //
  // update addresses
  //

  int is_arm64 = IS_ARM64 (hdr->kernel_addr + hdr->page_size);

  // kernel
  grub_size_t uefi_kernel_addr_sz;
  grub_uint64_t *uefi_kernel_addr =
    grub_efi_get_variable (is_arm64 ? "android_kernel64_addr" :
			   "android_kernel_addr", &global,
			   &uefi_kernel_addr_sz);
  if (uefi_kernel_addr && uefi_kernel_addr_sz == sizeof (grub_uint64_t))
    hdr->kernel_addr = (grub_uint32_t) * uefi_kernel_addr;
  if (uefi_kernel_addr)
    grub_free (uefi_kernel_addr);

  // ramdisk
  grub_size_t uefi_ramdisk_addr_sz;
  grub_uint64_t *uefi_ramdisk_addr =
    grub_efi_get_variable ("android_ramdisk_addr", &global,
			   &uefi_ramdisk_addr_sz);
  if (uefi_ramdisk_addr && uefi_ramdisk_addr_sz == sizeof (grub_uint64_t))
    hdr->ramdisk_addr = (grub_uint32_t) * uefi_ramdisk_addr;
  if (uefi_ramdisk_addr)
    grub_free (uefi_ramdisk_addr);

  // tags
  grub_size_t uefi_tags_addr_sz;
  grub_uint64_t *uefi_tags_addr =
    grub_efi_get_variable ("android_tags_addr", &global,
			   &uefi_tags_addr_sz);
  if (uefi_tags_addr && uefi_tags_addr_sz == sizeof (grub_uint64_t))
    hdr->tags_addr = (grub_uint32_t) * uefi_tags_addr;
  if (uefi_tags_addr)
    grub_free (uefi_tags_addr);

  //
  // load images
  //

  // allocate kernel memory
  hdr->kernel_addr =
    (grub_addr_t) grub_efi_allocate_pages (hdr->kernel_addr,
					   BYTES_TO_PAGES (hdr->kernel_size));
  if (!hdr->kernel_addr)
    return grub_errno;

  // load kernel
  grub_off_t offset = hdr->page_size;
  if (src->read (src, offset, kernel_size, (void *) hdr->kernel_addr))
    goto err_free_hdr;

  if (ramdisk_size > 0)
    {
      // allocate ramdisk memory
      hdr->ramdisk_addr =
	(grub_addr_t) grub_efi_allocate_pages (hdr->ramdisk_addr,
					       BYTES_TO_PAGES
					       (hdr->ramdisk_size));
      if (!hdr->ramdisk_addr)
	return grub_errno;

      // load ramdisk
      offset += kernel_size;
      if (src->read (src, offset, ramdisk_size, (void *) hdr->ramdisk_addr))
	goto err_free_hdr;

      // patch ramdisk
      if (multiboot && android_patch_ramdisk (hdr))
	goto err_remove_bootinfo;
    }

  // load second
  offset += ramdisk_size;
  if (second_size > 0)
    {
      // allocate second memory
      hdr->second_addr =
	(grub_addr_t) grub_efi_allocate_pages (hdr->second_addr,
					       BYTES_TO_PAGES
					       (hdr->second_size));
      if (!hdr->second_addr)
	return grub_errno;

      if (src->read (src, offset, second_size, (void *) hdr->second_addr))
	goto err_free_hdr;
    }

  // load devicetree
  offset += second_size;
#if 0
  if (dt_size > 0)
    {
      info.dt = grub_malloc (dt_size);
      info.dt_size = dt_size;
      if (src->read (src, offset, dt_size, info.dt))
	goto err_remove_bootinfo;
    }
#endif

  // set bootinfo
  bootinfo.hdr = hdr;

  //
  // calculate cmdline size
  //

  // basic cmdline(bootimg + args)
  size_linux_args = grub_loader_cmdline_size (argc, argv);
  size_bootimg_cmdline = grub_strlen ((const char *) hdr->cmdline);
  grub_size_t cmdline_size = size_bootimg_cmdline + size_linux_args;

  // uefi cmdline
  grub_size_t uefi_cmdline_sz;
  char *uefi_cmdline =
    grub_efi_get_variable ("android_additional_cmdline", &global,
			   &uefi_cmdline_sz);
  if (uefi_cmdline && uefi_cmdline_sz)
    {
      cmdline_size += uefi_cmdline_sz + 2;
    }

  // initrd
  if (multiboot)
    {
      size_initrd = grub_strlen (CMDLINE_INITRD);
      cmdline_size += size_initrd;
    }

  // grubdir
  const char *grubdir_val = grub_env_get ("cmdpath");
  size_grubdir_key = grub_strlen (CMDLINE_GRUBDIR);
  size_grubdir_val = grub_strlen (grubdir_val);
  cmdline_size += size_grubdir_key + size_grubdir_val;

  //
  // allocate memory for cmdline
  //

  linux_args = grub_malloc (cmdline_size + 1);
  if (!linux_args)
    goto err_remove_bootinfo;

  //
  // create cmdline
  //

  // bootimg args
  int cmdline_pos = 0;
  grub_memcpy (linux_args + cmdline_pos, hdr->cmdline, size_bootimg_cmdline);
  cmdline_pos += size_bootimg_cmdline;

  // uefi
  if (uefi_cmdline && uefi_cmdline_sz)
    {
      linux_args[cmdline_pos++] = ' ';
      grub_memcpy (linux_args + cmdline_pos, uefi_cmdline,
		   uefi_cmdline_sz - 1);
      cmdline_pos += uefi_cmdline_sz - 1;
      linux_args[cmdline_pos++] = ' ';
    }

  // linux args
  grub_create_loader_cmdline (argc, argv,
			      linux_args + cmdline_pos, size_linux_args);
  cmdline_pos += size_linux_args - 1;

  // initrd
  if (multiboot)
    {
      grub_memcpy (linux_args + cmdline_pos, CMDLINE_INITRD, size_initrd);
      cmdline_pos += size_initrd;
    }

  // grubdir
  grub_memcpy (linux_args + cmdline_pos, CMDLINE_GRUBDIR, size_grubdir_key);
  cmdline_pos += size_grubdir_key;
  grub_memcpy (linux_args + cmdline_pos, grubdir_val, size_grubdir_val);
  cmdline_pos += size_grubdir_val;

  // terminate
  linux_args[cmdline_pos] = '\0';

  //
  // generate tags
  //
  if (hdr->dt_size > 0)
    {
      grub_error (GRUB_ERR_BUG, N_("DT is not implemented."));
      goto err_remove_bootinfo;
    }
  else
    {
      android_generate_atags (hdr, linux_args);
    }

  return GRUB_ERR_NONE;

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
grub_cmd_android (grub_command_t cmd, int argc, char *argv[])
{
  grub_dl_ref (my_mod);
  struct source src;
  int namelen;

  if (argc == 0)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));

  namelen = grub_strlen (argv[0]);
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
  if (android_load
      (&src, argc, argv, !grub_strcmp (cmd->name, "android.multiboot")))
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

static grub_command_t cmd_android, cmd_android_multiboot;

GRUB_MOD_INIT (android)
{
  cmd_android = grub_register_command ("android", grub_cmd_android,
				       0, N_("Boot Android Image."));

  cmd_android_multiboot =
    grub_register_command ("android.multiboot", grub_cmd_android, 0,
			   N_("Boot Android Image in multiboot mode."));
  my_mod = mod;
}

GRUB_MOD_FINI (android)
{
  grub_unregister_command (cmd_android);
}
