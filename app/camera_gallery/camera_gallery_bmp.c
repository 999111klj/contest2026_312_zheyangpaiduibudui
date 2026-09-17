/****************************************************************************
 * apps/examples/camera_gallery/camera_gallery_bmp.c
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "camera_gallery.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BMP_HEADER_SIZE  CAMERA_GALLERY_BMP_HEADER_SIZE
#define BMP_ROW_SIZE     (CAMERA_GALLERY_WIDTH * 2)
#define BMP_FILE_SIZE    CAMERA_GALLERY_BMP_SIZE
#define BMP_COMPRESSION  3

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void bmp_put16(FAR uint8_t *dest, uint16_t value)
{
  dest[0] = value & 0xff;
  dest[1] = value >> 8;
}

static void bmp_put32(FAR uint8_t *dest, uint32_t value)
{
  dest[0] = value & 0xff;
  dest[1] = (value >> 8) & 0xff;
  dest[2] = (value >> 16) & 0xff;
  dest[3] = value >> 24;
}

static uint16_t bmp_get16(FAR const uint8_t *src)
{
  return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static uint32_t bmp_get32(FAR const uint8_t *src)
{
  return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
         ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static int bmp_write_full(int fd, FAR const void *buffer, size_t length)
{
  FAR const uint8_t *src = buffer;

  while (length > 0)
    {
      ssize_t nwritten = write(fd, src, length);

      if (nwritten < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return ERROR;
        }

      if (nwritten == 0)
        {
          errno = EIO;
          return ERROR;
        }

      src += nwritten;
      length -= nwritten;
    }

  return OK;
}

static int bmp_read_full(int fd, FAR void *buffer, size_t length)
{
  FAR uint8_t *dest = buffer;

  while (length > 0)
    {
      ssize_t nread = read(fd, dest, length);

      if (nread < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return ERROR;
        }

      if (nread == 0)
        {
          errno = EINVAL;
          return ERROR;
        }

      dest += nread;
      length -= nread;
    }

  return OK;
}

static void bmp_make_header(FAR uint8_t *header)
{
  memset(header, 0, BMP_HEADER_SIZE);
  header[0] = 'B';
  header[1] = 'M';
  bmp_put32(&header[2], BMP_FILE_SIZE);
  bmp_put32(&header[10], BMP_HEADER_SIZE);
  bmp_put32(&header[14], 40);
  bmp_put32(&header[18], CAMERA_GALLERY_WIDTH);
  bmp_put32(&header[22], CAMERA_GALLERY_HEIGHT);
  bmp_put16(&header[26], 1);
  bmp_put16(&header[28], 16);
  bmp_put32(&header[30], BMP_COMPRESSION);
  bmp_put32(&header[34], CAMERA_GALLERY_IMAGE_SIZE);
  bmp_put32(&header[54], 0xf800);
  bmp_put32(&header[58], 0x07e0);
  bmp_put32(&header[62], 0x001f);
}

static bool bmp_valid_header(FAR const uint8_t *header)
{
  return header[0] == 'B' && header[1] == 'M' &&
         bmp_get32(&header[2]) == BMP_FILE_SIZE &&
         bmp_get32(&header[6]) == 0 &&
         bmp_get32(&header[10]) == BMP_HEADER_SIZE &&
         bmp_get32(&header[14]) == 40 &&
         bmp_get32(&header[18]) == CAMERA_GALLERY_WIDTH &&
         bmp_get32(&header[22]) == CAMERA_GALLERY_HEIGHT &&
         bmp_get16(&header[26]) == 1 && bmp_get16(&header[28]) == 16 &&
         bmp_get32(&header[30]) == BMP_COMPRESSION &&
         bmp_get32(&header[34]) == CAMERA_GALLERY_IMAGE_SIZE &&
         bmp_get32(&header[54]) == 0xf800 &&
         bmp_get32(&header[58]) == 0x07e0 &&
         bmp_get32(&header[62]) == 0x001f;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int camera_gallery_bmp_encode(FAR const uint16_t *pixels,
                              FAR uint8_t *bmp, size_t bmp_size)
{
  FAR uint8_t *dest;
  int x;
  int y;

  if (pixels == NULL || bmp == NULL || bmp_size < BMP_FILE_SIZE)
    {
      errno = EINVAL;
      return ERROR;
    }

  bmp_make_header(bmp);
  dest = bmp + BMP_HEADER_SIZE;

  for (y = CAMERA_GALLERY_HEIGHT - 1; y >= 0; y--)
    {
      FAR const uint16_t *src = pixels + y * CAMERA_GALLERY_WIDTH;

      for (x = 0; x < CAMERA_GALLERY_WIDTH; x++)
        {
          bmp_put16(dest, src[x]);
          dest += 2;
        }
    }

  return OK;
}

int camera_gallery_bmp_save(FAR const char *path,
                            FAR const uint16_t *pixels)
{
  uint8_t header[BMP_HEADER_SIZE];
  uint8_t row[BMP_ROW_SIZE];
  int saved_errno = 0;
  int fd;
  int x;
  int y;
  int ret = ERROR;

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    {
      return ERROR;
    }

  bmp_make_header(header);
  if (bmp_write_full(fd, header, sizeof(header)) < 0)
    {
      goto out;
    }

  for (y = CAMERA_GALLERY_HEIGHT - 1; y >= 0; y--)
    {
      FAR const uint16_t *src = pixels + y * CAMERA_GALLERY_WIDTH;

      for (x = 0; x < CAMERA_GALLERY_WIDTH; x++)
        {
          bmp_put16(&row[x * 2], src[x]);
        }

      if (bmp_write_full(fd, row, sizeof(row)) < 0)
        {
          goto out;
        }
    }

  if (fsync(fd) < 0)
    {
      goto out;
    }

  ret = OK;

out:
  if (ret < 0)
    {
      saved_errno = errno;
    }

  if (close(fd) < 0 && ret == OK)
    {
      saved_errno = errno;
      ret = ERROR;
    }

  if (ret < 0)
    {
      unlink(path);
      errno = saved_errno;
    }

  return ret;
}

int camera_gallery_bmp_load(FAR const char *path, FAR uint16_t *pixels)
{
  struct stat st;
  uint8_t header[BMP_HEADER_SIZE];
  uint8_t row[BMP_ROW_SIZE];
  int fd;
  int x;
  int y;
  int ret = ERROR;

  if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) ||
      st.st_size != BMP_FILE_SIZE)
    {
      errno = EINVAL;
      return ERROR;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return ERROR;
    }

  if (bmp_read_full(fd, header, sizeof(header)) < 0 ||
      !bmp_valid_header(header))
    {
      errno = EINVAL;
      goto out;
    }

  for (y = CAMERA_GALLERY_HEIGHT - 1; y >= 0; y--)
    {
      FAR uint16_t *dest = pixels + y * CAMERA_GALLERY_WIDTH;

      if (bmp_read_full(fd, row, sizeof(row)) < 0)
        {
          goto out;
        }

      for (x = 0; x < CAMERA_GALLERY_WIDTH; x++)
        {
          dest[x] = bmp_get16(&row[x * 2]);
        }
    }

  ret = OK;

out:
  close(fd);
  return ret;
}

static bool camera_gallery_name_number(FAR const char *name,
                                       FAR unsigned int *number)
{
  unsigned int value = 0;
  int i;

  if (strlen(name) != 12 || strncmp(name, "IMG_", 4) != 0 ||
      strcmp(name + 8, ".BMP") != 0)
    {
      return false;
    }

  for (i = 4; i < 8; i++)
    {
      if (name[i] < '0' || name[i] > '9')
        {
          return false;
        }

      value = value * 10 + name[i] - '0';
    }

  *number = value;
  return true;
}

int camera_gallery_scan(FAR const char *directory,
                        FAR unsigned int *numbers, int capacity,
                        FAR unsigned int *maximum)
{
  FAR struct dirent *entry;
  DIR *dir;
  unsigned int number;
  unsigned int max = 0;
  int count = 0;
  int i;

  dir = opendir(directory);
  if (dir == NULL)
    {
      return ERROR;
    }

  errno = 0;
  while ((entry = readdir(dir)) != NULL)
    {
      if (!camera_gallery_name_number(entry->d_name, &number))
        {
          continue;
        }

      if (count == 0 || number > max)
        {
          max = number;
        }

      if (count < capacity && numbers != NULL)
        {
          i = count;
          while (i > 0 && numbers[i - 1] > number)
            {
              numbers[i] = numbers[i - 1];
              i--;
            }

          numbers[i] = number;
        }

      count++;
    }

  if (errno != 0)
    {
      int errcode = errno;
      closedir(dir);
      errno = errcode;
      return ERROR;
    }

  closedir(dir);
  *maximum = max;
  return count;
}
