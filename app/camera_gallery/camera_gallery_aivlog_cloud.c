/****************************************************************************
 * apps/examples/camera_gallery/camera_gallery_aivlog_cloud.c
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/stat.h>

#include <arpa/inet.h>

#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "infra/config_store.h"
#include "infra/network_manager.h"
#include "infra/vela_tls.h"
#include "agent_compat.h"

#include "camera_gallery.h"
#include "camera_gallery_aivlog_model.h"

#define AIVLOG_CLOUD_ROOT_NAME        "AIVLOG"
#define AIVLOG_CLOUD_SESSION_PREFIX   "SESSION_"
#define AIVLOG_CLOUD_MANIFEST_NAME    "manifest"
#define AIVLOG_CLOUD_MANIFEST_TMP     "manifest.tmp"
#define AIVLOG_CLOUD_PATH_SIZE        192
#define AIVLOG_CLOUD_LINE_SIZE        160
#define AIVLOG_CLOUD_HOST_SIZE        128
#define AIVLOG_CLOUD_TOKEN_SIZE       256
#define AIVLOG_CLOUD_CERT_PIN_SIZE    65
#define AIVLOG_CLOUD_DEVICE_ID_SIZE   33
#define AIVLOG_CLOUD_BASE_PATH_SIZE   96
#define AIVLOG_CLOUD_RESPONSE_SIZE    384
#define AIVLOG_CLOUD_SCHEME_SIZE      6
#define AIVLOG_CLOUD_PORT_SIZE        6
#define AIVLOG_CLOUD_DEFAULT_PATH     "/v1/aivlog"
#define AIVLOG_CLOUD_DEFAULT_SCHEME   "https"
#define AIVLOG_CLOUD_DEFAULT_HTTP_PORT "8080"
#define AIVLOG_CLOUD_DEFAULT_HTTPS_PORT "443"
#define AIVLOG_CLOUD_CONFIG_SCHEME    "aivlog_cloud_scheme"
#define AIVLOG_CLOUD_CONFIG_HOST      "aivlog_cloud_host"
#define AIVLOG_CLOUD_CONFIG_PORT      "aivlog_cloud_port"
#define AIVLOG_CLOUD_CONFIG_TOKEN     "aivlog_cloud_token"
#define AIVLOG_CLOUD_CONFIG_CERT_PIN  "aivlog_cloud_cert_sha256"
#define AIVLOG_CLOUD_CONFIG_DEVICE_ID "aivlog_device_id"
#define AIVLOG_CLOUD_CONFIG_PATH      "aivlog_cloud_path"
#define AIVLOG_CLOUD_MANIFEST_VERSION 1
#define AIVLOG_CLOUD_MANIFEST_BUFFER_SIZE 4096
#define AIVLOG_CLOUD_SD_BLOCK_SIZE    512
#define AIVLOG_CLOUD_IDLE_SECONDS     30

#define AIVLOG_CLOUD_STATE_ACTIVE      0
#define AIVLOG_CLOUD_STATE_FINISHED    1
#define AIVLOG_CLOUD_STATE_COMPLETE    2
#define AIVLOG_CLOUD_STATE_DISCARDED   3

#define AIVLOG_CLOUD_WORK_NONE          0
#define AIVLOG_CLOUD_WORK_START         1
#define AIVLOG_CLOUD_WORK_ASSET         2
#define AIVLOG_CLOUD_WORK_FINISH        3
#define AIVLOG_CLOUD_WORK_LOCAL_CLOSE   4
#define AIVLOG_CLOUD_WORK_LOCAL_DISCARD 5

struct aivlog_cloud_asset_s
{
  unsigned int sequence;
  int score;
  float raw_score;
  uint64_t dhash;
  unsigned int attempts;
  bool uploaded;
  bool failed;
};

struct aivlog_cloud_manifest_s
{
  uint32_t revision;
  uint32_t session_id;
  char session_uuid[AIVLOG_CLOUD_DEVICE_ID_SIZE];
  int state;
  bool remote_started;
  bool remote_finished;
  int asset_count;
  struct aivlog_cloud_asset_s
    assets[CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_MAX_ASSETS];
};

struct aivlog_cloud_work_s
{
  int type;
  uint32_t session_id;
  char session_uuid[AIVLOG_CLOUD_DEVICE_ID_SIZE];
  unsigned int sequence;
  int score;
  float raw_score;
  uint64_t dhash;
};

struct aivlog_cloud_config_s
{
  char scheme[AIVLOG_CLOUD_SCHEME_SIZE];
  char host[AIVLOG_CLOUD_HOST_SIZE];
  char port[AIVLOG_CLOUD_PORT_SIZE];
  char token[AIVLOG_CLOUD_TOKEN_SIZE];
  char cert_pin[AIVLOG_CLOUD_CERT_PIN_SIZE];
  char device_id[AIVLOG_CLOUD_DEVICE_ID_SIZE];
  char base_path[AIVLOG_CLOUD_BASE_PATH_SIZE];
};

struct aivlog_cloud_service_s
{
  pthread_mutex_t lock;
  pthread_cond_t cond;
  pthread_t thread;
  struct timespec last_accept;
  uint32_t current_session;
  uint64_t last_dhash;
  unsigned int retry_count;
  bool initialized;
  bool thread_started;
  bool stopping;
  bool current_active;
  bool current_accepting;
  bool cloud_blocked;
  bool has_last_accept;
};

static struct aivlog_cloud_service_s g_aivlog_cloud;
static pthread_mutex_t g_aivlog_cloud_config_lock =
  PTHREAD_MUTEX_INITIALIZER;

/* The ESP32-S3 SD/MMC path cannot reliably DMA directly into SPIRAM-backed
 * malloc buffers.  The uploader is single-threaded, so one internal .bss
 * staging buffer safely bridges SD reads into the large upload buffer.
 */
static uint8_t g_aivlog_cloud_sd_io[AIVLOG_CLOUD_SD_BLOCK_SIZE]
  __attribute__((aligned(64)));
static char g_aivlog_cloud_manifest_io[AIVLOG_CLOUD_MANIFEST_BUFFER_SIZE]
  __attribute__((aligned(64)));

static bool aivlog_cloud_valid_hex(FAR const char *text, size_t length);
static bool aivlog_cloud_valid_host(FAR const char *host);
static bool aivlog_cloud_valid_path(FAR const char *path);
static bool aivlog_cloud_valid_port(FAR const char *port);
static bool aivlog_cloud_valid_scheme(FAR const char *scheme);
static bool aivlog_cloud_valid_token(FAR const char *token);
static bool aivlog_cloud_valid_private_ipv4(FAR const char *host);

static int aivlog_cloud_path(FAR char *path, size_t path_size,
                             FAR const char *format, ...)
{
  va_list arguments;
  int length;

  va_start(arguments, format);
  length = vsnprintf(path, path_size, format, arguments);
  va_end(arguments);
  if (length < 0 || (size_t)length >= path_size)
    {
      errno = ENAMETOOLONG;
      return ERROR;
    }

  return OK;
}

static int aivlog_cloud_root_path(FAR char *path, size_t path_size)
{
  return aivlog_cloud_path(path, path_size, "%s/%s",
                           CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT,
                           AIVLOG_CLOUD_ROOT_NAME);
}

static int aivlog_cloud_session_path(uint32_t session_id,
                                     FAR char *path, size_t path_size)
{
  return aivlog_cloud_path(path, path_size, "%s/%s/%s%08lu",
                           CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT,
                           AIVLOG_CLOUD_ROOT_NAME,
                           AIVLOG_CLOUD_SESSION_PREFIX,
                           (unsigned long)session_id);
}

static int aivlog_cloud_manifest_path(uint32_t session_id, bool temporary,
                                      FAR char *path, size_t path_size)
{
  return aivlog_cloud_path(path, path_size, "%s/%s/%s%08lu/%s",
                           CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT,
                           AIVLOG_CLOUD_ROOT_NAME,
                           AIVLOG_CLOUD_SESSION_PREFIX,
                           (unsigned long)session_id,
                           temporary ? AIVLOG_CLOUD_MANIFEST_TMP :
                           AIVLOG_CLOUD_MANIFEST_NAME);
}

static int aivlog_cloud_asset_path(uint32_t session_id,
                                   unsigned int sequence, bool temporary,
                                   FAR char *path, size_t path_size)
{
  return aivlog_cloud_path(path, path_size,
                           "%s/%s/%s%08lu/ASSET_%04u.BMP%s",
                           CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT,
                           AIVLOG_CLOUD_ROOT_NAME,
                           AIVLOG_CLOUD_SESSION_PREFIX,
                           (unsigned long)session_id, sequence,
                           temporary ? ".tmp" : "");
}

static bool aivlog_cloud_session_name(FAR const char *name,
                                      FAR uint32_t *session_id)
{
  FAR char *end;
  unsigned long value;

  if (strncmp(name, AIVLOG_CLOUD_SESSION_PREFIX,
              strlen(AIVLOG_CLOUD_SESSION_PREFIX)) != 0)
    {
      return false;
    }

  errno = 0;
  value = strtoul(name + strlen(AIVLOG_CLOUD_SESSION_PREFIX), &end, 10);
  if (errno != 0 || *end != '\0' || value == 0 || value > UINT32_MAX)
    {
      return false;
    }

  *session_id = (uint32_t)value;
  return true;
}

static int aivlog_cloud_sync_directory(FAR const char *path)
{
  char marker[AIVLOG_CLOUD_PATH_SIZE];
  uint8_t value = 0xa5;
  int fd = open(path, O_RDONLY);

  if (fd >= 0)
    {
      if (fsync(fd) < 0 && errno != EINVAL && errno != ENOSYS &&
          errno != EBADF)
        {
          int saved_errno = errno;
          close(fd);
          errno = saved_errno;
          return ERROR;
        }

      if (close(fd) < 0)
        {
          return ERROR;
        }

      return OK;
    }

  /* NuttX FAT rejects opening a directory.  Fsync a marker created inside
   * that directory after each rename; the FAT file sync path flushes the
   * directory entry and volume metadata on this target.
   */

  if (aivlog_cloud_path(marker, sizeof(marker), "%s/.sync", path) < 0)
    {
      return ERROR;
    }

  fd = open(marker, O_WRONLY | O_CREAT, 0600);
  if (fd < 0 || lseek(fd, 0, SEEK_SET) < 0 ||
      write(fd, &value, sizeof(value)) != sizeof(value) || fsync(fd) < 0)
    {
      int saved_errno = errno;
      if (fd >= 0)
        {
          close(fd);
        }

      errno = saved_errno;
      return ERROR;
    }

  if (close(fd) < 0)
    {
      return ERROR;
    }

  return OK;
}

static int aivlog_cloud_write_manifest(
  FAR struct aivlog_cloud_manifest_s *manifest)
{
  char path[AIVLOG_CLOUD_PATH_SIZE];
  char temporary[AIVLOG_CLOUD_PATH_SIZE];
  char directory[AIVLOG_CLOUD_PATH_SIZE];
  size_t length;
  size_t padded_length;
  size_t offset;
  int fd = -1;
  int saved_errno = 0;
  int written;
  int i;

  if (aivlog_cloud_manifest_path(manifest->session_id, false,
                                 path, sizeof(path)) < 0 ||
      aivlog_cloud_manifest_path(manifest->session_id, true,
                                 temporary, sizeof(temporary)) < 0 ||
      aivlog_cloud_session_path(manifest->session_id, directory,
                                sizeof(directory)) < 0)
    {
      return ERROR;
    }

  manifest->revision++;
  written = snprintf(g_aivlog_cloud_manifest_io,
                     sizeof(g_aivlog_cloud_manifest_io),
                     "AIVLOG_MANIFEST %d\nREVISION %lu\n"
                     "SESSION %08lu\nUUID %s\nSTATE %d\n"
                     "REMOTE_STARTED %d\nREMOTE_FINISHED %d\nCOUNT %d\n",
                     AIVLOG_CLOUD_MANIFEST_VERSION,
                     (unsigned long)manifest->revision,
                     (unsigned long)manifest->session_id,
                     manifest->session_uuid, manifest->state,
                     manifest->remote_started ? 1 : 0,
                     manifest->remote_finished ? 1 : 0,
                     manifest->asset_count);
  if (written < 0 || (size_t)written >= sizeof(g_aivlog_cloud_manifest_io))
    {
      errno = ENOSPC;
      return ERROR;
    }

  length = (size_t)written;
  for (i = 0; i < manifest->asset_count; i++)
    {
      FAR struct aivlog_cloud_asset_s *asset = &manifest->assets[i];

      written = snprintf(g_aivlog_cloud_manifest_io + length,
                         sizeof(g_aivlog_cloud_manifest_io) - length,
                         "ASSET %u %d %.6f %016" PRIx64 " %d %d %u\n",
                         asset->sequence, asset->score,
                         (double)asset->raw_score, asset->dhash,
                         asset->uploaded ? 1 : 0,
                         asset->failed ? 1 : 0, asset->attempts);
      if (written < 0 ||
          (size_t)written >= sizeof(g_aivlog_cloud_manifest_io) - length)
        {
          errno = ENOSPC;
          return ERROR;
        }

      length += (size_t)written;
    }

  padded_length = (length + AIVLOG_CLOUD_SD_BLOCK_SIZE - 1) &
                  ~(AIVLOG_CLOUD_SD_BLOCK_SIZE - 1);
  if (padded_length >= sizeof(g_aivlog_cloud_manifest_io))
    {
      errno = ENOSPC;
      return ERROR;
    }

  /* FAT/SDMMC on this board requires aligned internal-RAM sector transfers.
   * Newline padding is valid because the parser ignores blank lines. */

  memset(g_aivlog_cloud_manifest_io + length, '\n',
         padded_length - length);
  fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    {
      return ERROR;
    }

  for (offset = 0; offset < padded_length;
       offset += AIVLOG_CLOUD_SD_BLOCK_SIZE)
    {
      ssize_t count;
      do
        {
          count = write(fd, g_aivlog_cloud_manifest_io + offset,
                        AIVLOG_CLOUD_SD_BLOCK_SIZE);
        }
      while (count < 0 && errno == EINTR);

      if (count != AIVLOG_CLOUD_SD_BLOCK_SIZE)
        {
          if (count >= 0)
            {
              errno = EIO;
            }

          goto fail;
        }
    }

  if (fsync(fd) < 0)
    {
      goto fail;
    }

  if (close(fd) < 0)
    {
      saved_errno = errno;
      fd = -1;
      goto fail_unlink;
    }
  fd = -1;

  fd = open(temporary, O_RDONLY);
  if (fd < 0)
    {
      goto fail_unlink;
    }

  for (offset = 0; offset < padded_length;
       offset += AIVLOG_CLOUD_SD_BLOCK_SIZE)
    {
      ssize_t count;
      do
        {
          count = read(fd, g_aivlog_cloud_sd_io,
                       AIVLOG_CLOUD_SD_BLOCK_SIZE);
        }
      while (count < 0 && errno == EINTR);

      if (count != AIVLOG_CLOUD_SD_BLOCK_SIZE ||
          memcmp(g_aivlog_cloud_sd_io,
                 g_aivlog_cloud_manifest_io + offset,
                 AIVLOG_CLOUD_SD_BLOCK_SIZE) != 0)
        {
          errno = EIO;
          goto fail;
        }
    }

  if (close(fd) < 0)
    {
      saved_errno = errno;
      fd = -1;
      goto fail_unlink;
    }
  fd = -1;

  if (rename(temporary, path) < 0 ||
      aivlog_cloud_sync_directory(directory) < 0)
    {
      goto fail_unlink;
    }

  return OK;

fail:
  saved_errno = errno;
  if (fd >= 0)
    {
      close(fd);
    }
fail_unlink:
  if (saved_errno == 0)
    {
      saved_errno = errno;
    }
  unlink(temporary);
  errno = saved_errno;
  return ERROR;
}

static int aivlog_cloud_read_manifest(
  uint32_t session_id, FAR struct aivlog_cloud_manifest_s *manifest)
{
  char path[AIVLOG_CLOUD_PATH_SIZE];
  char session_uuid[AIVLOG_CLOUD_DEVICE_ID_SIZE];
  char extra;
  FAR char *line;
  FAR char *cursor;
  FAR char *next;
  struct stat file_stat;
  size_t file_size;
  size_t offset;
  int fd = -1;
  unsigned long value;
  unsigned long manifest_session;
  unsigned long long hash;
  unsigned int sequence;
  unsigned int attempts;
  int version;
  int state;
  int boolean_value;
  int failed_value;
  int score;
  int declared_count = -1;
  int assets = 0;
  float raw_score;
  bool version_seen = false;
  bool revision_seen = false;
  bool session_seen = false;
  bool uuid_seen = false;
  bool state_seen = false;
  bool remote_started_seen = false;
  bool remote_finished_seen = false;
  bool count_seen = false;
  bool any_uploaded = false;
  bool all_resolved = true;

  if (aivlog_cloud_manifest_path(session_id, false,
                                 path, sizeof(path)) < 0)
    {
      return ERROR;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0 || fstat(fd, &file_stat) < 0 || file_stat.st_size <= 0 ||
      (size_t)file_stat.st_size >= sizeof(g_aivlog_cloud_manifest_io) ||
      ((size_t)file_stat.st_size % AIVLOG_CLOUD_SD_BLOCK_SIZE) != 0)
    {
      if (fd >= 0)
        {
          close(fd);
        }
      errno = EPROTO;
      return ERROR;
    }

  file_size = (size_t)file_stat.st_size;
  for (offset = 0; offset < file_size;
       offset += AIVLOG_CLOUD_SD_BLOCK_SIZE)
    {
      ssize_t count;
      do
        {
          count = read(fd, g_aivlog_cloud_manifest_io + offset,
                       AIVLOG_CLOUD_SD_BLOCK_SIZE);
        }
      while (count < 0 && errno == EINTR);

      if (count != AIVLOG_CLOUD_SD_BLOCK_SIZE)
        {
          close(fd);
          errno = EIO;
          return ERROR;
        }
    }

  close(fd);
  fd = -1;
  g_aivlog_cloud_manifest_io[file_size] = '\0';

  memset(manifest, 0, sizeof(*manifest));
  manifest->session_id = session_id;
  cursor = g_aivlog_cloud_manifest_io;
  while (*cursor != '\0')
    {
      line = cursor;
      next = strchr(cursor, '\n');
      if (next != NULL)
        {
          *next++ = '\0';
          cursor = next;
        }
      else
        {
          cursor += strlen(cursor);
        }

      if (line[0] == '\0' || strspn(line, " \t\r") == strlen(line))
        {
          continue;
        }

      if (sscanf(line, "AIVLOG_MANIFEST %d %c",
                 &version, &extra) == 1)
        {
          if (version_seen || version != AIVLOG_CLOUD_MANIFEST_VERSION)
            {
              errno = EPROTO;
              goto fail;
            }

          version_seen = true;
        }
      else if (sscanf(line, "REVISION %lu %c", &value, &extra) == 1)
        {
          if (revision_seen || value > UINT32_MAX)
            {
              errno = EPROTO;
              goto fail;
            }

          revision_seen = true;
          manifest->revision = (uint32_t)value;
        }
      else if (sscanf(line, "SESSION %lu %c",
                      &manifest_session, &extra) == 1)
        {
          if (session_seen || manifest_session != session_id)
            {
              errno = EPROTO;
              goto fail;
            }

          session_seen = true;
        }
      else if (sscanf(line, "UUID %32s %c",
                      session_uuid, &extra) == 1)
        {
          if (uuid_seen ||
              !aivlog_cloud_valid_hex(session_uuid,
                                      AIVLOG_CLOUD_DEVICE_ID_SIZE - 1))
            {
              errno = EPROTO;
              goto fail;
            }

          uuid_seen = true;
          snprintf(manifest->session_uuid,
                   sizeof(manifest->session_uuid), "%s", session_uuid);
        }
      else if (sscanf(line, "STATE %d %c", &state, &extra) == 1)
        {
          if (state_seen || state < AIVLOG_CLOUD_STATE_ACTIVE ||
              state > AIVLOG_CLOUD_STATE_DISCARDED)
            {
              errno = EPROTO;
              goto fail;
            }

          state_seen = true;
          manifest->state = state;
        }
      else if (sscanf(line, "REMOTE_STARTED %d %c",
                      &boolean_value, &extra) == 1)
        {
          if (remote_started_seen ||
              (boolean_value != 0 && boolean_value != 1))
            {
              errno = EPROTO;
              goto fail;
            }

          remote_started_seen = true;
          manifest->remote_started = boolean_value != 0;
        }
      else if (sscanf(line, "REMOTE_FINISHED %d %c",
                      &boolean_value, &extra) == 1)
        {
          if (remote_finished_seen ||
              (boolean_value != 0 && boolean_value != 1))
            {
              errno = EPROTO;
              goto fail;
            }

          remote_finished_seen = true;
          manifest->remote_finished = boolean_value != 0;
        }
      else if (sscanf(line, "COUNT %d %c",
                      &declared_count, &extra) == 1)
        {
          if (count_seen || declared_count < 0 ||
              declared_count >
                CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_MAX_ASSETS)
            {
              errno = EOVERFLOW;
              goto fail;
            }

          count_seen = true;
        }
      else if (sscanf(line,
                      "ASSET %u %d %f %llx %d %d %u %c",
                      &sequence, &score, &raw_score, &hash,
                      &boolean_value, &failed_value, &attempts,
                      &extra) == 7)
        {
          FAR struct aivlog_cloud_asset_s *asset;

          if (assets >=
                CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_MAX_ASSETS ||
              sequence != (unsigned int)assets + 1 ||
              score < 0 || score > 100 || raw_score != raw_score ||
              raw_score < 0.0f || raw_score > 100.0f ||
              (boolean_value != 0 && boolean_value != 1) ||
              (failed_value != 0 && failed_value != 1) ||
              (boolean_value != 0 && failed_value != 0))
            {
              errno = EPROTO;
              goto fail;
            }

          asset = &manifest->assets[assets++];
          asset->sequence = sequence;
          asset->score = score;
          asset->raw_score = raw_score;
          asset->dhash = (uint64_t)hash;
          asset->uploaded = boolean_value != 0;
          asset->failed = failed_value != 0;
          asset->attempts = attempts;
          any_uploaded |= asset->uploaded;
          all_resolved &= asset->uploaded || asset->failed;
        }
      else
        {
          errno = EPROTO;
          goto fail;
        }
    }

  if (!version_seen || !revision_seen || !session_seen ||
      !uuid_seen || !state_seen || !remote_started_seen || !remote_finished_seen ||
      !count_seen || declared_count != assets ||
      (any_uploaded && !manifest->remote_started) ||
      (manifest->remote_finished &&
       manifest->state != AIVLOG_CLOUD_STATE_COMPLETE) ||
      (manifest->state == AIVLOG_CLOUD_STATE_COMPLETE &&
       (!manifest->remote_started || !manifest->remote_finished ||
        !all_resolved)) ||
      (manifest->state == AIVLOG_CLOUD_STATE_DISCARDED &&
       (assets != 0 || manifest->remote_finished)))
    {
      errno = EPROTO;
      goto fail;
    }

  manifest->asset_count = assets;
  return OK;

fail:
  return ERROR;
}

static int aivlog_cloud_scan_max(FAR uint32_t *maximum)
{
  char root[AIVLOG_CLOUD_PATH_SIZE];
  FAR struct dirent *entry;
  FAR DIR *directory;
  uint32_t session_id;
  uint32_t max = 0;

  if (aivlog_cloud_root_path(root, sizeof(root)) < 0)
    {
      return ERROR;
    }

  directory = opendir(root);
  if (directory == NULL)
    {
      return ERROR;
    }

  errno = 0;
  while ((entry = readdir(directory)) != NULL)
    {
      if (aivlog_cloud_session_name(entry->d_name, &session_id) &&
          session_id > max)
        {
          max = session_id;
        }
    }

  if (errno != 0)
    {
      int saved_errno = errno;
      closedir(directory);
      errno = saved_errno;
      return ERROR;
    }

  closedir(directory);
  *maximum = max;
  return OK;
}

static int aivlog_cloud_recover_active(void)
{
  struct aivlog_cloud_manifest_s manifest;
  char root[AIVLOG_CLOUD_PATH_SIZE];
  FAR struct dirent *entry;
  FAR DIR *directory;
  uint32_t session_id;
  int recovered = 0;

  if (aivlog_cloud_root_path(root, sizeof(root)) < 0)
    {
      return ERROR;
    }

  directory = opendir(root);
  if (directory == NULL)
    {
      return ERROR;
    }

  while ((entry = readdir(directory)) != NULL)
    {
      if (!aivlog_cloud_session_name(entry->d_name, &session_id) ||
          aivlog_cloud_read_manifest(session_id, &manifest) < 0 ||
          manifest.state != AIVLOG_CLOUD_STATE_ACTIVE)
        {
          continue;
        }

      /* No producer exists while the application-level queue is starting.
       * ACTIVE therefore means the previous run reset or crashed.  Close it
       * locally so its uploaded assets can reach the finish endpoint.
       */

      manifest.state = AIVLOG_CLOUD_STATE_FINISHED;
      if (aivlog_cloud_write_manifest(&manifest) == OK)
        {
          printf("[aivlog-cloud] recovered interrupted session %08lu\n",
                 (unsigned long)session_id);
          recovered++;
        }
      else
        {
          printf("[aivlog-cloud] failed to recover session %08lu: %d\n",
                 (unsigned long)session_id, errno);
        }
    }

  closedir(directory);
  return recovered;
}

static int aivlog_cloud_find_work(FAR struct aivlog_cloud_work_s *work)
{
  struct aivlog_cloud_manifest_s manifest;
  char root[AIVLOG_CLOUD_PATH_SIZE];
  FAR struct dirent *entry;
  FAR DIR *directory;
  uint32_t session_id;
  int i;

  memset(work, 0, sizeof(*work));
  if (aivlog_cloud_root_path(root, sizeof(root)) < 0)
    {
      return ERROR;
    }

  directory = opendir(root);
  if (directory == NULL)
    {
      return ERROR;
    }

  while ((entry = readdir(directory)) != NULL)
    {
      if (!aivlog_cloud_session_name(entry->d_name, &session_id) ||
          aivlog_cloud_read_manifest(session_id, &manifest) < 0 ||
          manifest.state == AIVLOG_CLOUD_STATE_COMPLETE ||
          manifest.state == AIVLOG_CLOUD_STATE_DISCARDED)
        {
          continue;
        }

      work->session_id = session_id;
      snprintf(work->session_uuid, sizeof(work->session_uuid), "%s",
               manifest.session_uuid);
      if (manifest.state == AIVLOG_CLOUD_STATE_ACTIVE &&
          (!g_aivlog_cloud.current_active ||
           g_aivlog_cloud.current_session != session_id ||
           !g_aivlog_cloud.current_accepting))
        {
          work->type = AIVLOG_CLOUD_WORK_LOCAL_CLOSE;
          break;
        }

      if (manifest.asset_count == 0)
        {
          /* Do not create a remote session until the first asset is
           * durable.  A finished empty session has no Vlog to generate;
           * persist a local tombstone so current, recovered, and legacy
           * empty manifests all converge without contacting the server.
           */

          if (manifest.state == AIVLOG_CLOUD_STATE_FINISHED)
            {
              work->type = AIVLOG_CLOUD_WORK_LOCAL_DISCARD;
              break;
            }

          continue;
        }

      if (!manifest.remote_started)
        {
          work->type = AIVLOG_CLOUD_WORK_START;
          break;
        }

      for (i = 0; i < manifest.asset_count; i++)
        {
          if (!manifest.assets[i].uploaded &&
              !manifest.assets[i].failed)
            {
              work->type = AIVLOG_CLOUD_WORK_ASSET;
              work->sequence = manifest.assets[i].sequence;
              work->score = manifest.assets[i].score;
              work->raw_score = manifest.assets[i].raw_score;
              work->dhash = manifest.assets[i].dhash;
              break;
            }
        }

      if (work->type != AIVLOG_CLOUD_WORK_NONE)
        {
          break;
        }

      if (manifest.state == AIVLOG_CLOUD_STATE_FINISHED &&
          !manifest.remote_finished)
        {
          work->type = AIVLOG_CLOUD_WORK_FINISH;
          break;
        }
    }

  closedir(directory);
  return work->type == AIVLOG_CLOUD_WORK_NONE ? ERROR : OK;
}

static bool aivlog_cloud_valid_hex(FAR const char *text, size_t length)
{
  size_t index;

  if (text == NULL || strlen(text) != length)
    {
      return false;
    }

  for (index = 0; index < length; index++)
    {
      char value = text[index];
      if (!((value >= '0' && value <= '9') ||
            (value >= 'a' && value <= 'f') ||
            (value >= 'A' && value <= 'F')))
        {
          return false;
        }
    }

  return true;
}

static int aivlog_cloud_random_hex(FAR char *text, size_t byte_count)
{
  uint8_t random_bytes[16];
  size_t index;

  if (byte_count == 0 || byte_count > sizeof(random_bytes))
    {
      errno = EINVAL;
      return ERROR;
    }

  if (agent_secure_random(random_bytes, byte_count) < 0)
    {
      errno = EIO;
      return ERROR;
    }

  for (index = 0; index < byte_count; index++)
    {
      snprintf(text + index * 2, byte_count * 2 + 1 - index * 2,
               "%02x", random_bytes[index]);
    }

  return OK;
}

static int aivlog_cloud_ensure_device_id(void)
{
  uint8_t random_bytes[16];
  char device_id[AIVLOG_CLOUD_DEVICE_ID_SIZE];
  int index;

  if (claw_config_get(AIVLOG_CLOUD_CONFIG_DEVICE_ID, device_id,
                      sizeof(device_id)) == OK &&
      aivlog_cloud_valid_hex(device_id, sizeof(device_id) - 1))
    {
      return OK;
    }

  if (agent_secure_random(random_bytes, sizeof(random_bytes)) < 0)
    {
      errno = EIO;
      return ERROR;
    }

  for (index = 0; index < (int)sizeof(random_bytes); index++)
    {
      snprintf(device_id + index * 2,
               sizeof(device_id) - (size_t)index * 2,
               "%02x", random_bytes[index]);
    }

  return claw_config_set(AIVLOG_CLOUD_CONFIG_DEVICE_ID, device_id);
}

static int aivlog_cloud_load_config(FAR struct aivlog_cloud_config_s *config)
{
  bool https;
  int ret = ERROR;

  pthread_mutex_lock(&g_aivlog_cloud_config_lock);
  memset(config, 0, sizeof(*config));
  if (claw_config_get(AIVLOG_CLOUD_CONFIG_HOST, config->host,
                      sizeof(config->host)) < 0 ||
      claw_config_get(AIVLOG_CLOUD_CONFIG_TOKEN, config->token,
                      sizeof(config->token)) < 0 ||
      claw_config_get(AIVLOG_CLOUD_CONFIG_DEVICE_ID, config->device_id,
                      sizeof(config->device_id)) < 0)
    {
      errno = ENOENT;
      goto out;
    }

  if (claw_config_get(AIVLOG_CLOUD_CONFIG_SCHEME, config->scheme,
                      sizeof(config->scheme)) < 0)
    {
      snprintf(config->scheme, sizeof(config->scheme), "%s",
               AIVLOG_CLOUD_DEFAULT_SCHEME);
    }

  if (claw_config_get(AIVLOG_CLOUD_CONFIG_PORT, config->port,
                      sizeof(config->port)) < 0)
    {
      snprintf(config->port, sizeof(config->port), "%s",
               strcmp(config->scheme, "http") == 0 ?
               AIVLOG_CLOUD_DEFAULT_HTTP_PORT :
               AIVLOG_CLOUD_DEFAULT_HTTPS_PORT);
    }

  if (claw_config_get(AIVLOG_CLOUD_CONFIG_CERT_PIN, config->cert_pin,
                      sizeof(config->cert_pin)) < 0)
    {
      config->cert_pin[0] = '\0';
    }

  if (claw_config_get(AIVLOG_CLOUD_CONFIG_PATH, config->base_path,
                      sizeof(config->base_path)) < 0)
    {
      snprintf(config->base_path, sizeof(config->base_path), "%s",
               AIVLOG_CLOUD_DEFAULT_PATH);
    }

  https = strcmp(config->scheme, "https") == 0;
  if (!aivlog_cloud_valid_scheme(config->scheme) ||
      !aivlog_cloud_valid_host(config->host) ||
      !aivlog_cloud_valid_port(config->port) ||
      !aivlog_cloud_valid_token(config->token) ||
      !aivlog_cloud_valid_hex(config->device_id,
                              AIVLOG_CLOUD_DEVICE_ID_SIZE - 1) ||
      !aivlog_cloud_valid_path(config->base_path) ||
      (https && strcmp(config->port,
                       AIVLOG_CLOUD_DEFAULT_HTTPS_PORT) != 0) ||
      (!https && (strcmp(config->port,
                         AIVLOG_CLOUD_DEFAULT_HTTP_PORT) != 0 ||
                  !aivlog_cloud_valid_private_ipv4(config->host))) ||
      (https && !aivlog_cloud_valid_hex(
                  config->cert_pin, AIVLOG_CLOUD_CERT_PIN_SIZE - 1)) ||
      (!https && config->cert_pin[0] != '\0'))
    {
      errno = EINVAL;
      goto out;
    }

  ret = OK;

out:
  pthread_mutex_unlock(&g_aivlog_cloud_config_lock);
  return ret;
}

static int aivlog_cloud_read_asset(uint32_t session_id,
                                   unsigned int sequence,
                                   FAR uint8_t **body)
{
  struct stat st;
  char path[AIVLOG_CLOUD_PATH_SIZE];
  char temporary[AIVLOG_CLOUD_PATH_SIZE];
  char directory[AIVLOG_CLOUD_PATH_SIZE];
  FAR uint8_t *buffer;
  size_t offset = 0;
  int fd;

  if (aivlog_cloud_asset_path(session_id, sequence, false,
                              path, sizeof(path)) < 0 ||
      aivlog_cloud_asset_path(session_id, sequence, true,
                              temporary, sizeof(temporary)) < 0 ||
      aivlog_cloud_session_path(session_id, directory,
                                sizeof(directory)) < 0)
    {
      return ERROR;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0 && errno == ENOENT)
    {
      if (rename(temporary, path) == 0)
        {
          if (aivlog_cloud_sync_directory(directory) < 0)
            {
              return ERROR;
            }

          fd = open(path, O_RDONLY);
        }
    }

  if (fd < 0)
    {
      return ERROR;
    }

  if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
      st.st_size != CAMERA_GALLERY_BMP_SIZE)
    {
      close(fd);
      errno = EINVAL;
      return ERROR;
    }

  buffer = malloc(CAMERA_GALLERY_BMP_SIZE);
  if (buffer == NULL)
    {
      close(fd);
      errno = ENOMEM;
      return ERROR;
    }

  while (offset < CAMERA_GALLERY_BMP_SIZE)
    {
      size_t chunk = CAMERA_GALLERY_BMP_SIZE - offset;
      ssize_t nread;

      if (chunk > sizeof(g_aivlog_cloud_sd_io))
        {
          chunk = sizeof(g_aivlog_cloud_sd_io);
        }

      nread = read(fd, g_aivlog_cloud_sd_io, chunk);
      if (nread < 0 && errno == EINTR)
        {
          continue;
        }

      if (nread <= 0)
        {
          int saved_errno = nread == 0 ? EIO : errno;
          close(fd);
          free(buffer);
          errno = saved_errno;
          return ERROR;
        }

      memcpy(buffer + offset, g_aivlog_cloud_sd_io, (size_t)nread);
      offset += (size_t)nread;
    }

  close(fd);
  *body = buffer;
  return OK;
}

static int aivlog_cloud_transport_request(
  FAR const struct aivlog_cloud_config_s *config,
  FAR const char *method, FAR const char *path,
  FAR const vela_header_t *headers, FAR const char *body, size_t body_len,
  FAR char *response, size_t response_size)
{
  if (strcmp(config->scheme, "http") == 0)
    {
      return vela_http_request(config->host, config->port, method, path,
                               headers, body, body_len,
                               response, response_size, NULL);
    }

  return vela_https_request_pinned(config->host, config->port, method, path,
                                   headers, body, body_len, config->cert_pin,
                                   response, response_size, NULL);
}

static int aivlog_cloud_request(FAR const struct aivlog_cloud_config_s *config,
                                FAR const struct aivlog_cloud_work_s *work)
{
  char authorization[AIVLOG_CLOUD_TOKEN_SIZE + 16];
  char remote_session[AIVLOG_CLOUD_DEVICE_ID_SIZE * 2 + 4];
  char idempotency[96];
  char score_text[16];
  char raw_text[24];
  char hash_text[24];
  char path[AIVLOG_CLOUD_PATH_SIZE];
  char json[192];
  char response[AIVLOG_CLOUD_RESPONSE_SIZE];
  FAR uint8_t *body = NULL;
  vela_header_t headers[8];
  int header_count = 0;
  int status;

  snprintf(authorization, sizeof(authorization), "Bearer %s", config->token);
  snprintf(remote_session, sizeof(remote_session), "%s-%s",
           config->device_id, work->session_uuid);
  headers[header_count++] = (vela_header_t){"Authorization", authorization};

  if (work->type == AIVLOG_CLOUD_WORK_START)
    {
      snprintf(idempotency, sizeof(idempotency), "aivlog-%s-start",
               remote_session);
      headers[header_count++] =
        (vela_header_t){"Idempotency-Key", idempotency};
      headers[header_count++] =
        (vela_header_t){"Content-Type", "application/json"};
      headers[header_count] = (vela_header_t){NULL, NULL};
      if (aivlog_cloud_path(path, sizeof(path), "%s/sessions",
                            config->base_path) < 0)
        {
          return ERROR;
        }

      snprintf(json, sizeof(json),
               "{\"session_id\":\"%s\",\"device_id\":\"%s\","
               "\"score_threshold\":%d}",
               remote_session, config->device_id,
               CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_THRESHOLD);
      status = aivlog_cloud_transport_request(
        config, "POST", path, headers, json, strlen(json),
        response, sizeof(response));
    }
  else if (work->type == AIVLOG_CLOUD_WORK_ASSET)
    {
      snprintf(idempotency, sizeof(idempotency), "aivlog-%s-%04u",
               remote_session, work->sequence);
      snprintf(score_text, sizeof(score_text), "%d", work->score);
      snprintf(raw_text, sizeof(raw_text), "%.6f", (double)work->raw_score);
      snprintf(hash_text, sizeof(hash_text), "%016" PRIx64, work->dhash);
      headers[header_count++] =
        (vela_header_t){"Idempotency-Key", idempotency};
      headers[header_count++] =
        (vela_header_t){"Content-Type", "image/bmp"};
      headers[header_count++] =
        (vela_header_t){"X-AIVlog-Score", score_text};
      headers[header_count++] =
        (vela_header_t){"X-AIVlog-Raw-Score", raw_text};
      headers[header_count++] =
        (vela_header_t){"X-AIVlog-DHash", hash_text};
      headers[header_count] = (vela_header_t){NULL, NULL};
      if (aivlog_cloud_path(path, sizeof(path),
                            "%s/sessions/%s/assets/%04u",
                            config->base_path, remote_session,
                            work->sequence) < 0 ||
          aivlog_cloud_read_asset(work->session_id, work->sequence,
                                  &body) < 0)
        {
          return ERROR;
        }

      status = aivlog_cloud_transport_request(
        config, "PUT", path, headers, (FAR const char *)body,
        CAMERA_GALLERY_BMP_SIZE, response, sizeof(response));
      free(body);
    }
  else
    {
      snprintf(idempotency, sizeof(idempotency), "aivlog-%s-finish",
               remote_session);
      headers[header_count++] =
        (vela_header_t){"Idempotency-Key", idempotency};
      headers[header_count++] =
        (vela_header_t){"Content-Type", "application/json"};
      headers[header_count] = (vela_header_t){NULL, NULL};
      if (aivlog_cloud_path(path, sizeof(path),
                            "%s/sessions/%s/finish",
                            config->base_path, remote_session) < 0)
        {
          return ERROR;
        }

      snprintf(json, sizeof(json),
               "{\"session_id\":\"%s\",\"generate_vlog\":true}",
               remote_session);
      status = aivlog_cloud_transport_request(
        config, "POST", path, headers, json, strlen(json),
        response, sizeof(response));
    }

  if (status < 200 || status >= 300)
    {
      printf("[aivlog-cloud] request type=%d session=%08lu asset=%u "
             "failed status=%d\n", work->type,
             (unsigned long)work->session_id, work->sequence, status);
      if (status == VELA_TLS_ERR_VERIFY || status == 401 || status == 403)
        {
          errno = EACCES;
        }
      else if (status >= 400 && status < 500 && status != 408 &&
               status != 425 && status != 429)
        {
          errno = ECANCELED;
        }
      else
        {
          errno = EAGAIN;
        }

      return ERROR;
    }

  printf("[aivlog-cloud] request type=%d session=%08lu asset=%u "
         "status=%d response=%.120s\n", work->type,
         (unsigned long)work->session_id, work->sequence,
         status, response);
  return OK;
}

static int aivlog_cloud_cleanup_completed(
  FAR const struct aivlog_cloud_manifest_s *manifest)
{
  char path[AIVLOG_CLOUD_PATH_SIZE];
  char temporary[AIVLOG_CLOUD_PATH_SIZE];
  char directory[AIVLOG_CLOUD_PATH_SIZE];
  int i;

  for (i = 0; i < manifest->asset_count; i++)
    {
      unsigned int sequence = manifest->assets[i].sequence;

      if (aivlog_cloud_asset_path(manifest->session_id, sequence, false,
                                  path, sizeof(path)) < 0 ||
          aivlog_cloud_asset_path(manifest->session_id, sequence, true,
                                  temporary, sizeof(temporary)) < 0)
        {
          return ERROR;
        }

      if (unlink(path) < 0 && errno != ENOENT)
        {
          return ERROR;
        }
      if (unlink(temporary) < 0 && errno != ENOENT)
        {
          return ERROR;
        }
    }

  if (aivlog_cloud_session_path(manifest->session_id, directory,
                                sizeof(directory)) < 0)
    {
      return ERROR;
    }

  return aivlog_cloud_sync_directory(directory);
}

static int aivlog_cloud_complete_work(
  FAR const struct aivlog_cloud_work_s *work, bool succeeded,
  bool permanent_failure)
{
  struct aivlog_cloud_manifest_s manifest;
  int i;

  if (aivlog_cloud_read_manifest(work->session_id, &manifest) < 0)
    {
      return ERROR;
    }

  if (work->type == AIVLOG_CLOUD_WORK_LOCAL_DISCARD)
    {
      if (manifest.asset_count != 0)
        {
          errno = EBUSY;
          return ERROR;
        }

      manifest.state = AIVLOG_CLOUD_STATE_DISCARDED;
      if (aivlog_cloud_write_manifest(&manifest) < 0)
        {
          return ERROR;
        }

      if (g_aivlog_cloud.current_active &&
          g_aivlog_cloud.current_session == work->session_id)
        {
          g_aivlog_cloud.current_active = false;
        }

      printf("[aivlog-cloud] discarded empty session %08lu locally\n",
             (unsigned long)work->session_id);
      return OK;
    }

  if (work->type == AIVLOG_CLOUD_WORK_LOCAL_CLOSE)
    {
      manifest.state = AIVLOG_CLOUD_STATE_FINISHED;
      if (aivlog_cloud_write_manifest(&manifest) < 0)
        {
          return ERROR;
        }

      if (g_aivlog_cloud.current_active &&
          g_aivlog_cloud.current_session == work->session_id)
        {
          g_aivlog_cloud.current_active = false;
        }

      printf("[aivlog-cloud] session %08lu closed locally by queue\n",
             (unsigned long)work->session_id);
      return OK;
    }

  if (work->type == AIVLOG_CLOUD_WORK_START)
    {
      if (succeeded)
        {
          manifest.remote_started = true;
        }
    }
  else if (work->type == AIVLOG_CLOUD_WORK_ASSET)
    {
      for (i = 0; i < manifest.asset_count; i++)
        {
          if (manifest.assets[i].sequence == work->sequence)
            {
              if (succeeded)
                {
                  manifest.assets[i].uploaded = true;
                }
              else
                {
                  manifest.assets[i].attempts++;
                  manifest.assets[i].failed = permanent_failure;
                }

              break;
            }
        }

      if (i == manifest.asset_count)
        {
          errno = ENOENT;
          return ERROR;
        }
    }
  else if (succeeded)
    {
      manifest.remote_finished = true;
      manifest.state = AIVLOG_CLOUD_STATE_COMPLETE;
    }

  if (aivlog_cloud_write_manifest(&manifest) < 0)
    {
      return ERROR;
    }

  if (manifest.state == AIVLOG_CLOUD_STATE_COMPLETE)
    {
      return aivlog_cloud_cleanup_completed(&manifest);
    }

  return OK;
}

static void aivlog_cloud_wait(unsigned int seconds)
{
  struct timespec deadline;

  if (clock_gettime(CLOCK_REALTIME, &deadline) < 0)
    {
      sleep(seconds);
      return;
    }

  deadline.tv_sec += seconds;
  pthread_mutex_lock(&g_aivlog_cloud.lock);
  if (!g_aivlog_cloud.stopping)
    {
      pthread_cond_timedwait(&g_aivlog_cloud.cond,
                             &g_aivlog_cloud.lock, &deadline);
    }

  pthread_mutex_unlock(&g_aivlog_cloud.lock);
}

static unsigned int aivlog_cloud_retry_delay(void)
{
  unsigned int delay = CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_RETRY_SEC;
  unsigned int retry = g_aivlog_cloud.retry_count;

  while (retry-- > 1 &&
         delay < CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_RETRY_MAX_SEC)
    {
      if (delay >
          CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_RETRY_MAX_SEC / 2)
        {
          delay = CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_RETRY_MAX_SEC;
          break;
        }

      delay *= 2;
    }

  if (delay > CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_RETRY_MAX_SEC)
    {
      delay = CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_RETRY_MAX_SEC;
    }

  return delay;
}

static FAR void *aivlog_cloud_worker(FAR void *argument)
{
  struct aivlog_cloud_config_s config;
  struct aivlog_cloud_work_s work;
  bool stopping;
  bool blocked;
  bool permanent_failure;
  int request_errno;
  int request_ret;

  (void)argument;
  for (; ; )
    {
      pthread_mutex_lock(&g_aivlog_cloud.lock);
      stopping = g_aivlog_cloud.stopping;
      blocked = g_aivlog_cloud.cloud_blocked;
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      if (stopping)
        {
          break;
        }

      pthread_mutex_lock(&g_aivlog_cloud.lock);
      if (aivlog_cloud_find_work(&work) < 0)
        {
          pthread_mutex_unlock(&g_aivlog_cloud.lock);
          aivlog_cloud_wait(AIVLOG_CLOUD_IDLE_SECONDS);
          continue;
        }

      if (work.type == AIVLOG_CLOUD_WORK_LOCAL_CLOSE ||
          work.type == AIVLOG_CLOUD_WORK_LOCAL_DISCARD)
        {
          int local_ret = aivlog_cloud_complete_work(&work, true, false);

          pthread_mutex_unlock(&g_aivlog_cloud.lock);
          if (local_ret < 0)
            {
              printf("[aivlog-cloud] local queue update retry failed: %d\n",
                     errno);
              g_aivlog_cloud.retry_count++;
              aivlog_cloud_wait(aivlog_cloud_retry_delay());
            }
          else
            {
              g_aivlog_cloud.retry_count = 0;
            }

          continue;
        }

      if (blocked)
        {
          pthread_mutex_unlock(&g_aivlog_cloud.lock);
          aivlog_cloud_wait(AIVLOG_CLOUD_IDLE_SECONDS);
          continue;
        }

      if (aivlog_cloud_load_config(&config) < 0)
        {
          explicit_bzero(&config, sizeof(config));
          pthread_mutex_unlock(&g_aivlog_cloud.lock);
          aivlog_cloud_wait(AIVLOG_CLOUD_IDLE_SECONDS);
          continue;
        }

      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      if (!network_is_connected() && network_wifi_reconnect() < 0)
        {
          explicit_bzero(&config, sizeof(config));
          printf("[aivlog-cloud] network unavailable, queue retained\n");
          g_aivlog_cloud.retry_count++;
          aivlog_cloud_wait(aivlog_cloud_retry_delay());
          continue;
        }

      request_ret = aivlog_cloud_request(&config, &work);
      explicit_bzero(&config, sizeof(config));
      if (request_ret < 0)
        {
          int completion_ret;

          request_errno = errno;
          if (request_errno == EACCES || request_errno == ECANCELED)
            {
              pthread_mutex_lock(&g_aivlog_cloud.lock);
              g_aivlog_cloud.cloud_blocked = true;
              pthread_mutex_unlock(&g_aivlog_cloud.lock);
              printf("[aivlog-cloud] queue paused until backend config "
                     "changes (errno=%d)\n", request_errno);
              g_aivlog_cloud.retry_count = 0;
              continue;
            }

          permanent_failure = work.type == AIVLOG_CLOUD_WORK_ASSET &&
                              (request_errno == ENOENT ||
                               request_errno == EINVAL ||
                               request_errno == EFBIG);
          pthread_mutex_lock(&g_aivlog_cloud.lock);
          completion_ret = aivlog_cloud_complete_work(
            &work, false, permanent_failure);
          pthread_mutex_unlock(&g_aivlog_cloud.lock);

          if (completion_ret < 0 || !permanent_failure)
            {
              g_aivlog_cloud.retry_count++;
              aivlog_cloud_wait(aivlog_cloud_retry_delay());
            }
          else
            {
              g_aivlog_cloud.retry_count = 0;
            }

          continue;
        }

      pthread_mutex_lock(&g_aivlog_cloud.lock);
      if (aivlog_cloud_complete_work(&work, true, false) < 0)
        {
          printf("[aivlog-cloud] persist completion failed: %d\n", errno);
          pthread_mutex_unlock(&g_aivlog_cloud.lock);
          g_aivlog_cloud.retry_count++;
          aivlog_cloud_wait(aivlog_cloud_retry_delay());
          continue;
        }

      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      g_aivlog_cloud.retry_count = 0;
    }

  return NULL;
}

static uint16_t aivlog_cloud_luma(uint16_t pixel)
{
  uint16_t red = ((pixel >> 11) & 0x1f) << 1;
  uint16_t green = (pixel >> 5) & 0x3f;
  uint16_t blue = (pixel & 0x1f) << 1;

  return red * 77 + green * 150 + blue * 29;
}

static uint64_t aivlog_cloud_dhash(FAR const uint16_t *pixels)
{
  uint64_t hash = 0;
  int row;
  int column;

  for (row = 0; row < 8; row++)
    {
      int y = row * (CAMERA_GALLERY_HEIGHT - 1) / 7;

      for (column = 0; column < 8; column++)
        {
          int left_x = column * (CAMERA_GALLERY_WIDTH - 1) / 8;
          int right_x = (column + 1) * (CAMERA_GALLERY_WIDTH - 1) / 8;
          uint16_t left = aivlog_cloud_luma(
            pixels[y * CAMERA_GALLERY_WIDTH + left_x]);
          uint16_t right = aivlog_cloud_luma(
            pixels[y * CAMERA_GALLERY_WIDTH + right_x]);

          hash <<= 1;
          if (left < right)
            {
              hash |= 1;
            }
        }
    }

  return hash;
}

static unsigned int aivlog_cloud_hamming(uint64_t value)
{
  unsigned int count = 0;

  while (value != 0)
    {
      value &= value - 1;
      count++;
    }

  return count;
}

static bool aivlog_cloud_cooldown_elapsed(FAR const struct timespec *now)
{
  int64_t milliseconds;

  milliseconds = (int64_t)(now->tv_sec -
                 g_aivlog_cloud.last_accept.tv_sec) * 1000 +
                 ((int64_t)now->tv_nsec -
                  g_aivlog_cloud.last_accept.tv_nsec) / 1000000;
  return milliseconds >=
         CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_COOLDOWN_MS;
}

int camera_gallery_aivlog_cloud_initialize(void)
{
  struct sched_param parameter;
  pthread_attr_t attributes;
  char root[AIVLOG_CLOUD_PATH_SIZE];
  bool attributes_initialized = false;
  int ret;

  if (g_aivlog_cloud.initialized)
    {
      return OK;
    }

  if (aivlog_cloud_root_path(root, sizeof(root)) < 0 ||
      (mkdir(root, 0777) < 0 && errno != EEXIST) ||
      config_store_init() < 0 || aivlog_cloud_ensure_device_id() < 0)
    {
      return ERROR;
    }

  memset(&g_aivlog_cloud, 0, sizeof(g_aivlog_cloud));
  ret = pthread_mutex_init(&g_aivlog_cloud.lock, NULL);
  if (ret != 0)
    {
      errno = ret;
      return ERROR;
    }

  ret = pthread_cond_init(&g_aivlog_cloud.cond, NULL);
  if (ret != 0)
    {
      pthread_mutex_destroy(&g_aivlog_cloud.lock);
      errno = ret;
      return ERROR;
    }

  g_aivlog_cloud.initialized = true;
  if (aivlog_cloud_recover_active() < 0)
    {
      printf("[aivlog-cloud] interrupted-session scan failed: %d\n", errno);
    }

  ret = pthread_attr_init(&attributes);
  if (ret == 0)
    {
      attributes_initialized = true;
      ret = pthread_attr_setstacksize(
        &attributes,
        CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_STACKSIZE);
    }

  if (ret == 0)
    {
      memset(&parameter, 0, sizeof(parameter));
      parameter.sched_priority =
        CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_PRIORITY;
      ret = pthread_attr_setschedparam(&attributes, &parameter);
    }

  if (ret == 0)
    {
      ret = pthread_attr_setinheritsched(&attributes,
                                         PTHREAD_EXPLICIT_SCHED);
    }

  if (ret == 0)
    {
      ret = pthread_create(&g_aivlog_cloud.thread, &attributes,
                           aivlog_cloud_worker, NULL);
    }

  if (attributes_initialized)
    {
      pthread_attr_destroy(&attributes);
    }

  if (ret != 0)
    {
      errno = ret;
      camera_gallery_aivlog_cloud_finalize();
      return ERROR;
    }

  g_aivlog_cloud.thread_started = true;
  printf("[aivlog-cloud] queue ready threshold=%d cooldown_ms=%d "
         "max_assets=%d priority=%d\n",
         CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_THRESHOLD,
         CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_COOLDOWN_MS,
         CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_MAX_ASSETS,
         CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_PRIORITY);
  return OK;
}

void camera_gallery_aivlog_cloud_finalize(void)
{
  if (!g_aivlog_cloud.initialized)
    {
      return;
    }

  camera_gallery_aivlog_session_finish();
  pthread_mutex_lock(&g_aivlog_cloud.lock);
  g_aivlog_cloud.stopping = true;
  pthread_cond_signal(&g_aivlog_cloud.cond);
  pthread_mutex_unlock(&g_aivlog_cloud.lock);
  if (g_aivlog_cloud.thread_started)
    {
      pthread_join(g_aivlog_cloud.thread, NULL);
    }

  pthread_cond_destroy(&g_aivlog_cloud.cond);
  pthread_mutex_destroy(&g_aivlog_cloud.lock);
  memset(&g_aivlog_cloud, 0, sizeof(g_aivlog_cloud));
}

int camera_gallery_aivlog_session_start(void)
{
  struct aivlog_cloud_manifest_s manifest;
  struct aivlog_cloud_work_s close_work;
  char session_path[AIVLOG_CLOUD_PATH_SIZE];
  char root[AIVLOG_CLOUD_PATH_SIZE];
  uint32_t maximum = 0;
  uint32_t session_id;

  if (!g_aivlog_cloud.initialized &&
      camera_gallery_aivlog_cloud_initialize() < 0)
    {
      return ERROR;
    }

  pthread_mutex_lock(&g_aivlog_cloud.lock);
  if (g_aivlog_cloud.current_active &&
      !g_aivlog_cloud.current_accepting)
    {
      memset(&close_work, 0, sizeof(close_work));
      close_work.type = AIVLOG_CLOUD_WORK_LOCAL_CLOSE;
      close_work.session_id = g_aivlog_cloud.current_session;
      aivlog_cloud_complete_work(&close_work, true, false);
    }

  if (g_aivlog_cloud.current_active)
    {
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      errno = EBUSY;
      return ERROR;
    }

  if (aivlog_cloud_scan_max(&maximum) < 0 || maximum == UINT32_MAX)
    {
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      if (maximum == UINT32_MAX)
        {
          errno = ENOSPC;
        }
      return ERROR;
    }

  session_id = maximum + 1;
  if (aivlog_cloud_session_path(session_id, session_path,
                                sizeof(session_path)) < 0 ||
      aivlog_cloud_root_path(root, sizeof(root)) < 0 ||
      mkdir(session_path, 0777) < 0)
    {
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      return ERROR;
    }

  if (aivlog_cloud_sync_directory(root) < 0)
    {
      rmdir(session_path);
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      return ERROR;
    }

  memset(&manifest, 0, sizeof(manifest));
  manifest.session_id = session_id;
  if (aivlog_cloud_random_hex(manifest.session_uuid, 16) < 0)
    {
      rmdir(session_path);
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      return ERROR;
    }

  manifest.state = AIVLOG_CLOUD_STATE_ACTIVE;
  if (aivlog_cloud_write_manifest(&manifest) < 0)
    {
      rmdir(session_path);
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      return ERROR;
    }

  g_aivlog_cloud.current_session = session_id;
  g_aivlog_cloud.current_active = true;
  g_aivlog_cloud.current_accepting = true;
  g_aivlog_cloud.has_last_accept = false;
  pthread_cond_signal(&g_aivlog_cloud.cond);
  pthread_mutex_unlock(&g_aivlog_cloud.lock);
  printf("[aivlog-cloud] session %08lu started\n",
         (unsigned long)session_id);
  return OK;
}

int camera_gallery_aivlog_session_candidate(
  FAR const uint16_t *pixels,
  FAR const struct camera_gallery_aivlog_score_s *result)
{
  struct aivlog_cloud_manifest_s manifest;
  struct aivlog_cloud_asset_s *asset;
  struct timespec now;
  char path[AIVLOG_CLOUD_PATH_SIZE];
  char temporary[AIVLOG_CLOUD_PATH_SIZE];
  char directory[AIVLOG_CLOUD_PATH_SIZE];
  uint64_t dhash;
  unsigned int distance = UINT_MAX;
  unsigned int sequence;
  int saved_errno;

  if (pixels == NULL || result == NULL)
    {
      errno = EINVAL;
      return ERROR;
    }

  /* Initialization can fail when the SD card is unavailable.  The scorer
   * must continue without touching an uninitialized mutex in that case.
   */

  if (!g_aivlog_cloud.initialized)
    {
      errno = ENODEV;
      return ERROR;
    }

  if (result->score <
      CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_THRESHOLD)
    {
      return OK;
    }

  dhash = aivlog_cloud_dhash(pixels);
  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    {
      return ERROR;
    }

  pthread_mutex_lock(&g_aivlog_cloud.lock);
  if (!g_aivlog_cloud.initialized ||
      !g_aivlog_cloud.current_accepting)
    {
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      errno = ENODEV;
      return ERROR;
    }

  if (g_aivlog_cloud.has_last_accept)
    {
      distance = aivlog_cloud_hamming(dhash ^ g_aivlog_cloud.last_dhash);
      if (!aivlog_cloud_cooldown_elapsed(&now) ||
          distance <
            CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_DHASH_DISTANCE)
        {
          pthread_mutex_unlock(&g_aivlog_cloud.lock);
          return OK;
        }
    }

  if (aivlog_cloud_read_manifest(g_aivlog_cloud.current_session,
                                 &manifest) < 0)
    {
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      return ERROR;
    }

  if (manifest.asset_count >=
      CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD_MAX_ASSETS)
    {
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      errno = ENOSPC;
      return ERROR;
    }

  sequence = (unsigned int)manifest.asset_count + 1;
  if (aivlog_cloud_asset_path(manifest.session_id, sequence, false,
                              path, sizeof(path)) < 0 ||
      aivlog_cloud_asset_path(manifest.session_id, sequence, true,
                              temporary, sizeof(temporary)) < 0 ||
      aivlog_cloud_session_path(manifest.session_id, directory,
                                sizeof(directory)) < 0)
    {
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      return ERROR;
    }

  if (camera_gallery_bmp_save(temporary, pixels) < 0)
    {
      saved_errno = errno;
      unlink(temporary);
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      errno = saved_errno;
      return ERROR;
    }

  asset = &manifest.assets[manifest.asset_count++];
  asset->sequence = sequence;
  asset->score = result->score;
  asset->raw_score = result->raw_score;
  asset->dhash = dhash;
  if (aivlog_cloud_write_manifest(&manifest) < 0)
    {
      saved_errno = errno;
      unlink(temporary);
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      errno = saved_errno;
      return ERROR;
    }

  if (rename(temporary, path) < 0)
    {
      /* The committed manifest intentionally remains.  The uploader will
       * recover the matching .tmp file before reading this asset. */

      saved_errno = errno;
      pthread_cond_signal(&g_aivlog_cloud.cond);
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      errno = saved_errno;
      return ERROR;
    }

  if (aivlog_cloud_sync_directory(directory) < 0)
    {
      saved_errno = errno;
      pthread_cond_signal(&g_aivlog_cloud.cond);
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      errno = saved_errno;
      return ERROR;
    }

  g_aivlog_cloud.last_accept = now;
  g_aivlog_cloud.last_dhash = dhash;
  g_aivlog_cloud.has_last_accept = true;
  pthread_cond_signal(&g_aivlog_cloud.cond);
  pthread_mutex_unlock(&g_aivlog_cloud.lock);
  printf("[aivlog-cloud] queued session=%08lu asset=%04u raw=%.6f "
         "score=%d dhash=%016" PRIx64 " distance=%u\n",
         (unsigned long)manifest.session_id, sequence,
         (double)result->raw_score, result->score, dhash, distance);
  return OK;
}

void camera_gallery_aivlog_session_finish(void)
{
  struct aivlog_cloud_manifest_s manifest;
  uint32_t session_id;

  if (!g_aivlog_cloud.initialized)
    {
      return;
    }

  pthread_mutex_lock(&g_aivlog_cloud.lock);
  if (!g_aivlog_cloud.current_active)
    {
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      return;
    }

  session_id = g_aivlog_cloud.current_session;
  g_aivlog_cloud.current_accepting = false;
  if (aivlog_cloud_read_manifest(session_id, &manifest) < 0)
    {
      printf("[aivlog-cloud] finish read failed session=%08lu errno=%d\n",
             (unsigned long)session_id, errno);
      pthread_cond_signal(&g_aivlog_cloud.cond);
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      return;
    }

  manifest.state = AIVLOG_CLOUD_STATE_FINISHED;
  if (aivlog_cloud_write_manifest(&manifest) < 0)
    {
      printf("[aivlog-cloud] finish persist failed session=%08lu "
             "errno=%d\n", (unsigned long)session_id, errno);
      pthread_cond_signal(&g_aivlog_cloud.cond);
      pthread_mutex_unlock(&g_aivlog_cloud.lock);
      return;
    }

  /* current_accepting was cleared before I/O, so a failed FINISHED commit
   * can be retried by the queue without merging the next UI session.
   */

  g_aivlog_cloud.current_active = false;
  pthread_cond_signal(&g_aivlog_cloud.cond);
  pthread_mutex_unlock(&g_aivlog_cloud.lock);
  printf("[aivlog-cloud] session %08lu finished locally\n",
         (unsigned long)session_id);
}

void camera_gallery_aivlog_cloud_wake(void)
{
  if (!g_aivlog_cloud.initialized)
    {
      return;
    }

  pthread_mutex_lock(&g_aivlog_cloud.lock);
  g_aivlog_cloud.cloud_blocked = false;
  pthread_cond_signal(&g_aivlog_cloud.cond);
  pthread_mutex_unlock(&g_aivlog_cloud.lock);
}

static bool aivlog_cloud_valid_host(FAR const char *host)
{
  FAR const char *cursor;

  if (host == NULL || host[0] == '\0' || strlen(host) >=
      AIVLOG_CLOUD_HOST_SIZE || strstr(host, "://") != NULL)
    {
      return false;
    }

  for (cursor = host; *cursor != '\0'; cursor++)
    {
      if (*cursor == '/' || *cursor == ':' || *cursor == ' ' ||
          *cursor == '\t')
        {
          return false;
        }
    }

  return true;
}

static bool aivlog_cloud_valid_pin(FAR const char *pin)
{
  return aivlog_cloud_valid_hex(pin, AIVLOG_CLOUD_CERT_PIN_SIZE - 1);
}

static bool aivlog_cloud_valid_scheme(FAR const char *scheme)
{
  return scheme != NULL &&
         (strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0);
}

static bool aivlog_cloud_valid_token(FAR const char *token)
{
  FAR const unsigned char *cursor = (FAR const unsigned char *)token;
  size_t length;

  if (token == NULL)
    {
      return false;
    }

  length = strlen(token);
  if (length < 32 || length >= AIVLOG_CLOUD_TOKEN_SIZE)
    {
      return false;
    }

  while (*cursor != '\0')
    {
      if (!isalnum(*cursor) && *cursor != '-' && *cursor != '_' &&
          *cursor != '.' && *cursor != '~')
        {
          return false;
        }

      cursor++;
    }

  return true;
}

static bool aivlog_cloud_valid_private_ipv4(FAR const char *host)
{
  struct in_addr address;
  FAR const uint8_t *bytes;

  if (host == NULL || inet_pton(AF_INET, host, &address) != 1)
    {
      return false;
    }

  bytes = (FAR const uint8_t *)&address.s_addr;
  return bytes[0] == 10 ||
         (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) ||
         (bytes[0] == 192 && bytes[1] == 168);
}

static bool aivlog_cloud_valid_port(FAR const char *port)
{
  FAR char *end;
  unsigned long value;

  if (port == NULL || port[0] == '\0')
    {
      return false;
    }

  errno = 0;
  value = strtoul(port, &end, 10);
  return errno == 0 && *end == '\0' && value >= 1 && value <= 65535;
}

static bool aivlog_cloud_valid_path(FAR const char *path)
{
  return path != NULL && path[0] == '/' &&
         strlen(path) < AIVLOG_CLOUD_BASE_PATH_SIZE &&
         strstr(path, "..") == NULL && strchr(path, ' ') == NULL;
}

int camera_gallery_aivlog_cloud_configure(FAR const char *scheme,
                                          FAR const char *host,
                                          uint16_t port,
                                          FAR const char *token,
                                          FAR const char *cert_pin,
                                          FAR const char *path)
{
  struct claw_config_pair_s pairs[6];
  char port_text[AIVLOG_CLOUD_PORT_SIZE];
  bool https;
  int length;

  if (!aivlog_cloud_valid_scheme(scheme) ||
      !aivlog_cloud_valid_host(host) || port == 0 ||
      !aivlog_cloud_valid_token(token) ||
      cert_pin == NULL || !aivlog_cloud_valid_path(path))
    {
      errno = EINVAL;
      return ERROR;
    }

  https = strcmp(scheme, "https") == 0;
  if ((https && (port != 443 || !aivlog_cloud_valid_pin(cert_pin))) ||
      (!https && (port != 8080 || cert_pin[0] != '\0' ||
                  !aivlog_cloud_valid_private_ipv4(host))))
    {
      errno = EINVAL;
      return ERROR;
    }

  length = snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
  if (length < 1 || (size_t)length >= sizeof(port_text))
    {
      errno = EINVAL;
      return ERROR;
    }

  pairs[0] = (struct claw_config_pair_s)
    {AIVLOG_CLOUD_CONFIG_SCHEME, scheme};
  pairs[1] = (struct claw_config_pair_s)
    {AIVLOG_CLOUD_CONFIG_HOST, host};
  pairs[2] = (struct claw_config_pair_s)
    {AIVLOG_CLOUD_CONFIG_PORT, port_text};
  pairs[3] = (struct claw_config_pair_s)
    {AIVLOG_CLOUD_CONFIG_TOKEN, token};
  pairs[4] = (struct claw_config_pair_s)
    {AIVLOG_CLOUD_CONFIG_CERT_PIN, cert_pin};
  pairs[5] = (struct claw_config_pair_s)
    {AIVLOG_CLOUD_CONFIG_PATH, path};

  pthread_mutex_lock(&g_aivlog_cloud_config_lock);
  if (config_store_init() < 0)
    {
      printf("[aivlog-cloud] config store init failed errno=%d\n", errno);
      pthread_mutex_unlock(&g_aivlog_cloud_config_lock);
      return ERROR;
    }

  if (aivlog_cloud_ensure_device_id() < 0)
    {
      printf("[aivlog-cloud] device id persistence failed errno=%d\n",
             errno);
      pthread_mutex_unlock(&g_aivlog_cloud_config_lock);
      return ERROR;
    }

  if (claw_config_set_many(pairs,
                           sizeof(pairs) / sizeof(pairs[0])) < 0)
    {
      printf("[aivlog-cloud] backend persistence failed errno=%d\n", errno);
      pthread_mutex_unlock(&g_aivlog_cloud_config_lock);
      return ERROR;
    }

  pthread_mutex_unlock(&g_aivlog_cloud_config_lock);

  camera_gallery_aivlog_cloud_wake();
  return OK;
}

int camera_gallery_aivlog_cloud_command(int argc, FAR char *argv[])
{
  struct aivlog_cloud_config_s config;
  FAR const char *base_path = AIVLOG_CLOUD_DEFAULT_PATH;

  if (argc >= 2 && strcmp(argv[1], "set_aivlog_backend") == 0)
    {
      FAR const char *cert_pin;
      unsigned long port;

      if (argc != 7 && argc != 8)
        {
          printf("Usage: camera_gallery set_aivlog_backend "
                 "<http|https> <host> <port> <token> "
                 "<cert_sha256|-> [base_path]\n");
          return EXIT_FAILURE;
        }

      if (argc == 8)
        {
          base_path = argv[7];
        }

      cert_pin = strcmp(argv[6], "-") == 0 ? "" : argv[6];
      if (!aivlog_cloud_valid_port(argv[4]))
        {
          return EXIT_FAILURE;
        }

      port = strtoul(argv[4], NULL, 10);
      if (camera_gallery_aivlog_cloud_configure(
            argv[2], argv[3], (uint16_t)port, argv[5], cert_pin,
            base_path) < 0)
        {
          printf("camera_gallery: failed to save AI Vlog backend: %d\n",
                 errno);
          return EXIT_FAILURE;
        }

      printf("AI Vlog backend configured: %s://%s:%lu%s "
             "(credentials protected)\n",
             argv[2], argv[3], port, base_path);
      return EXIT_SUCCESS;
    }

  if (argc >= 2 && strcmp(argv[1], "set_aivlog_cloud") == 0)
    {
      if ((argc != 5 && argc != 6) ||
          (argc == 6 && !aivlog_cloud_valid_path(argv[5])) ||
          camera_gallery_aivlog_cloud_configure(
            "https", argv[2], 443, argv[3], argv[4],
            argc == 6 ? argv[5] : base_path) < 0)
        {
          printf("Usage: camera_gallery set_aivlog_cloud "
                 "<host> <token> <cert_sha256> [base_path]\n");
          return EXIT_FAILURE;
        }

      printf("AI Vlog cloud configured: https://%s:443%s\n",
             argv[2], argc == 6 ? argv[5] : base_path);
      return EXIT_SUCCESS;
    }

  if (argc == 2 && strcmp(argv[1], "clear_aivlog_cloud") == 0)
    {
      if (config_store_init() < 0 ||
          config_del(AIVLOG_CLOUD_CONFIG_SCHEME) < 0 ||
          config_del(AIVLOG_CLOUD_CONFIG_HOST) < 0 ||
          config_del(AIVLOG_CLOUD_CONFIG_PORT) < 0 ||
          config_del(AIVLOG_CLOUD_CONFIG_TOKEN) < 0 ||
          config_del(AIVLOG_CLOUD_CONFIG_CERT_PIN) < 0 ||
          config_del(AIVLOG_CLOUD_CONFIG_PATH) < 0)
        {
          return EXIT_FAILURE;
        }

      camera_gallery_aivlog_cloud_wake();
      printf("AI Vlog cloud configuration cleared; local queue retained.\n");
      return EXIT_SUCCESS;
    }

  if (argc == 2 && strcmp(argv[1], "aivlog_cloud_status") == 0)
    {
      if (config_store_init() < 0 || aivlog_cloud_load_config(&config) < 0)
        {
          printf("AI Vlog backend: not configured (local queue only)\n");
        }
      else
        {
          printf("AI Vlog backend: %s://%s:%s%s "
                 "(credentials configured)\n",
                 config.scheme, config.host, config.port, config.base_path);
        }

      return EXIT_SUCCESS;
    }

  return -ENOSYS;
}
