#include <grub/dl.h>
#include <grub/misc.h>
#include <grub/lib/cpio.h>
#include <grub/mm.h>

GRUB_MOD_LICENSE ("GPLv3+");

// -rwxr-x---
static const char *mode_executable = "000081e8";
static const char *mode_directory = "000041f9";

/*
 * asc_ul()
 *	convert hex/octal character string into a u_long. We do not have to
 *	check for overflow! (the headers in all supported formats are not large
 *	enough to create an overflow).
 *	NOTE: strings passed to us are NOT TERMINATED.
 * Return:
 *	unsigned long value
 */

static u_long
asc_ul (char *str, int len, int base)
{
  char *stop;
  u_long tval = 0;

  stop = str + len;

  /*
   * skip over leading blanks and zeros
   */
  while ((str < stop) && ((*str == ' ') || (*str == '0')))
    ++str;

  /*
   * for each valid digit, shift running value (tval) over to next digit
   * and add next digit
   */
  if (base == HEX)
    {
      while (str < stop)
	{
	  if ((*str >= '0') && (*str <= '9'))
	    tval = (tval << 4) + (*str++ - '0');
	  else if ((*str >= 'A') && (*str <= 'F'))
	    tval = (tval << 4) + 10 + (*str++ - 'A');
	  else if ((*str >= 'a') && (*str <= 'f'))
	    tval = (tval << 4) + 10 + (*str++ - 'a');
	  else
	    break;
	}
    }
  else
    {
      while ((str < stop) && (*str >= '0') && (*str <= '7'))
	tval = (tval << 3) + (*str++ - '0');
    }
  return tval;
}

static int
vcpio_id (char *blk)
{
  if ((grub_strncmp (blk, AVMAGIC, sizeof (AVMAGIC) - 1) != 0))
    return -1;
  return 0;
}

static int
vcpio_rd (char **ptr, CPIO_OBJ * cpio_obj)
{
  HD_VCPIO *hd;
  int nsz;
  ot_type filesize, pad, dpad = 0;

  // check magic
  if (vcpio_id (*ptr))
    {
      grub_printf ("NOT a cpio object!\n");
      return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("NOT a cpio object"));
    }

  // cast to struct
  hd = (void *) *ptr;

  // get the length of the file name
  if ((nsz = (int) asc_ul (hd->c_namesize, sizeof (hd->c_namesize), HEX)) < 2)
    {
      return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("Invalid namesize"));
    }

  // get header padding. header + filename is aligned to 4 byte boundaries
  pad = VCPIO_PAD (sizeof (HD_VCPIO) + nsz);

  // get filesize
  filesize = asc_ul (hd->c_filesize, sizeof (hd->c_filesize), HEX);

  // get data padding
  dpad = VCPIO_PAD (filesize);

  // set data
  cpio_obj->hd = hd;
  cpio_obj->name = *ptr + sizeof (HD_VCPIO);
  cpio_obj->namesize = nsz;
  cpio_obj->data = *ptr + sizeof (HD_VCPIO) + nsz + pad;
  cpio_obj->filesize = filesize;
  cpio_obj->ignore = 0;

  // new pointer
  *ptr += sizeof (HD_VCPIO) + nsz + pad + filesize + dpad;

  return GRUB_ERR_NONE;
}

static HD_VCPIO footer_hd = {
  .c_magic = {AVMAGIC},
  .c_ino = {"000494ff"},
  .c_mode = {"000001a4"},
  .c_nlink = {"00000001"},
};

static CPIO_OBJ footer_obj = {
  .hd = &footer_hd,
  .name = (char *) TRAILER,
  .namesize = sizeof (TRAILER),
  .filesize = 0,
  .ignore = 0,
};

/*
 * ul_asc()
 *	convert an unsigned long into an hex/oct ascii string. pads with LEADING
 *	ascii 0's to fill string completely
 *	NOTE: the string created is NOT TERMINATED.
 */

static int
ul_asc (u_long val, char *str, int len, int base)
{
  char *pt;
  u_long digit;

  /*
   * WARNING str is not '\0' terminated by this routine
   */
  pt = str + len - 1;

  /*
   * do a tailwise conversion (start at right most end of string to place
   * least significant digit). Keep shifting until conversion value goes
   * to zero (all digits were converted)
   */
  if (base == HEX)
    {
      while (pt >= str)
	{
	  if ((digit = (val & 0xf)) < 10)
	    *pt-- = '0' + (char) digit;
	  else
	    *pt-- = 'a' + (char) (digit - 10);
	  if ((val = (val >> 4)) == (u_long) 0)
	    break;
	}
    }
  else
    {
      while (pt >= str)
	{
	  *pt-- = '0' + (char) (val & 0x7);
	  if ((val = (val >> 3)) == (u_long) 0)
	    break;
	}
    }

  /*
   * pad with leading ascii ZEROS. We return -1 if we ran out of space.
   */
  while (pt >= str)
    *pt-- = '0';
  if (val != (u_long) 0)
    return (-1);
  return (0);
}

// returns size of cpio binary
int
cpio_write (CPIO_OBJ * cpio_obj, int num, void *destination, unsigned *size)
{
  int i, pad;
  char *ptr = destination;

  for (i = 0; i <= num; i++)
    {
      // choose between cobj and footer
      CPIO_OBJ *obj = 0;
      if (i == num)
	{
	  obj = &footer_obj;
	}
      else
	obj = &cpio_obj[i];

      // skip ignored objects
      if (cpio_obj[i].ignore)
	continue;

      // header
      HD_VCPIO *hd = (void *) ptr;
      if (ptr + ALIGN4 (sizeof (HD_VCPIO) + obj->namesize) +
	  ALIGN4 (obj->filesize) >= ((char *) destination) + *size)
	{
	  return grub_error (GRUB_ERR_BAD_ARGUMENT,
			     N_("buffer is too small"));
	}

      // copy header
      memcpy (ptr, obj->hd, sizeof (HD_VCPIO));
      ptr += sizeof (HD_VCPIO);

      // update sizes
      if (ul_asc
	  (obj->filesize, hd->c_filesize, sizeof (hd->c_filesize), HEX))
	{
	  return grub_error (GRUB_ERR_BAD_ARGUMENT,
			     N_("Could not update filesize"));
	}
      if (ul_asc
	  (obj->namesize, hd->c_namesize, sizeof (hd->c_namesize), HEX))
	{
	  return grub_error (GRUB_ERR_BAD_ARGUMENT,
			     N_("Could not update namesize"));
	}

      if (obj->namesize > 0)
	{
	  // name
	  memcpy (ptr, obj->name, obj->namesize);
	  ptr += obj->namesize;

	  // padding
	  pad = VCPIO_PAD (sizeof (HD_VCPIO) + obj->namesize);
	  memset (ptr, 0, pad);
	  ptr += pad;
	}

      if (obj->filesize > 0)
	{
	  // data
	  memcpy (ptr, obj->data, obj->filesize);
	  ptr += obj->filesize;

	  // padding
	  pad = VCPIO_PAD (obj->filesize);
	  memset (ptr, 0, pad);
	  ptr += pad;
	}
    }

  *size = ptr - (char *) destination;
  return GRUB_ERR_NONE;
}

grub_err_t
android_cpio_make_executable_file (CPIO_OBJ * obj)
{
  // allocate empty header
  HD_VCPIO *hd = obj->hd = grub_malloc (sizeof (HD_VCPIO));
  grub_memset (hd, '0', sizeof (HD_VCPIO));
  grub_memcpy (hd->c_magic, AVMAGIC, sizeof (AVMAGIC) - 1);

  grub_memcpy (hd->c_mode, mode_executable,
	       grub_strlen (mode_executable) - 1);

  return GRUB_ERR_NONE;
}

grub_err_t
android_cpio_make_directory (CPIO_OBJ * obj)
{
  // allocate empty header
  HD_VCPIO *hd = obj->hd = grub_malloc (sizeof (HD_VCPIO));
  grub_memset (hd, '0', sizeof (HD_VCPIO));
  grub_memcpy (hd->c_magic, AVMAGIC, sizeof (AVMAGIC) - 1);

  grub_memcpy (hd->c_mode, mode_directory, grub_strlen (mode_directory) - 1);

  return GRUB_ERR_NONE;
}

// returns num of objects
int
cpio_load (void *ptr, CPIO_OBJ * cpio_obj, unsigned long *len)
{
  char *obj_ptr = ptr;
  int count = 0;

  // read data
  while (obj_ptr < ((char *) ptr) + *len)
    {
      int i = count++;

      if (vcpio_rd (&obj_ptr, &(cpio_obj[i])))
	{
	  return grub_errno;
	}

      if (grub_strcmp (cpio_obj[i].name, TRAILER) == 0)
	{
	  count--;
	  break;
	}
    }

  *len = count;
  return GRUB_ERR_NONE;
}
