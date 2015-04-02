/* hexdump.c - hexdump function */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2008,2009  Free Software Foundation, Inc.
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

#include <grub/types.h>
#include <grub/misc.h>
#include <grub/lib/cpio.h>
#include <grub/mm.h>

#define ALIGN(x,ps) (((x) + (ps)-1) & (~((ps)-1)))

static const char *cpio_mode_executable = "000081e8";

grub_uint32_t
cpio_strtoul (char *in)
{
  char buf[9];
  grub_memcpy (buf, in, 8);
  buf[8] = 0;

  return grub_strtoul (buf, NULL, 16);
}

void
cpio_ul2ostr (char *buf, grub_uint32_t in)
{
  grub_snprintf (buf, 9, "%08x", in);
}

int
cpio_is_valid (void *ptr)
{
  return !grub_strncmp (ptr, CPIO_NEWC_MAGIC, 6);
}

int
cpio_has_next (cpio_newc_header_t * hdr)
{
  return ! !grub_strcmp ((const char *) (hdr + 1), CPIO_TRAILER);
}

grub_size_t
cpio_predict_obj_size (grub_uint32_t namesize, grub_uint32_t filesize)
{
  return ALIGN (sizeof (cpio_newc_header_t) + namesize, 4) + ALIGN (filesize,
								    4);
}

grub_size_t
cpio_get_obj_size (cpio_newc_header_t * hdr)
{
  grub_uint32_t namesize = cpio_strtoul (hdr->c_namesize);
  grub_uint32_t filesize = cpio_strtoul (hdr->c_filesize);

  return cpio_predict_obj_size (namesize, filesize);
}

cpio_newc_header_t *
cpio_get_last (cpio_newc_header_t * hdr)
{
  while (cpio_is_valid (hdr) && cpio_has_next (hdr))
    hdr = (cpio_newc_header_t *) (((char *) hdr) + cpio_get_obj_size (hdr));

  return hdr;
}

cpio_newc_header_t *
cpio_create_obj (cpio_newc_header_t * hdr, const char *name, const void *data,
		 grub_size_t data_size)
{
  grub_uint32_t namesize = grub_strlen (name) + 1;
  grub_uint32_t namepad =
    ALIGN (sizeof (*hdr) + namesize, 4) - (sizeof (*hdr) + namesize);
  char *nameptr = (char *) (hdr + 1);
  char *dataptr = (char *) (nameptr + namesize + namepad);
  char intbuf[9];

  // clear
  grub_memset (hdr, '0', sizeof (*hdr));

  // magic
  grub_memcpy (hdr->c_magic, CPIO_NEWC_MAGIC, 8);

  // namesize
  cpio_ul2ostr (intbuf, namesize);
  grub_memcpy (hdr->c_namesize, intbuf, 8);
  // name
  grub_memcpy (nameptr, name, namesize);

  // filesize
  cpio_ul2ostr (intbuf, data_size);
  grub_memcpy (hdr->c_filesize, intbuf, 8);
  // data
  if (data)
    {
      grub_memcpy (dataptr, data, data_size);
    }

  // mode: -rwxr-x---
  grub_memcpy (hdr->c_mode, cpio_mode_executable,
	       grub_strlen (cpio_mode_executable) - 1);

  return (cpio_newc_header_t *) (dataptr + ALIGN (data_size, 4));
}
