#ifndef CPIO_H
#define CPIO_H

typedef unsigned long u_long;
typedef unsigned long long ot_type;

/*
 * System VR4 cpio header structure (with/without file data crc)
 */
typedef struct
{
  char c_magic[6];		/* magic cookie */
  char c_ino[8];		/* inode number */
  char c_mode[8];		/* file type/access */
  char c_uid[8];		/* owners uid */
  char c_gid[8];		/* owners gid */
  char c_nlink[8];		/* # of links at archive creation */
  char c_mtime[8];		/* modification time */
  char c_filesize[8];		/* length of file in bytes */
  char c_maj[8];		/* block/char major # (device) */
  char c_min[8];		/* block/char minor # (device) */
  char c_rmaj[8];		/* special file major # (node) */
  char c_rmin[8];		/* special file minor # (node) */
  char c_namesize[8];		/* length of pathname */
  char c_chksum[8];		/* 0 OR CRC of bytes of FILE data */
} HD_VCPIO;

typedef struct
{
  HD_VCPIO *hd;
  char *name;
  int namesize;
  void *data;
  ot_type filesize;
  int ignore;
} CPIO_OBJ;

#define	AVMAGIC		"070701"	/* ascii string of above */
#define HEX		16
#define OCT		8
#define VCPIO_PAD(x)	((4 - ((x) & 3)) & 3)	/* pad to next 4 byte word */
#define ALIGN(x,ps)    (((x) + (ps)-1) & (~((ps)-1)))
#define ALIGN4(x)    ALIGN((x), 4)
#define TRAILER		"TRAILER!!!"	/* name in last archive record */

int cpio_load (void *ptr, CPIO_OBJ * cpio_obj, unsigned long *len);
int cpio_write (CPIO_OBJ * cpio_obj, int num, void *destination,
		unsigned *size);

#endif /* CPIO_H */
