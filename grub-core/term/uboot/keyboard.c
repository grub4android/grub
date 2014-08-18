/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2007,2008,2009  Free Software Foundation, Inc.
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
#include <grub/misc.h>
#include <grub/term.h>
#include <grub/time.h>
#include <grub/loader.h>
#include <grub/uboot/uboot.h>

GRUB_MOD_LICENSE ("GPLv3+");

#define KEY_LEFT	0x110
#define KEY_RIGHT	0x111
#define KEY_UP		0x112
#define KEY_DOWN	0x113
#define KEY_CENTER	0x114

/* If there is a character pending, return it;
   otherwise return GRUB_TERM_NO_KEY.  */
static int
grub_uboot_keyboard_getkey (struct grub_term_input *term __attribute__ ((unused)))
{
  int code = grub_uboot_input_getkey();
  if(code==KEY_UP)
	return GRUB_TERM_KEY_UP;
  if(code==KEY_DOWN)
	return GRUB_TERM_KEY_DOWN;
  if(code==KEY_RIGHT)
	return GRUB_TERM_KEY_RIGHT;

  return GRUB_TERM_NO_KEY;
}

static grub_err_t
grub_keyboard_controller_fini (struct grub_term_input *term __attribute__ ((unused)))
{
  return GRUB_ERR_NONE;
}


static struct grub_term_input grub_uboot_keyboard_term =
  {
    .name = "uboot_keyboard",
    .fini = grub_keyboard_controller_fini,
    .getkey = grub_uboot_keyboard_getkey
  };

GRUB_MOD_INIT(uboot_keyboard)
{
  grub_term_register_input ("uboot_keyboard", &grub_uboot_keyboard_term);
}

GRUB_MOD_FINI(uboot_keyboard)
{
  grub_keyboard_controller_fini (NULL);
  grub_term_unregister_input (&grub_uboot_keyboard_term);
}
