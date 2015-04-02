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

#ifndef GRUB_LOADER_MACHINE_HEADER
#define GRUB_LOADER_MACHINE_HEADER	1

typedef void *(*grub_efi_mmap_iteration_cb) (void *,
					     grub_efi_physical_address_t,
					     grub_efi_uint64_t);

grub_err_t EXPORT_FUNC (grub_efi_prepare_platform) (void);
void * EXPORT_FUNC (grub_efi_allocate_loader_memory) (grub_uint32_t min_offset,
						      grub_uint32_t size);
void *EXPORT_FUNC (grub_efi_iterate_memory_map) (void *pdata,
						 grub_efi_mmap_iteration_cb
						 cb);

#endif /* ! GRUB_LOADER_MACHINE_HEADER */
