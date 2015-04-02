/*
 *  linux/include/asm/setup.h
 *
 *  Copyright (C) 1997-1999 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Structure passed to kernel to tell it about the
 *  hardware it's running on.  See Documentation/arm/Setup
 *  for more info.
 */
#ifndef GRUB_ATAGS_CPU_HEADER
#define GRUB_ATAGS_CPU_HEADER

#include <grub/types.h>

#define COMMAND_LINE_SIZE 1024

/* The list ends with an ATAG_NONE node. */
#define ATAG_NONE	0x00000000

struct tag_header
{
  grub_uint32_t size;
  grub_uint32_t tag;
};

/* The list must start with an ATAG_CORE node */
#define ATAG_CORE	0x54410001

struct tag_core
{
  grub_uint32_t flags;		/* bit 0 = read-only */
  grub_uint32_t pagesize;
  grub_uint32_t rootdev;
};

/* it is allowed to have multiple ATAG_MEM nodes */
#define ATAG_MEM	0x54410002

struct tag_mem32
{
  grub_uint32_t size;
  grub_uint32_t start;		/* physical start address */
};

/* VGA text type displays */
#define ATAG_VIDEOTEXT	0x54410003

struct tag_videotext
{
  grub_uint8_t x;
  grub_uint8_t y;
  grub_uint16_t video_page;
  grub_uint8_t video_mode;
  grub_uint8_t video_cols;
  grub_uint16_t video_ega_bx;
  grub_uint8_t video_lines;
  grub_uint8_t video_isvga;
  grub_uint16_t video_points;
};

/* describes how the ramdisk will be used in kernel */
#define ATAG_RAMDISK	0x54410004

struct tag_ramdisk
{
  grub_uint32_t flags;		/* bit 0 = load, bit 1 = prompt */
  grub_uint32_t size;		/* decompressed ramdisk size in _kilo_ bytes */
  grub_uint32_t start;		/* starting block of floppy-based RAM disk image */
};

/* describes where the compressed ramdisk image lives (virtual address) */
/*
 * this one accidentally used virtual addresses - as such,
 * it's deprecated.
 */
#define ATAG_INITRD	0x54410005

/* describes where the compressed ramdisk image lives (physical address) */
#define ATAG_INITRD2	0x54420005

struct tag_initrd
{
  grub_uint32_t start;		/* physical start address */
  grub_uint32_t size;		/* size of compressed ramdisk image in bytes */
};

/* board serial number. "64 bits should be enough for everybody" */
#define ATAG_SERIAL	0x54410006

struct tag_serialnr
{
  grub_uint32_t low;
  grub_uint32_t high;
};

/* board revision */
#define ATAG_REVISION	0x54410007

struct tag_revision
{
  grub_uint32_t rev;
};

/* initial values for vesafb-type framebuffers. see struct screen_info
 * in include/linux/tty.h
 */
#define ATAG_VIDEOLFB	0x54410008

struct tag_videolfb
{
  grub_uint16_t lfb_width;
  grub_uint16_t lfb_height;
  grub_uint16_t lfb_depth;
  grub_uint16_t lfb_linelength;
  grub_uint32_t lfb_base;
  grub_uint32_t lfb_size;
  grub_uint8_t red_size;
  grub_uint8_t red_pos;
  grub_uint8_t green_size;
  grub_uint8_t green_pos;
  grub_uint8_t blue_size;
  grub_uint8_t blue_pos;
  grub_uint8_t rsvd_size;
  grub_uint8_t rsvd_pos;
};

/* command line: \0 terminated string */
#define ATAG_CMDLINE	0x54410009

struct tag_cmdline
{
  char cmdline[1];		/* this is the minimum size */
};

/* acorn RiscPC specific information */
#define ATAG_ACORN	0x41000101

struct tag_acorn
{
  grub_uint32_t memc_control_reg;
  grub_uint32_t vram_pages;
  grub_uint8_t sounddefault;
  grub_uint8_t adfsdrives;
};

/* footbridge memory clock, see arch/arm/mach-footbridge/arch.c */
#define ATAG_MEMCLK	0x41000402

struct tag_memclk
{
  grub_uint32_t fmemclk;
};

struct tag
{
  struct tag_header hdr;
  union
  {
    struct tag_core core;
    struct tag_mem32 mem;
    struct tag_videotext videotext;
    struct tag_ramdisk ramdisk;
    struct tag_initrd initrd;
    struct tag_serialnr serialnr;
    struct tag_revision revision;
    struct tag_videolfb videolfb;
    struct tag_cmdline cmdline;

    /*
     * Acorn specific
     */
    struct tag_acorn acorn;

    /*
     * DC21285 specific
     */
    struct tag_memclk memclk;
  } u;
};

struct tagtable
{
  grub_uint32_t tag;
  int (*parse) (const struct tag *);
};

#define tag_member_present(tag,member)				\
	((unsigned long)(&((struct tag *)0L)->member + 1)	\
		<= (tag)->hdr.size * 4)

#define tag_next(t)	((struct tag *)((grub_uint32_t *)(t) + (t)->hdr.size))
#define tag_size(type)	((sizeof(struct tag_header) + sizeof(struct type)) >> 2)

#define for_each_tag(t,base)		\
	for (t = base; t->hdr.size; t = tag_next(t))

#endif /* ! GRUB_ATAGS_CPU_HEADER */
