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

#ifndef GRUB_CPIO_HEADER
#define GRUB_CPIO_HEADER 1

#define	CPIO_NEWC_MAGIC "070701"
#define CPIO_TRAILER "TRAILER!!!"

typedef struct cpio_newc_header
{
  char c_magic[6];
  char c_ino[8];
  char c_mode[8];
  char c_uid[8];
  char c_gid[8];
  char c_nlink[8];
  char c_mtime[8];
  char c_filesize[8];
  char c_devmajor[8];
  char c_devminor[8];
  char c_rdevmajor[8];
  char c_rdevminor[8];
  char c_namesize[8];
  char c_check[8];
} GRUB_PACKED cpio_newc_header_t;

grub_uint32_t cpio_strtoul (char *in);
void cpio_ul2ostr (char *buf, grub_uint32_t in);
int cpio_is_valid (void *ptr);
int cpio_has_next (cpio_newc_header_t * hdr);
grub_size_t cpio_predict_obj_size (grub_uint32_t namesize,
				   grub_uint32_t filesize);
grub_size_t cpio_get_obj_size (cpio_newc_header_t * hdr);
cpio_newc_header_t *cpio_get_last (cpio_newc_header_t * hdr);

cpio_newc_header_t *cpio_create_obj (cpio_newc_header_t * hdr,
				     const char *name, const void *data,
				     grub_size_t data_size);

#endif /* ! GRUB_CPIO_HEADER */
