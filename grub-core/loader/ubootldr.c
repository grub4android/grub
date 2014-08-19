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
#include <grub/uboot/api_public.h>
#include <grub/uboot/uboot.h>

GRUB_MOD_LICENSE ("GPLv3+");

static grub_dl_t my_mod;
struct boot_request request;

static grub_err_t
ubootldr_boot (void)
{
  grub_uboot_boot_file(&request);
  return grub_error (GRUB_ERR_BAD_OS, "UBOOT boot request failed");
}

static grub_err_t
ubootldr_load (const char *filename, grub_file_t file)
{
  // setup request
  request.size = grub_file_size (file);
  request.data = grub_uboot_boot_get_ldr_addr();
  if(!request.data )
    return grub_error (GRUB_ERR_IO, "Couldn't get addr from uboot");

  // read file into RAM
  if (grub_file_read (file, request.data, request.size) != request.size)
    {
      if (!grub_errno)
        grub_error (GRUB_ERR_BAD_OS, N_("premature end of file %s"),
		    filename);
      return grub_errno;
    }

  return GRUB_ERR_NONE;
}

static grub_err_t
ubootldr_unload (void)
{
  grub_dl_unref (my_mod);

  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_ubootldr (grub_command_t cmd __attribute__ ((unused)),
		int argc, char *argv[])
{
  grub_err_t err;
  grub_file_t file;
  grub_dl_ref (my_mod);

  if (argc == 0)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));

  // open file
  file = grub_file_open (argv[0]);
  if (!file)
    goto fail;

  // load file
  err = ubootldr_load (argv[0], file);
  grub_file_close (file);
  if (err)
    goto fail;

  // set loader
  grub_loader_set (ubootldr_boot, ubootldr_unload, 0);

  return GRUB_ERR_NONE;

fail:
  grub_dl_unref (my_mod);
  return grub_errno;
}

static grub_command_t cmd_ubootldr;

GRUB_MOD_INIT (ubootldr)
{
  cmd_ubootldr = grub_register_command ("ubootldr", grub_cmd_ubootldr,
				     0, N_("Boot Image via UBOOT API."));
  my_mod = mod;
}

GRUB_MOD_FINI (ubootldr)
{
  grub_unregister_command (cmd_ubootldr);
}
