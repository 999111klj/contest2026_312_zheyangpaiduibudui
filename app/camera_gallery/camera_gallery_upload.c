/****************************************************************************
 * apps/examples/camera_gallery/camera_gallery_upload.c
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "infra/network_manager.h"
#include "mbedtls/sha256.h"

#include "camera_gallery.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAMERA_GALLERY_UPLOAD_DEFAULT_PORT  "8080"
#define CAMERA_GALLERY_UPLOAD_BUFFER_SIZE   2048
#define CAMERA_GALLERY_UPLOAD_TIMEOUT_SEC   20
#define CAMERA_GALLERY_SHA256_SIZE          32
#define CAMERA_GALLERY_SHA256_TEXT_SIZE     65

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int camera_gallery_upload_parse_number(FAR const char *text,
                                              FAR unsigned int *number)
{
  FAR char *end;
  unsigned long value;

  if (text == NULL || text[0] == '\0')
    {
      errno = EINVAL;
      return ERROR;
    }

  errno = 0;
  value = strtoul(text, &end, 10);
  if (errno != 0 || *end != '\0' || value > 9999)
    {
      errno = EINVAL;
      return ERROR;
    }

  *number = (unsigned int)value;
  return OK;
}

static int camera_gallery_upload_validate_port(FAR const char *text)
{
  FAR char *end;
  unsigned long value;

  if (text == NULL || text[0] == '\0')
    {
      errno = EINVAL;
      return ERROR;
    }

  errno = 0;
  value = strtoul(text, &end, 10);
  if (errno != 0 || *end != '\0' || value == 0 || value > 65535)
    {
      errno = EINVAL;
      return ERROR;
    }

  return OK;
}

static int camera_gallery_upload_hash(int fd,
                                      FAR char *text,
                                      size_t text_size)
{
  mbedtls_sha256_context context;
  uint8_t digest[CAMERA_GALLERY_SHA256_SIZE];
  uint8_t buffer[CAMERA_GALLERY_UPLOAD_BUFFER_SIZE];
  size_t total = 0;
  ssize_t nread;
  int ret = ERROR;
  int i;

  if (text_size < CAMERA_GALLERY_SHA256_TEXT_SIZE)
    {
      errno = ENOSPC;
      return ERROR;
    }

  mbedtls_sha256_init(&context);
  if (mbedtls_sha256_starts(&context, 0) != 0)
    {
      errno = EIO;
      goto out;
    }

  while (total < CAMERA_GALLERY_BMP_SIZE)
    {
      size_t requested = CAMERA_GALLERY_BMP_SIZE - total;

      if (requested > sizeof(buffer))
        {
          requested = sizeof(buffer);
        }

      do
        {
          nread = read(fd, buffer, requested);
        }
      while (nread < 0 && errno == EINTR);

      if (nread < 0)
        {
          goto out;
        }

      if (nread == 0)
        {
          errno = EIO;
          goto out;
        }

      if (mbedtls_sha256_update(&context, buffer, (size_t)nread) != 0)
        {
          errno = EIO;
          goto out;
        }

      total += (size_t)nread;
    }

  do
    {
      nread = read(fd, buffer, 1);
    }
  while (nread < 0 && errno == EINTR);

  if (nread < 0)
    {
      goto out;
    }

  if (nread != 0)
    {
      errno = EFBIG;
      goto out;
    }

  if (mbedtls_sha256_finish(&context, digest) != 0)
    {
      errno = EIO;
      goto out;
    }

  for (i = 0; i < CAMERA_GALLERY_SHA256_SIZE; i++)
    {
      snprintf(text + i * 2, text_size - i * 2, "%02x", digest[i]);
    }

  text[CAMERA_GALLERY_SHA256_TEXT_SIZE - 1] = '\0';
  ret = OK;

out:
  mbedtls_sha256_free(&context);
  return ret;
}

static int camera_gallery_upload_connect(FAR const char *host,
                                         FAR const char *port)
{
  struct addrinfo hints;
  FAR struct addrinfo *addresses = NULL;
  FAR struct addrinfo *address;
  struct timeval timeout;
  int sockfd = -1;
  int ret;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  ret = getaddrinfo(host, port, &hints, &addresses);
  if (ret != 0)
    {
      errno = EHOSTUNREACH;
      return ERROR;
    }

  timeout.tv_sec = CAMERA_GALLERY_UPLOAD_TIMEOUT_SEC;
  timeout.tv_usec = 0;
  for (address = addresses; address != NULL; address = address->ai_next)
    {
      sockfd = socket(address->ai_family, address->ai_socktype,
                      address->ai_protocol);
      if (sockfd < 0)
        {
          continue;
        }

      setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO,
                 &timeout, sizeof(timeout));
      setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,
                 &timeout, sizeof(timeout));
      do
        {
          ret = connect(sockfd, address->ai_addr, address->ai_addrlen);
        }
      while (ret < 0 && errno == EINTR);

      if (ret == 0)
        {
          break;
        }

      close(sockfd);
      sockfd = -1;
    }

  freeaddrinfo(addresses);
  if (sockfd < 0 && errno == 0)
    {
      errno = ECONNREFUSED;
    }

  return sockfd;
}

static int camera_gallery_upload_send_all(int sockfd,
                                          FAR const void *buffer,
                                          size_t length)
{
  FAR const uint8_t *cursor = buffer;

  while (length > 0)
    {
      ssize_t nsent = send(sockfd, cursor, length, 0);

      if (nsent < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return ERROR;
        }

      if (nsent == 0)
        {
          errno = EPIPE;
          return ERROR;
        }

      cursor += nsent;
      length -= nsent;
    }

  return OK;
}

static int camera_gallery_upload_response(int sockfd,
                                          FAR const char *expected_digest)
{
  char response[1024];
  FAR char *digest;
  FAR char *space;
  size_t used = 0;
  ssize_t nread;
  long status;

  while (used + 1 < sizeof(response))
    {
      do
        {
          nread = recv(sockfd, response + used,
                       sizeof(response) - used - 1, 0);
        }
      while (nread < 0 && errno == EINTR);

      if (nread <= 0)
        {
          break;
        }

      used += nread;
      response[used] = '\0';
      if (strstr(response, "\r\n\r\n") != NULL)
        {
          break;
        }
    }

  response[used] = '\0';
  if (used == 0 || strncmp(response, "HTTP/", 5) != 0 ||
      strstr(response, "\r\n\r\n") == NULL)
    {
      errno = EPROTO;
      return ERROR;
    }

  space = strchr(response, ' ');
  if (space == NULL)
    {
      errno = EPROTO;
      return ERROR;
    }

  status = strtol(space + 1, NULL, 10);
  if (status < 200 || status >= 300)
    {
      printf("camera_gallery: upload server returned HTTP %ld\n", status);
      errno = EIO;
      return ERROR;
    }

  digest = strstr(response, "\r\nX-Content-SHA256: ");
  if (digest == NULL)
    {
      errno = EPROTO;
      return ERROR;
    }

  digest += strlen("\r\nX-Content-SHA256: ");
  if (strncmp(digest, expected_digest, CAMERA_GALLERY_SHA256_TEXT_SIZE - 1)
      != 0 ||
      digest[CAMERA_GALLERY_SHA256_TEXT_SIZE - 1] != '\r')
    {
      errno = EBADMSG;
      return ERROR;
    }

  return OK;
}

static int camera_gallery_upload_photo(unsigned int number,
                                       FAR const char *host,
                                       FAR const char *port)
{
  struct stat st;
  char path[128];
  char filename[16];
  char digest[CAMERA_GALLERY_SHA256_TEXT_SIZE];
  char header[512];
  uint8_t buffer[CAMERA_GALLERY_UPLOAD_BUFFER_SIZE];
  size_t remaining;
  ssize_t nread;
  int saved_errno;
  int sockfd = -1;
  int fd = -1;
  int length;
  int ret = ERROR;

  length = snprintf(filename, sizeof(filename), "IMG_%04u.BMP", number);
  if (length < 0 || (size_t)length >= sizeof(filename))
    {
      errno = ENAMETOOLONG;
      return ERROR;
    }

  length = snprintf(path, sizeof(path), "%s/DCIM/%s",
                    CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT,
                    filename);
  if (length < 0 || (size_t)length >= sizeof(path))
    {
      errno = ENAMETOOLONG;
      return ERROR;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return ERROR;
    }

  if (fstat(fd, &st) < 0)
    {
      goto out;
    }

  if (!S_ISREG(st.st_mode) || st.st_size != CAMERA_GALLERY_BMP_SIZE)
    {
      errno = EINVAL;
      goto out;
    }

  if (camera_gallery_upload_hash(fd, digest, sizeof(digest)) < 0 ||
      lseek(fd, 0, SEEK_SET) < 0)
    {
      goto out;
    }

  if (!network_is_connected() && network_wifi_reconnect() < 0)
    {
      errno = ENETDOWN;
      goto out;
    }

  sockfd = camera_gallery_upload_connect(host, port);
  if (sockfd < 0)
    {
      goto out;
    }

  length = snprintf(header, sizeof(header),
                    "POST /upload/%s HTTP/1.1\r\n"
                    "Host: %s:%s\r\n"
                    "Content-Type: image/bmp\r\n"
                    "Content-Length: %lu\r\n"
                    "X-Content-SHA256: %s\r\n"
                    "Connection: close\r\n\r\n",
                    filename, host, port,
                    (unsigned long)CAMERA_GALLERY_BMP_SIZE, digest);
  if (length < 0 || (size_t)length >= sizeof(header))
    {
      errno = ENAMETOOLONG;
      goto out;
    }

  printf("camera_gallery: uploading %s to %s:%s sha256=%s\n",
         filename, host, port, digest);
  if (camera_gallery_upload_send_all(sockfd, header, length) < 0)
    {
      goto out;
    }

  remaining = CAMERA_GALLERY_BMP_SIZE;
  while (remaining > 0)
    {
      size_t requested = remaining;

      if (requested > sizeof(buffer))
        {
          requested = sizeof(buffer);
        }

      do
        {
          nread = read(fd, buffer, requested);
        }
      while (nread < 0 && errno == EINTR);

      if (nread < 0)
        {
          goto out;
        }

      if (nread == 0)
        {
          errno = EIO;
          goto out;
        }

      if (camera_gallery_upload_send_all(sockfd, buffer,
                                         (size_t)nread) < 0)
        {
          goto out;
        }

      remaining -= (size_t)nread;
    }

  if (camera_gallery_upload_response(sockfd, digest) < 0)
    {
      goto out;
    }

  printf("camera_gallery: upload complete %s sha256=%s\n",
         filename, digest);
  ret = OK;

out:
  saved_errno = errno;
  if (sockfd >= 0)
    {
      close(sockfd);
    }

  if (fd >= 0)
    {
      close(fd);
    }

  errno = saved_errno;
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int camera_gallery_upload_command(int argc, FAR char *argv[])
{
  FAR const char *port = CAMERA_GALLERY_UPLOAD_DEFAULT_PORT;
  unsigned int number;

  if ((argc != 4 && argc != 5) ||
      camera_gallery_upload_parse_number(argv[2], &number) < 0)
    {
      printf("Usage: camera_gallery upload <0-9999> <pc_ip> [port]\n");
      return EXIT_FAILURE;
    }

  if (argc == 5)
    {
      port = argv[4];
    }

  if (camera_gallery_upload_validate_port(port) < 0)
    {
      printf("camera_gallery: invalid upload port\n");
      return EXIT_FAILURE;
    }

  if (camera_gallery_upload_photo(number, argv[3], port) < 0)
    {
      printf("camera_gallery: upload failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}
