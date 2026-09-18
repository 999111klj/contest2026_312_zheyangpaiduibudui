/****************************************************************************
 * apps/examples/camera_gallery/camera_gallery_main.c
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <nuttx/input/buttons.h>
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_ADC_BUTTONS
#  include <nuttx/analog/adc.h>
#  include <nuttx/analog/ioctl.h>
#endif
#include <nuttx/video/video.h>
#include <nuttx/video/v4l2_cap.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "camera_gallery.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAMERA_BUFFER_COUNT    3
#define BUTTON_STABLE_COUNT    2
#define BUTTON_SAMPLE_US       10000
#define MAX_GALLERY_IMAGES     10000
#define GALLERY_PATH_SIZE      128
#define SD_MOUNT_RETRY_COUNT   50
#define SD_MOUNT_RETRY_US      200000
#define SD_MOUNT_LOG_INTERVAL  10

/* ADC ladder boundaries in millivolts.  Values measured on this board are
 * approximately 2914 mV released, 2148 mV MENU, 1853 mV PLAY,
 * 1089 mV UP, and 747 mV DOWN.  Each boundary is the rounded midpoint
 * between adjacent measured levels.
 */

#define BUTTON_RELEASE_MILLIVOLTS  2530
#define BUTTON_MENU_MILLIVOLTS     2000
#define BUTTON_PLAY_MILLIVOLTS     1470
#define BUTTON_UP_MILLIVOLTS        918

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum camera_gallery_button_action_e
{
  CAMERA_GALLERY_BUTTON_NONE = 0,
  CAMERA_GALLERY_BUTTON_PRESS
};

enum camera_gallery_home_selection_e
{
  CAMERA_GALLERY_HOME_CAMERA = 0,
  CAMERA_GALLERY_HOME_ALBUM,
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
  CAMERA_GALLERY_HOME_AIVLOG,
#endif
  CAMERA_GALLERY_HOME_COUNT
};

struct camera_gallery_button_event_s
{
  btn_buttonset_t button;
  enum camera_gallery_button_action_e action;
};

struct camera_buffer_s
{
  FAR uint16_t *start;
  size_t length;
};

struct camera_gallery_app_s
{
  enum camera_gallery_state_e state;
  struct camera_buffer_s buffers[CAMERA_BUFFER_COUNT];
  FAR uint16_t *gallery_pixels;
  FAR unsigned int *numbers;
  btn_buttonset_t button_candidate;
  btn_buttonset_t button_stable;
  btn_buttonset_t button_pressed_id;
  int camera_fd;
  int buttons_fd;
  int adc_fd;
  int button_count;
  int image_count;
  int image_index;
  int home_selection;
  bool button_initialized;
  bool button_pressed;
  bool streaming;
  bool sd_ready;
  bool sd_mounted_here;
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_CLOUD_SCORE
  bool cloud_ready;
#endif
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_camera_gallery_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_camera_gallery_running;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int camera_gallery_make_path(FAR const char *path)
{
  struct stat st;
  char copy[GALLERY_PATH_SIZE];
  FAR char *separator;
  int ret;

  ret = snprintf(copy, sizeof(copy), "%s", path);
  if (ret < 0 || (size_t)ret >= sizeof(copy))
    {
      errno = ENAMETOOLONG;
      return ERROR;
    }

  for (separator = copy + 1; *separator != '\0'; separator++)
    {
      if (*separator != '/')
        {
          continue;
        }

      *separator = '\0';
      if (mkdir(copy, 0777) < 0 && errno != EEXIST)
        {
          return ERROR;
        }

      *separator = '/';
    }

  if (mkdir(copy, 0777) < 0 && errno != EEXIST)
    {
      return ERROR;
    }

  if (stat(copy, &st) < 0 || !S_ISDIR(st.st_mode))
    {
      errno = ENOTDIR;
      return ERROR;
    }

  return OK;
}

static int camera_gallery_sd_initialize(FAR struct camera_gallery_app_s *app)
{
  struct stat st;
  struct statfs fs;
  char dcim[GALLERY_PATH_SIZE];
  int mount_errno = 0;
  int retry;
  int ret;

  if (camera_gallery_make_path(
        CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT) < 0)
    {
      app->sd_ready = false;
      return ERROR;
    }

  ret = statfs(CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT, &fs);
  if (ret < 0 || fs.f_type != MSDOS_SUPER_MAGIC)
    {
      /* mmcsd_slotinitialize() returns before its media-change worker has
       * necessarily finished probing the card.  Wait for the block node and
       * retry mount for up to ten seconds, but never initialize SDIO twice.
       */

      for (retry = 0; retry < SD_MOUNT_RETRY_COUNT; retry++)
        {
          if (stat(CONFIG_EXAMPLES_CAMERA_GALLERY_SD_DEVPATH, &st) < 0)
            {
              ret = ERROR;
              mount_errno = errno;
            }
          else if (!S_ISBLK(st.st_mode))
            {
              ret = ERROR;
              mount_errno = ENODEV;
            }
          else
            {
              ret = mount(CONFIG_EXAMPLES_CAMERA_GALLERY_SD_DEVPATH,
                          CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT,
                          "vfat", 0, NULL);
              if (ret == 0)
                {
                  app->sd_mounted_here = true;
                  if (retry > 0)
                    {
                      printf("camera_gallery: SD mounted after %d retries\n",
                             retry);
                    }

                  break;
                }

              mount_errno = errno;
              if (mount_errno == EBUSY)
                {
                  break;
                }
            }

          if (mount_errno != EINVAL && mount_errno != ENODEV &&
              mount_errno != ENOENT && mount_errno != EIO &&
              mount_errno != EAGAIN && mount_errno != ETIMEDOUT)
            {
              break;
            }

          if (retry == 0 ||
              (retry + 1) % SD_MOUNT_LOG_INTERVAL == 0)
            {
              printf("camera_gallery: SD not ready (%d), attempt %d/%d\n",
                     mount_errno, retry + 1, SD_MOUNT_RETRY_COUNT);
            }

          if (retry + 1 < SD_MOUNT_RETRY_COUNT)
            {
              usleep(SD_MOUNT_RETRY_US);
            }
        }

      if (ret < 0 && mount_errno != EBUSY)
        {
          printf("camera_gallery: SD mount failed after %d attempts: %d\n",
                 retry < SD_MOUNT_RETRY_COUNT ? retry + 1 : retry,
                 mount_errno);
          app->sd_ready = false;
          errno = mount_errno;
          return ERROR;
        }

      ret = statfs(CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT, &fs);
      if (ret < 0)
        {
          mount_errno = errno;
          printf("camera_gallery: SD statfs failed: %d\n", mount_errno);
          app->sd_ready = false;
          errno = mount_errno;
          return ERROR;
        }

      if (fs.f_type != MSDOS_SUPER_MAGIC)
        {
          mount_errno = mount_errno == EBUSY ? EBUSY : ENODEV;
          printf("camera_gallery: SD mountpoint has unexpected fs type "
                 "0x%lx\n", (unsigned long)fs.f_type);
          app->sd_ready = false;
          errno = mount_errno;
          return ERROR;
        }
    }

  ret = snprintf(dcim, sizeof(dcim), "%s/DCIM",
                 CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT);
  if (ret < 0 || (size_t)ret >= sizeof(dcim) ||
      camera_gallery_make_path(dcim) < 0)
    {
      app->sd_ready = false;
      if (ret >= 0 && (size_t)ret >= sizeof(dcim))
        {
          errno = ENAMETOOLONG;
        }

      return ERROR;
    }

  app->sd_ready = true;
  return OK;
}

static void camera_gallery_stream_stop(FAR struct camera_gallery_app_s *app)
{
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int i;

  if (app->camera_fd >= 0 && app->streaming)
    {
      if (ioctl(app->camera_fd, VIDIOC_STREAMOFF, (uintptr_t)&type) < 0)
        {
          printf("camera_gallery: VIDIOC_STREAMOFF failed: %d\n", errno);
        }
    }

  app->streaming = false;
  if (app->camera_fd >= 0)
    {
      close(app->camera_fd);
      app->camera_fd = -1;
    }

  for (i = 0; i < CAMERA_BUFFER_COUNT; i++)
    {
      free(app->buffers[i].start);
      app->buffers[i].start = NULL;
      app->buffers[i].length = 0;
    }
}

static int camera_gallery_stream_start(FAR struct camera_gallery_app_s *app)
{
  struct v4l2_requestbuffers request;
  struct v4l2_format format;
  struct v4l2_buffer buffer;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int i;

  app->camera_fd = open(CONFIG_EXAMPLES_CAMERA_GALLERY_DEVPATH, O_RDWR);
  if (app->camera_fd < 0)
    {
      printf("camera_gallery: open %s failed: %d\n",
             CONFIG_EXAMPLES_CAMERA_GALLERY_DEVPATH, errno);
      return ERROR;
    }

  memset(&format, 0, sizeof(format));
  format.type = type;
  format.fmt.pix.width = CAMERA_GALLERY_WIDTH;
  format.fmt.pix.height = CAMERA_GALLERY_HEIGHT;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
  if (ioctl(app->camera_fd, VIDIOC_S_FMT, (uintptr_t)&format) < 0)
    {
      printf("camera_gallery: VIDIOC_S_FMT failed: %d\n", errno);
      goto fail;
    }

  if (format.fmt.pix.width != CAMERA_GALLERY_WIDTH ||
      format.fmt.pix.height != CAMERA_GALLERY_HEIGHT ||
      format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565)
    {
      printf("camera_gallery: camera rejected QVGA RGB565\n");
      errno = EINVAL;
      goto fail;
    }

  memset(&request, 0, sizeof(request));
  request.type = type;
  request.memory = V4L2_MEMORY_USERPTR;
  request.count = CAMERA_BUFFER_COUNT;
  request.mode = V4L2_BUF_MODE_RING;
  if (ioctl(app->camera_fd, VIDIOC_REQBUFS, (uintptr_t)&request) < 0)
    {
      printf("camera_gallery: VIDIOC_REQBUFS failed: %d\n", errno);
      goto fail;
    }

  if (request.count < CAMERA_BUFFER_COUNT)
    {
      printf("camera_gallery: driver provides only %u buffers\n",
             request.count);
      errno = ENOMEM;
      goto fail;
    }

  for (i = 0; i < CAMERA_BUFFER_COUNT; i++)
    {
      app->buffers[i].length = CAMERA_GALLERY_IMAGE_SIZE;
      app->buffers[i].start = memalign(32, CAMERA_GALLERY_IMAGE_SIZE);
      if (app->buffers[i].start == NULL)
        {
          printf("camera_gallery: frame buffer allocation failed\n");
          goto fail;
        }
    }

  for (i = 0; i < CAMERA_BUFFER_COUNT; i++)
    {
      memset(&buffer, 0, sizeof(buffer));
      buffer.type = type;
      buffer.memory = V4L2_MEMORY_USERPTR;
      buffer.index = i;
      buffer.m.userptr = (uintptr_t)app->buffers[i].start;
      buffer.length = app->buffers[i].length;
      if (ioctl(app->camera_fd, VIDIOC_QBUF, (uintptr_t)&buffer) < 0)
        {
          printf("camera_gallery: VIDIOC_QBUF failed: %d\n", errno);
          goto fail;
        }
    }

  if (ioctl(app->camera_fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      printf("camera_gallery: VIDIOC_STREAMON failed: %d\n", errno);
      goto fail;
    }

  app->streaming = true;
  return OK;

fail:
  camera_gallery_stream_stop(app);
  return ERROR;
}

static btn_buttonset_t camera_gallery_adc_button(int32_t millivolts)
{
  if (millivolts <= 0 || millivolts >= BUTTON_RELEASE_MILLIVOLTS)
    {
      return 0;
    }

  if (millivolts >= BUTTON_MENU_MILLIVOLTS)
    {
      return CAMERA_GALLERY_BUTTON_MENU;
    }

  if (millivolts >= BUTTON_PLAY_MILLIVOLTS)
    {
      return CAMERA_GALLERY_BUTTON_PLAY;
    }

  if (millivolts >= BUTTON_UP_MILLIVOLTS)
    {
      return CAMERA_GALLERY_BUTTON_UP;
    }

  return CAMERA_GALLERY_BUTTON_DOWN;
}

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_ADC_BUTTONS
static int
camera_gallery_read_adc(FAR struct camera_gallery_app_s *app,
                        FAR int32_t *millivolts)
{
  struct adc_msg_s message;
  ssize_t nread;

  if (ioctl(app->adc_fd, ANIOC_TRIGGER, 0) < 0)
    {
      return ERROR;
    }

  for (; ; )
    {
      nread = read(app->adc_fd, &message, sizeof(message));
      if (nread < 0 && errno == EINTR)
        {
          continue;
        }

      if (nread != sizeof(message))
        {
          if (nread >= 0)
            {
              errno = EIO;
            }

          return ERROR;
        }

      if (message.am_channel == 0)
        {
          *millivolts = message.am_data;
          return OK;
        }
    }
}
#endif

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_ADC_MONITOR
static void
camera_gallery_adc_monitor(FAR struct camera_gallery_app_s *app)
{
  char line[20];
  int32_t millivolts;
  int ret;

  for (; ; )
    {
      ret = camera_gallery_read_adc(app, &millivolts);
      if (ret < 0)
        {
          printf("camera_gallery: ADC monitor read failed: %d\n", errno);
          camera_gallery_nx_page("ADC MONITOR", "ADC ERROR",
                                 "RESTART", true);
          usleep(500000);
          continue;
        }

      ret = snprintf(line, sizeof(line), "RAW %ldMV",
                     (long)millivolts);
      if (ret < 0 || ret >= sizeof(line))
        {
          snprintf(line, sizeof(line), "RAW ERROR");
        }

      printf("camera_gallery: ADC1_CH0 %ld mV\n", (long)millivolts);
      camera_gallery_nx_page("ADC MONITOR", line, "HOLD BUTTON", true);
      usleep(200000);
    }
}
#endif

static int camera_gallery_read_buttons(FAR struct camera_gallery_app_s *app,
                                       FAR btn_buttonset_t *sample)
{
  ssize_t nread;
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_ADC_BUTTONS
  int32_t millivolts;
#endif

  for (; ; )
    {
      nread = read(app->buttons_fd, sample, sizeof(*sample));
      if (nread >= 0 || errno != EINTR)
        {
          break;
        }
    }

  if (nread != sizeof(*sample))
    {
      if (nread >= 0)
        {
          errno = EIO;
        }

      return ERROR;
    }

  /* BOOT is intentionally not part of the application controls.  Read the
   * GPIO device to preserve its normal driver lifecycle, then discard it;
   * MENU/PLAY/DOWN/UP all come from the ADC resistor ladder.
   */

  *sample = 0;

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_ADC_BUTTONS
  if (camera_gallery_read_adc(app, &millivolts) < 0)
    {
      return ERROR;
    }

  *sample |= camera_gallery_adc_button(millivolts);
#endif

  *sample &= CAMERA_GALLERY_BUTTON_MASK;
  return OK;
}

static btn_buttonset_t
camera_gallery_primary_button(btn_buttonset_t buttons)
{
  btn_buttonset_t bit;

  for (bit = 1; bit != 0; bit <<= 1)
    {
      if ((buttons & bit) != 0)
        {
          return bit;
        }
    }

  return 0;
}

static int
camera_gallery_button_action(FAR struct camera_gallery_app_s *app,
                             FAR struct camera_gallery_button_event_s *event)
{
  btn_buttonset_t sample;

  event->button = 0;
  event->action = CAMERA_GALLERY_BUTTON_NONE;

  for (; ; )
    {
      if (camera_gallery_read_buttons(app, &sample) < 0)
        {
          return ERROR;
        }

      /* Pace direct reads while still avoiding poll or interrupt use. */

      usleep(BUTTON_SAMPLE_US);

      if (sample == app->button_candidate)
        {
          if (app->button_count < BUTTON_STABLE_COUNT)
            {
              app->button_count++;
            }
        }
      else
        {
          app->button_candidate = sample;
          app->button_count = 1;
        }

      if (app->button_count < BUTTON_STABLE_COUNT)
        {
          continue;
        }

      if (!app->button_initialized)
        {
          /* Ignore a key already held while the application starts. */

          app->button_stable = sample;
          app->button_initialized = true;
          app->button_pressed = false;
        }
      else if (sample != app->button_stable)
        {
          app->button_stable = sample;
          if (sample != 0)
            {
              /* Latch the first stable ADC key until all keys are released,
               * so transitions between nonzero ladder levels cannot lose
               * the release event.
               */

              if (!app->button_pressed)
                {
                  app->button_pressed = true;
                  app->button_pressed_id =
                    camera_gallery_primary_button(sample);
                }
            }
          else if (app->button_pressed)
            {
              app->button_pressed = false;
              event->button = app->button_pressed_id;
              event->action = CAMERA_GALLERY_BUTTON_PRESS;
            }
        }

      return OK;
    }
}

static void camera_gallery_show_home(FAR struct camera_gallery_app_s *app)
{
  app->state = CAMERA_GALLERY_HOME;
  camera_gallery_nx_home(app->home_selection);
}

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
static void camera_gallery_enter_aivlog(
  FAR struct camera_gallery_app_s *app)
{
  camera_gallery_nx_page("AI VLOG", "MODEL INIT", "PLEASE WAIT", true);
  if (camera_gallery_aivlog_live_start() < 0)
    {
      printf("camera_gallery: AI Vlog live start failed: %d\n", errno);
      camera_gallery_nx_page("AI VLOG", "MODEL ERROR", "MENU BACK", true);
      camera_gallery_show_home(app);
      return;
    }

  if (camera_gallery_stream_start(app) < 0)
    {
      printf("camera_gallery: AI Vlog camera start failed: %d\n", errno);
      camera_gallery_aivlog_live_stop();
      camera_gallery_show_home(app);
      return;
    }

  app->state = CAMERA_GALLERY_AIVLOG;
}
#endif

static int camera_gallery_enter_camera(FAR struct camera_gallery_app_s *app)
{
  if (camera_gallery_stream_start(app) < 0)
    {
      camera_gallery_show_home(app);
      return ERROR;
    }

  app->state = CAMERA_GALLERY_CAMERA_PREVIEW;
  return OK;
}

static int camera_gallery_scan_images(FAR struct camera_gallery_app_s *app,
                                      FAR unsigned int *maximum)
{
  char dcim[GALLERY_PATH_SIZE];
  int ret;

  ret = snprintf(dcim, sizeof(dcim), "%s/DCIM",
                 CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT);
  if (ret < 0 || ret >= sizeof(dcim))
    {
      errno = ENAMETOOLONG;
      return ERROR;
    }

  ret = camera_gallery_scan(dcim, app->numbers, MAX_GALLERY_IMAGES,
                            maximum);
  if (ret > MAX_GALLERY_IMAGES)
    {
      errno = EOVERFLOW;
      return ERROR;
    }

  return ret;
}

static int camera_gallery_load_current(FAR struct camera_gallery_app_s *app)
{
  char path[GALLERY_PATH_SIZE];
  int ret;

  ret = snprintf(path, sizeof(path), "%s/DCIM/IMG_%04u.BMP",
                 CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT,
                 app->numbers[app->image_index]);
  if (ret < 0 || ret >= sizeof(path) ||
      camera_gallery_bmp_load(path, app->gallery_pixels) < 0)
    {
      camera_gallery_nx_page("ALBUM", "SD ERROR", "MENU BACK", true);
      return ERROR;
    }

  camera_gallery_nx_draw(app->gallery_pixels, CAMERA_GALLERY_WIDTH,
                         CAMERA_GALLERY_HEIGHT);
  return OK;
}

static void
camera_gallery_enter_gallery(FAR struct camera_gallery_app_s *app)
{
  unsigned int maximum;

  app->state = CAMERA_GALLERY_GALLERY;
  if (camera_gallery_sd_initialize(app) < 0)
    {
      camera_gallery_nx_page("ALBUM", "SD ERROR", "MENU BACK", true);
      app->image_count = 0;
      return;
    }

  app->image_count = camera_gallery_scan_images(app, &maximum);
  if (app->image_count < 0)
    {
      app->sd_ready = false;
      app->image_count = 0;
      camera_gallery_nx_page("ALBUM", "SD ERROR", "MENU BACK", true);
    }
  else if (app->image_count == 0)
    {
      camera_gallery_nx_page("ALBUM", "NO PHOTOS", "MENU BACK", true);
    }
  else
    {
      app->image_index = app->image_count - 1;
      camera_gallery_load_current(app);
    }
}

static int camera_gallery_next_number(FAR struct camera_gallery_app_s *app,
                                      FAR unsigned int *next)
{
  unsigned int maximum;
  unsigned int candidate;
  int count;
  int index = 0;

  count = camera_gallery_scan_images(app, &maximum);
  if (count < 0)
    {
      return ERROR;
    }

  if (count == 0)
    {
      *next = 0;
      return OK;
    }

  if (maximum < 9999)
    {
      *next = maximum + 1;
      return OK;
    }

  for (candidate = 0; candidate <= 9999; candidate++)
    {
      while (index < count && app->numbers[index] < candidate)
        {
          index++;
        }

      if (index == count || app->numbers[index] != candidate)
        {
          *next = candidate;
          return OK;
        }
    }

  errno = ENOSPC;
  return ERROR;
}

static int camera_gallery_save_frame(FAR struct camera_gallery_app_s *app,
                                     FAR const uint16_t *pixels)
{
  unsigned int number;
  char path[GALLERY_PATH_SIZE];
  int ret;

  if (camera_gallery_sd_initialize(app) < 0 ||
      camera_gallery_next_number(app, &number) < 0)
    {
      app->sd_ready = false;
      camera_gallery_nx_page("CAMERA", "SD ERROR", "PLAY PHOTO", true);
      return ERROR;
    }

  ret = snprintf(path, sizeof(path), "%s/DCIM/IMG_%04u.BMP",
                 CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT, number);
  if (ret < 0 || ret >= sizeof(path))
    {
      errno = ENAMETOOLONG;
      app->sd_ready = false;
      printf("camera_gallery: invalid SD photo path: %d\n", errno);
      camera_gallery_nx_page("CAMERA", "SD ERROR", "PLAY PHOTO", true);
      return ERROR;
    }

  if (camera_gallery_bmp_save(path, pixels) < 0)
    {
      int save_errno = errno;

      app->sd_ready = false;
      printf("camera_gallery: save %s failed: %d\n", path, save_errno);
      camera_gallery_nx_page("CAMERA", "SD ERROR", "PLAY PHOTO", true);
      errno = save_errno;
      return ERROR;
    }

  printf("camera_gallery: saved %s\n", path);
#ifndef CONFIG_EXAMPLES_CAMERA_GALLERY_CLOUD_SCORE
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
  if (app->state != CAMERA_GALLERY_AIVLOG)
#endif
    {
      camera_gallery_nx_page("CAMERA", "PHOTO SAVED", "PLAY PHOTO", true);
    }
#endif
  return OK;
}

static int camera_gallery_preview(FAR struct camera_gallery_app_s *app)
{
  struct camera_gallery_button_event_s event;
  struct v4l2_buffer buffer;
  FAR uint16_t *pixels;
  enum camera_gallery_level_status_e level_status;
  int level_tilt;
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
  bool aivlog = app->state == CAMERA_GALLERY_AIVLOG;
#endif
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_CLOUD_SCORE
  uint32_t generation;
  bool captured = false;
#endif

  memset(&buffer, 0, sizeof(buffer));
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_USERPTR;
  if (ioctl(app->camera_fd, VIDIOC_DQBUF, (uintptr_t)&buffer) < 0)
    {
      printf("camera_gallery: VIDIOC_DQBUF failed: %d\n", errno);
      return ERROR;
    }

  pixels = (FAR uint16_t *)(uintptr_t)buffer.m.userptr;
  level_status = CAMERA_GALLERY_LEVEL_UNAVAILABLE;
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
  if (!aivlog)
#endif
    {
      level_status = camera_gallery_level_update(&level_tilt);
      camera_gallery_nx_level_update(level_status, level_tilt);
    }

  if (buffer.bytesused >= CAMERA_GALLERY_IMAGE_SIZE)
    {
      camera_gallery_nx_draw(pixels, CAMERA_GALLERY_WIDTH,
                             CAMERA_GALLERY_HEIGHT);
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
      if (aivlog && camera_gallery_aivlog_live_submit(pixels) < 0 &&
          errno != EBUSY)
        {
          printf("camera_gallery: AI Vlog frame submit failed: %d\n",
                 errno);
        }
#endif
    }

  if (camera_gallery_button_action(app, &event) < 0)
    {
      return ERROR;
    }

  /* Save synchronously while this dequeued USERPTR still belongs to us.
   * PLAY is the confirm/shutter key.  The regular camera uses the level
   * assist to gate capture.  AI Vlog skips the assist and always permits
   * manual capture so its overlay contains only the live AI score.
   */

  if (event.action == CAMERA_GALLERY_BUTTON_PRESS &&
      event.button == CAMERA_GALLERY_BUTTON_PLAY &&
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
      !aivlog &&
#endif
      !camera_gallery_level_capture_allowed(level_status))
    {
      printf("camera_gallery: shutter blocked until level (state=%d)\n",
             level_status);
    }
  else if (event.action == CAMERA_GALLERY_BUTTON_PRESS &&
           event.button == CAMERA_GALLERY_BUTTON_PLAY &&
           buffer.bytesused >= CAMERA_GALLERY_IMAGE_SIZE)
    {
      if (camera_gallery_save_frame(app, pixels) == OK)
        {
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_CLOUD_SCORE
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
          if (!aivlog)
#endif
            {
              memcpy(app->gallery_pixels, pixels,
                     CAMERA_GALLERY_IMAGE_SIZE);
              camera_gallery_nx_level_clear();
              camera_gallery_nx_score_clear();
              camera_gallery_nx_draw(app->gallery_pixels,
                                     CAMERA_GALLERY_WIDTH,
                                     CAMERA_GALLERY_HEIGHT);
              generation = camera_gallery_nx_score_pending();

              if (!app->cloud_ready)
                {
                  if (camera_gallery_cloud_initialize() == OK)
                    {
                      app->cloud_ready = true;
                      printf("camera_gallery: cloud scoring recovered\n");
                    }
                  else
                    {
                      printf("camera_gallery: cloud scoring retry failed: "
                             "%d\n", errno);
                    }
                }

              if (!app->cloud_ready)
                {
                  camera_gallery_nx_score_error(generation,
                                                "AI INIT ERROR");
                }
              else if (camera_gallery_cloud_submit(pixels, generation) < 0)
                {
                  int submit_errno = errno;

                  printf("camera_gallery: cloud submit failed: %d\n",
                         submit_errno);
                  camera_gallery_nx_score_error(
                    generation,
                    submit_errno == EBUSY ? "AI BUSY" :
                    submit_errno == ENOMEM ? "AI MEMORY" : "AI ERROR");
                }

              captured = true;
            }
#endif
        }
    }

  if (ioctl(app->camera_fd, VIDIOC_QBUF, (uintptr_t)&buffer) < 0)
    {
      printf("camera_gallery: requeue failed: %d\n", errno);
      return ERROR;
    }

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_CLOUD_SCORE
  if (captured)
    {
      camera_gallery_stream_stop(app);
      app->state = CAMERA_GALLERY_PHOTO_REVIEW;
      return OK;
    }
#endif

  if (event.button == CAMERA_GALLERY_BUTTON_MENU &&
      event.action == CAMERA_GALLERY_BUTTON_PRESS)
    {
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
      if (aivlog)
        {
          camera_gallery_aivlog_live_stop();
        }
#endif
      camera_gallery_stream_stop(app);
      camera_gallery_show_home(app);
    }

  return OK;
}

static void
camera_gallery_handle_button(
  FAR struct camera_gallery_app_s *app,
  FAR const struct camera_gallery_button_event_s *event)
{
  if (event->action == CAMERA_GALLERY_BUTTON_NONE)
    {
      return;
    }

  switch (app->state)
    {
      case CAMERA_GALLERY_HOME:
        if (event->button == CAMERA_GALLERY_BUTTON_PLAY)
          {
            if (app->home_selection == CAMERA_GALLERY_HOME_CAMERA)
              {
                camera_gallery_enter_camera(app);
              }
            else if (app->home_selection == CAMERA_GALLERY_HOME_ALBUM)
              {
                camera_gallery_enter_gallery(app);
              }
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
            else
              {
                camera_gallery_enter_aivlog(app);
              }
#endif
          }
        else
          {
            if (event->button == CAMERA_GALLERY_BUTTON_UP)
              {
                app->home_selection--;
                if (app->home_selection < 0)
                  {
                    app->home_selection = CAMERA_GALLERY_HOME_COUNT - 1;
                  }
              }
            else if (event->button == CAMERA_GALLERY_BUTTON_DOWN)
              {
                app->home_selection++;
                if (app->home_selection >= CAMERA_GALLERY_HOME_COUNT)
                  {
                    app->home_selection = CAMERA_GALLERY_HOME_CAMERA;
                  }
              }

            camera_gallery_show_home(app);
          }
        break;

      case CAMERA_GALLERY_PHOTO_REVIEW:
        if (event->button == CAMERA_GALLERY_BUTTON_MENU ||
            event->button == CAMERA_GALLERY_BUTTON_PLAY)
          {
            camera_gallery_nx_score_clear();
            if (camera_gallery_enter_camera(app) < 0)
              {
                camera_gallery_nx_page("CAMERA", "CAM ERROR",
                                       "MENU BACK", true);
              }
          }
        break;

      case CAMERA_GALLERY_GALLERY:
        if (event->button == CAMERA_GALLERY_BUTTON_MENU)
          {
            camera_gallery_show_home(app);
          }
        else if (app->image_count > 0 &&
                 event->button == CAMERA_GALLERY_BUTTON_DOWN)
          {
            app->image_index++;
            if (app->image_index >= app->image_count)
              {
                app->image_index = 0;
              }

            camera_gallery_load_current(app);
          }
        else if (app->image_count > 0 &&
                 event->button == CAMERA_GALLERY_BUTTON_UP)
          {
            if (app->image_index == 0)
              {
                app->image_index = app->image_count - 1;
              }
            else
              {
                app->image_index--;
              }

            camera_gallery_load_current(app);
          }
        else if (app->image_count > 0)
          {
            camera_gallery_load_current(app);
          }
        else
          {
            camera_gallery_nx_page("ALBUM", "NO PHOTOS",
                                   "MENU BACK", true);
          }
        break;

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
      case CAMERA_GALLERY_AIVLOG:
        /* AI Vlog is serviced by camera_gallery_preview(). */

        break;
#endif

      default:
        break;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct camera_gallery_button_event_s event;
  struct camera_gallery_app_s app;
  int ret = EXIT_FAILURE;

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
  if (argc == 2 && strcmp(argv[1], "aivlog_score_sd") == 0)
    {
      memset(&app, 0, sizeof(app));
      if (camera_gallery_sd_initialize(&app) < 0)
        {
          printf("[aivlog] SD initialization failed: %d\n", errno);
          return EXIT_FAILURE;
        }

      ret = camera_gallery_aivlog_score_sd(false) == OK ?
            EXIT_SUCCESS : EXIT_FAILURE;
      if (app.sd_mounted_here)
        {
          umount(CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT);
        }

      return ret;
    }
#endif

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_CLOUD_SCORE
  if (argc > 1)
    {
      memset(&app, 0, sizeof(app));
      if (camera_gallery_sd_initialize(&app) < 0)
        {
          printf("camera_gallery: mount SD before configuring cloud: %d\n",
                 errno);
          return EXIT_FAILURE;
        }

      return camera_gallery_cloud_command(argc, argv);
    }
#endif

  pthread_mutex_lock(&g_camera_gallery_lock);
  if (g_camera_gallery_running)
    {
      pthread_mutex_unlock(&g_camera_gallery_lock);
      printf("camera_gallery: already running\n");
      return EXIT_FAILURE;
    }

  g_camera_gallery_running = true;
  pthread_mutex_unlock(&g_camera_gallery_lock);

  memset(&app, 0, sizeof(app));
  app.camera_fd = -1;
  app.buttons_fd = -1;
  app.adc_fd = -1;
  app.home_selection = CAMERA_GALLERY_HOME_CAMERA;

  app.gallery_pixels = malloc(CAMERA_GALLERY_IMAGE_SIZE);
  app.numbers = malloc(MAX_GALLERY_IMAGES * sizeof(unsigned int));
  if (app.gallery_pixels == NULL || app.numbers == NULL)
    {
      printf("camera_gallery: out of memory\n");
      goto out;
    }

  if (camera_gallery_nx_initialize() < 0)
    {
      goto out;
    }

  /* Level assist is optional at runtime.  If the accelerometer is missing,
   * the overlay reports NO IMU and the shutter remains available.
   */

  camera_gallery_level_initialize();

  app.buttons_fd = open(CONFIG_EXAMPLES_CAMERA_GALLERY_BUTTONS_DEVPATH,
                        O_RDONLY);
  if (app.buttons_fd < 0)
    {
      printf("camera_gallery: open %s failed: %d\n",
             CONFIG_EXAMPLES_CAMERA_GALLERY_BUTTONS_DEVPATH, errno);
      goto out_nx;
    }

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_ADC_BUTTONS
  app.adc_fd = open(CONFIG_EXAMPLES_CAMERA_GALLERY_ADC_DEVPATH, O_RDONLY);
  if (app.adc_fd < 0)
    {
      printf("camera_gallery: open %s failed: %d\n",
             CONFIG_EXAMPLES_CAMERA_GALLERY_ADC_DEVPATH, errno);
      goto out_buttons;
    }
#endif

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_ADC_MONITOR
  camera_gallery_adc_monitor(&app);
#endif

  app.sd_ready = camera_gallery_sd_initialize(&app) == OK;
  if (!app.sd_ready)
    {
      printf("camera_gallery: SD initialization failed: %d\n", errno);
    }

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_CLOUD_SCORE
  if (app.sd_ready && camera_gallery_cloud_initialize() == OK)
    {
      app.cloud_ready = true;
    }
  else
    {
      printf("camera_gallery: cloud scoring unavailable: %d\n", errno);
    }
#endif

#if defined(CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD) && \
    defined(CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR)
  /* Publish the vlog-assistant skill for the on-device AI agent
   * (packages/ai_agent); the agent loads skills from its data directory
   * when a conversation starts.
   */

  if (app.sd_ready && camera_gallery_aivlog_install_skill() < 0)
    {
      printf("camera_gallery: AI agent skill install failed: %d\n", errno);
    }
#endif

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_PROVISION
  /* Provisioning owns config/network access and must start before the cloud
   * queue.  BLE availability is optional; initialization failure does not
   * prevent the camera application from running.
   */

  if (camera_gallery_aivlog_provision_initialize() < 0)
    {
      printf("camera_gallery: AI Vlog provisioning unavailable: %d\n",
             errno);
    }
#endif

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD
  /* Start the persistent queue as soon as SD is ready.  This resumes uploads
   * and closes an interrupted ACTIVE manifest without requiring the user to
   * enter AI Vlog again after reboot.
   */

  if (app.sd_ready && camera_gallery_aivlog_cloud_initialize() < 0)
    {
      printf("camera_gallery: AI Vlog queue unavailable: %d\n", errno);
    }
#endif

  if (camera_gallery_button_action(&app, &event) < 0)
    {
      printf("camera_gallery: initial button read failed: %d\n", errno);
      goto out_buttons;
    }

  camera_gallery_show_home(&app);
  for (; ; )
    {
      if (app.state == CAMERA_GALLERY_CAMERA_PREVIEW
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
          || app.state == CAMERA_GALLERY_AIVLOG
#endif
         )
        {
          if (camera_gallery_preview(&app) < 0)
            {
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
              if (app.state == CAMERA_GALLERY_AIVLOG)
                {
                  camera_gallery_aivlog_live_stop();
                }
#endif
              camera_gallery_stream_stop(&app);
              camera_gallery_show_home(&app);
            }
        }
      else
        {
          if (camera_gallery_button_action(&app, &event) < 0)
            {
              printf("camera_gallery: button read failed: %d\n", errno);
              break;
            }

          camera_gallery_handle_button(&app, &event);
        }
    }

out_buttons:
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
  camera_gallery_aivlog_live_stop();
#endif
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_PROVISION
  /* Stop provisioning before the cloud worker that it can wake. */

  camera_gallery_aivlog_provision_finalize();
#endif
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD
  /* Stop the persistent uploader while the SD card is still mounted. */

  camera_gallery_aivlog_cloud_finalize();
#endif
  camera_gallery_stream_stop(&app);
  if (app.adc_fd >= 0)
    {
      close(app.adc_fd);
    }

  close(app.buttons_fd);
  if (app.sd_mounted_here)
    {
      umount(CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT);
    }

out_nx:
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_CLOUD_SCORE
  if (app.cloud_ready)
    {
      camera_gallery_cloud_finalize();
    }
#endif
  camera_gallery_level_finalize();
  camera_gallery_nx_finalize();
out:
  free(app.numbers);
  free(app.gallery_pixels);
  pthread_mutex_lock(&g_camera_gallery_lock);
  g_camera_gallery_running = false;
  pthread_mutex_unlock(&g_camera_gallery_lock);
  return ret;
}
