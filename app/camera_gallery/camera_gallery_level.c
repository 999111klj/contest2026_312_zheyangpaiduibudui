/****************************************************************************
 * apps/examples/camera_gallery/camera_gallery_level.c
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef CONFIG_SENSORS_QMA7981
#  include <nuttx/sensors/qma7981.h>
#endif

#include "camera_gallery.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The QMA7981 is configured for +/-8 g, where 1 g is approximately 1020
 * counts.  Its X/Y axes lie in the EYE PCB plane; the dominant in-plane
 * gravity axis is selected dynamically so portrait and landscape holding
 * orientations both work.
 */

#define LEVEL_FILTER_SHIFT          2
#define LEVEL_FILTER_STABLE_DELTA   25
#define LEVEL_RAW_STABLE_DELTA      80
#define LEVEL_STABLE_SAMPLES        4
#define LEVEL_READY_SAMPLES         3
#define LEVEL_UPRIGHT_MIN           600
#define LEVEL_GRAVITY_MIN           700
#define LEVEL_GRAVITY_MAX           1400
#define LEVEL_THRESHOLD_PERMILLE    70
#define LEVEL_READ_ERROR_LIMIT      3

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct camera_gallery_level_s
{
  int fd;
  int32_t filtered[3];
  int32_t previous_raw[3];
  int32_t zero[2];
  unsigned int stable_samples;
  unsigned int ready_samples;
  unsigned int read_errors;
  unsigned int diagnostic_samples;
  enum camera_gallery_level_status_e last_status;
  bool filter_ready;
  bool status_reported;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct camera_gallery_level_s g_level =
{
  .fd = -1
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int32_t camera_gallery_abs32(int32_t value)
{
  return value < 0 ? -value : value;
}

static FAR const char *camera_gallery_level_name(
  enum camera_gallery_level_status_e status)
{
  switch (status)
    {
      case CAMERA_GALLERY_LEVEL_HOLD:
        return "HOLD";
      case CAMERA_GALLERY_LEVEL_ADJUST:
        return "ADJUST";
      case CAMERA_GALLERY_LEVEL_READY:
        return "SHOOT";
      default:
        return "NO IMU";
    }
}

static enum camera_gallery_level_status_e
camera_gallery_level_report(enum camera_gallery_level_status_e status,
                            int tilt_permille)
{
  bool periodic = false;

  if (status == CAMERA_GALLERY_LEVEL_HOLD)
    {
      g_level.diagnostic_samples++;
      if (g_level.diagnostic_samples >= 30)
        {
          g_level.diagnostic_samples = 0;
          periodic = true;
        }
    }
  else
    {
      g_level.diagnostic_samples = 0;
    }

  if (!g_level.status_reported || status != g_level.last_status || periodic)
    {
      printf("camera_gallery: level %s raw=%ld,%ld,%ld filt=%ld,%ld,%ld "
             "stable=%u tilt=%d\n",
             camera_gallery_level_name(status),
             (long)g_level.previous_raw[0],
             (long)g_level.previous_raw[1],
             (long)g_level.previous_raw[2],
             (long)g_level.filtered[0],
             (long)g_level.filtered[1],
             (long)g_level.filtered[2],
             g_level.stable_samples, tilt_permille);
      g_level.last_status = status;
      g_level.status_reported = true;
    }

  return status;
}

static enum camera_gallery_level_status_e
camera_gallery_level_evaluate(FAR int *tilt_permille)
{
  int32_t gravity_min_sq = LEVEL_GRAVITY_MIN * LEVEL_GRAVITY_MIN;
  int32_t gravity_max_sq = LEVEL_GRAVITY_MAX * LEVEL_GRAVITY_MAX;
  int32_t abs_x = camera_gallery_abs32(g_level.filtered[0]);
  int32_t abs_y = camera_gallery_abs32(g_level.filtered[1]);
  int32_t vertical;
  int32_t lateral;
  int32_t magnitude_sq;
  int32_t tilt;

  /* A surface-mounted accelerometer has X/Y in the PCB plane and Z normal
   * to it.  Select whichever in-plane axis currently carries gravity as the
   * vertical axis, allowing both portrait and landscape camera orientation.
   */

  if (abs_y >= abs_x)
    {
      vertical = abs_y;
      lateral = g_level.filtered[0] - g_level.zero[0];
    }
  else
    {
      vertical = abs_x;
      lateral = g_level.filtered[1] - g_level.zero[1];
    }

  magnitude_sq = g_level.filtered[0] * g_level.filtered[0] +
                 g_level.filtered[1] * g_level.filtered[1] +
                 g_level.filtered[2] * g_level.filtered[2];

  if (g_level.stable_samples < LEVEL_STABLE_SAMPLES ||
      vertical < LEVEL_UPRIGHT_MIN ||
      magnitude_sq < gravity_min_sq || magnitude_sq > gravity_max_sq)
    {
      g_level.ready_samples = 0;
      *tilt_permille = 0;
      return CAMERA_GALLERY_LEVEL_HOLD;
    }

  tilt = lateral * 1000 / vertical;
  *tilt_permille = (int)tilt;
  if (camera_gallery_abs32(tilt) > LEVEL_THRESHOLD_PERMILLE)
    {
      g_level.ready_samples = 0;
      return CAMERA_GALLERY_LEVEL_ADJUST;
    }

  if (g_level.ready_samples < LEVEL_READY_SAMPLES)
    {
      g_level.ready_samples++;
    }

  return g_level.ready_samples >= LEVEL_READY_SAMPLES ?
         CAMERA_GALLERY_LEVEL_READY : CAMERA_GALLERY_LEVEL_HOLD;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int camera_gallery_level_initialize(void)
{
  memset(&g_level, 0, sizeof(g_level));
  g_level.fd = -1;

#ifdef CONFIG_SENSORS_QMA7981
  g_level.fd = open(CONFIG_EXAMPLES_CAMERA_GALLERY_LEVEL_DEVPATH, O_RDONLY);
  if (g_level.fd < 0)
    {
      printf("camera_gallery: open %s failed: %d; level assist disabled\n",
             CONFIG_EXAMPLES_CAMERA_GALLERY_LEVEL_DEVPATH, errno);
      return ERROR;
    }

  printf("camera_gallery: level assist ready on %s\n",
         CONFIG_EXAMPLES_CAMERA_GALLERY_LEVEL_DEVPATH);
  return OK;
#else
  errno = ENOSYS;
  printf("camera_gallery: QMA7981 support is disabled\n");
  return ERROR;
#endif
}

enum camera_gallery_level_status_e
camera_gallery_level_update(FAR int *tilt_permille)
{
#ifdef CONFIG_SENSORS_QMA7981
  struct qma7981_data_s sample;
  enum camera_gallery_level_status_e status;
  int16_t raw[3];
  int32_t old[3];
  int32_t filtered_delta;
  int32_t raw_delta;
  ssize_t nread;
  int i;

  if (tilt_permille == NULL)
    {
      errno = EINVAL;
      return CAMERA_GALLERY_LEVEL_UNAVAILABLE;
    }

  *tilt_permille = 0;
  if (g_level.fd < 0)
    {
      return camera_gallery_level_report(
        CAMERA_GALLERY_LEVEL_UNAVAILABLE, 0);
    }

  do
    {
      nread = read(g_level.fd, &sample, sizeof(sample));
    }
  while (nread < 0 && errno == EINTR);

  if (nread != sizeof(sample))
    {
      g_level.read_errors++;
      g_level.filter_ready = false;
      g_level.stable_samples = 0;
      g_level.ready_samples = 0;
      if (g_level.read_errors == LEVEL_READ_ERROR_LIMIT)
        {
          printf("camera_gallery: level sensor read failed: %d\n", errno);
        }

      return camera_gallery_level_report(
        g_level.read_errors >= LEVEL_READ_ERROR_LIMIT ?
        CAMERA_GALLERY_LEVEL_UNAVAILABLE : CAMERA_GALLERY_LEVEL_HOLD, 0);
    }

  g_level.read_errors = 0;
  raw[0] = sample.x;
  raw[1] = sample.y;
  raw[2] = sample.z;
  if (!g_level.filter_ready)
    {
      g_level.filtered[0] = raw[0];
      g_level.filtered[1] = raw[1];
      g_level.filtered[2] = raw[2];
      g_level.previous_raw[0] = raw[0];
      g_level.previous_raw[1] = raw[1];
      g_level.previous_raw[2] = raw[2];
      g_level.filter_ready = true;
      g_level.stable_samples = 0;
      return camera_gallery_level_report(CAMERA_GALLERY_LEVEL_HOLD, 0);
    }

  memcpy(old, g_level.filtered, sizeof(old));
  g_level.filtered[0] += (raw[0] - g_level.filtered[0]) >>
                         LEVEL_FILTER_SHIFT;
  g_level.filtered[1] += (raw[1] - g_level.filtered[1]) >>
                         LEVEL_FILTER_SHIFT;
  g_level.filtered[2] += (raw[2] - g_level.filtered[2]) >>
                         LEVEL_FILTER_SHIFT;

  filtered_delta = 0;
  raw_delta = 0;
  for (i = 0; i < 3; i++)
    {
      int32_t axis_filtered_delta =
        camera_gallery_abs32(g_level.filtered[i] - old[i]);
      int32_t axis_raw_delta =
        camera_gallery_abs32(raw[i] - g_level.previous_raw[i]);

      if (axis_filtered_delta > filtered_delta)
        {
          filtered_delta = axis_filtered_delta;
        }

      if (axis_raw_delta > raw_delta)
        {
          raw_delta = axis_raw_delta;
        }

      g_level.previous_raw[i] = raw[i];
    }

  if (filtered_delta <= LEVEL_FILTER_STABLE_DELTA &&
      raw_delta <= LEVEL_RAW_STABLE_DELTA)
    {
      if (g_level.stable_samples < LEVEL_STABLE_SAMPLES)
        {
          g_level.stable_samples++;
        }
    }
  else
    {
      g_level.stable_samples = 0;
    }

  status = camera_gallery_level_evaluate(tilt_permille);
  return camera_gallery_level_report(status, *tilt_permille);
#else
  if (tilt_permille != NULL)
    {
      *tilt_permille = 0;
    }

  return CAMERA_GALLERY_LEVEL_UNAVAILABLE;
#endif
}

int camera_gallery_level_calibrate(void)
{
  int32_t abs_x;
  int32_t abs_y;
  int lateral_axis;

  if (g_level.fd < 0 || !g_level.filter_ready)
    {
      errno = ENODEV;
      return ERROR;
    }

  abs_x = camera_gallery_abs32(g_level.filtered[0]);
  abs_y = camera_gallery_abs32(g_level.filtered[1]);
  if (g_level.stable_samples < LEVEL_STABLE_SAMPLES ||
      (abs_x > abs_y ? abs_x : abs_y) < LEVEL_UPRIGHT_MIN)
    {
      errno = EAGAIN;
      return ERROR;
    }

  lateral_axis = abs_y >= abs_x ? 0 : 1;
  g_level.zero[lateral_axis] = g_level.filtered[lateral_axis];
  g_level.ready_samples = 0;
  printf("camera_gallery: level zero calibrated axis=%c value=%ld\n",
         lateral_axis == 0 ? 'x' : 'y',
         (long)g_level.zero[lateral_axis]);
  return OK;
}

bool camera_gallery_level_capture_allowed(
  enum camera_gallery_level_status_e status)
{
  /* Preserve manual photography if the sensor is absent or has failed. */

  return status == CAMERA_GALLERY_LEVEL_READY ||
         status == CAMERA_GALLERY_LEVEL_UNAVAILABLE;
}

void camera_gallery_level_finalize(void)
{
  if (g_level.fd >= 0)
    {
      close(g_level.fd);
    }

  memset(&g_level, 0, sizeof(g_level));
  g_level.fd = -1;
}
