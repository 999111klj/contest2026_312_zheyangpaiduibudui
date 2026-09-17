/****************************************************************************
 * apps/examples/camera_gallery/camera_gallery_aivlog.c
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "camera_gallery.h"
#include "camera_gallery_aivlog_model.h"

#define AIVLOG_MAX_IMAGES 10000
#define AIVLOG_PATH_SIZE  128
#define AIVLOG_TOP_COUNT  5

static pthread_mutex_t g_aivlog_lock = PTHREAD_MUTEX_INITIALIZER;

struct aivlog_live_s
{
  pthread_mutex_t lock;
  pthread_cond_t cond;
  pthread_t thread;
  FAR uint16_t *pixels;
  struct timespec last_submit;
  uint32_t generation;
  uint32_t sequence;
  bool initialized;
  bool thread_started;
  bool stopping;
  bool pending;
  bool busy;
  bool has_last_submit;
};

static struct aivlog_live_s g_aivlog_live;

static bool aivlog_live_interval_elapsed(FAR const struct timespec *now)
{
  int64_t milliseconds;

  milliseconds = (int64_t)(now->tv_sec -
                 g_aivlog_live.last_submit.tv_sec) * 1000 +
                 ((int64_t)now->tv_nsec -
                  g_aivlog_live.last_submit.tv_nsec) / 1000000;
  return milliseconds >=
         CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_INTERVAL_MS;
}

static FAR void *aivlog_live_worker(FAR void *arg)
{
  struct camera_gallery_aivlog_score_s result;
  uint32_t sequence;
  bool stopping;
  int ret;

  (void)arg;
  for (; ; )
    {
      pthread_mutex_lock(&g_aivlog_live.lock);
      while (!g_aivlog_live.pending && !g_aivlog_live.stopping)
        {
          pthread_cond_wait(&g_aivlog_live.cond, &g_aivlog_live.lock);
        }

      if (g_aivlog_live.stopping && !g_aivlog_live.pending)
        {
          pthread_mutex_unlock(&g_aivlog_live.lock);
          break;
        }

      g_aivlog_live.pending = false;
      sequence = g_aivlog_live.sequence;
      pthread_mutex_unlock(&g_aivlog_live.lock);

      ret = camera_gallery_aivlog_model_score(
        g_aivlog_live.pixels, CAMERA_GALLERY_WIDTH,
        CAMERA_GALLERY_HEIGHT, &result);

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD
      if (ret == OK &&
          camera_gallery_aivlog_session_candidate(
            g_aivlog_live.pixels, &result) < 0 &&
          errno != ENODEV && errno != ENOSPC)
        {
          printf("[aivlog-cloud] candidate failed: %d\n", errno);
        }
#endif

      /* Keep busy asserted until any accepted high-score snapshot has been
       * persisted.  This prevents live_submit() from overwriting the worker's
       * private RGB565 buffer while the session module hashes or saves it.
       */

      pthread_mutex_lock(&g_aivlog_live.lock);
      stopping = g_aivlog_live.stopping;
      g_aivlog_live.busy = false;
      pthread_mutex_unlock(&g_aivlog_live.lock);

      if (stopping)
        {
          continue;
        }

      if (ret < 0)
        {
          int score_errno = errno;

          printf("[aivlog] live frame=%lu failed: errno=%d\n",
                 (unsigned long)sequence, score_errno);
          camera_gallery_nx_score_error(g_aivlog_live.generation,
                                        "AI ERROR");
        }
      else
        {
          printf("[aivlog] live frame=%lu raw=%.6f score=%d "
                 "latency_ms=%lu\n",
                 (unsigned long)sequence, (double)result.raw_score,
                 result.score, (unsigned long)result.latency_ms);
          camera_gallery_nx_score_result(g_aivlog_live.generation,
                                         result.score, "PLAY PHOTO");
        }
    }

  return NULL;
}

int camera_gallery_aivlog_live_start(void)
{
  struct sched_param param;
  pthread_attr_t attr;
  int saved_errno;
  int ret;

  ret = pthread_mutex_trylock(&g_aivlog_lock);
  if (ret != 0)
    {
      errno = ret;
      printf("[aivlog] scorer is already running\n");
      return ERROR;
    }

  memset(&g_aivlog_live, 0, sizeof(g_aivlog_live));
  ret = pthread_mutex_init(&g_aivlog_live.lock, NULL);
  if (ret != 0)
    {
      pthread_mutex_unlock(&g_aivlog_lock);
      errno = ret;
      return ERROR;
    }

  ret = pthread_cond_init(&g_aivlog_live.cond, NULL);
  if (ret != 0)
    {
      pthread_mutex_destroy(&g_aivlog_live.lock);
      pthread_mutex_unlock(&g_aivlog_lock);
      errno = ret;
      return ERROR;
    }

  g_aivlog_live.initialized = true;
  g_aivlog_live.pixels = memalign(32, CAMERA_GALLERY_IMAGE_SIZE);
  if (g_aivlog_live.pixels == NULL)
    {
      errno = ENOMEM;
      goto fail;
    }

  if (camera_gallery_aivlog_model_initialize() < 0)
    {
      printf("[aivlog] live model initialization failed: %d\n", errno);
      goto fail;
    }

  g_aivlog_live.generation = camera_gallery_nx_score_live_pending();
  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      ret = pthread_attr_setstacksize(
        &attr, CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_STACKSIZE);
      if (ret == 0)
        {
          memset(&param, 0, sizeof(param));
          param.sched_priority =
            CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_PRIORITY;
          ret = pthread_attr_setschedparam(&attr, &param);
        }

      if (ret == 0)
        {
          ret = pthread_attr_setinheritsched(&attr,
                                             PTHREAD_EXPLICIT_SCHED);
        }

      if (ret == 0)
        {
          ret = pthread_create(&g_aivlog_live.thread, &attr,
                               aivlog_live_worker, NULL);
        }

      pthread_attr_destroy(&attr);
    }

  if (ret != 0)
    {
      errno = ret;
      goto fail;
    }

  g_aivlog_live.thread_started = true;
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD
  if (camera_gallery_aivlog_cloud_initialize() < 0 ||
      camera_gallery_aivlog_session_start() < 0)
    {
      printf("[aivlog-cloud] local session unavailable: %d\n", errno);
    }
#endif
  printf("[aivlog] live scoring ready, priority=%d interval_ms=%d, "
         "PLAY saves photo, MENU exits\n",
         CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_PRIORITY,
         CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_INTERVAL_MS);
  return OK;

fail:
  saved_errno = errno;
  camera_gallery_aivlog_live_stop();
  errno = saved_errno;
  return ERROR;
}

int camera_gallery_aivlog_live_submit(FAR const uint16_t *pixels)
{
  struct timespec now;
  bool have_now;

  if (!g_aivlog_live.initialized || !g_aivlog_live.thread_started ||
      pixels == NULL)
    {
      errno = ENODEV;
      return ERROR;
    }

  have_now = clock_gettime(CLOCK_MONOTONIC, &now) == 0;
  pthread_mutex_lock(&g_aivlog_live.lock);
  if (g_aivlog_live.stopping || g_aivlog_live.busy ||
      (CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_INTERVAL_MS > 0 &&
       have_now && g_aivlog_live.has_last_submit &&
       !aivlog_live_interval_elapsed(&now)))
    {
      pthread_mutex_unlock(&g_aivlog_live.lock);
      errno = g_aivlog_live.stopping ? ECANCELED : EBUSY;
      return ERROR;
    }

  memcpy(g_aivlog_live.pixels, pixels, CAMERA_GALLERY_IMAGE_SIZE);
  if (have_now)
    {
      g_aivlog_live.last_submit = now;
      g_aivlog_live.has_last_submit = true;
    }

  g_aivlog_live.sequence++;
  g_aivlog_live.busy = true;
  g_aivlog_live.pending = true;
  pthread_cond_signal(&g_aivlog_live.cond);
  pthread_mutex_unlock(&g_aivlog_live.lock);
  return OK;
}

void camera_gallery_aivlog_live_stop(void)
{
  if (!g_aivlog_live.initialized)
    {
      return;
    }

  pthread_mutex_lock(&g_aivlog_live.lock);
  g_aivlog_live.stopping = true;
  pthread_cond_signal(&g_aivlog_live.cond);
  pthread_mutex_unlock(&g_aivlog_live.lock);

  if (g_aivlog_live.thread_started)
    {
      pthread_join(g_aivlog_live.thread, NULL);
      g_aivlog_live.thread_started = false;
    }

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD
  /* The worker has now finished any inference and local SD commit that was
   * already in flight.  Mark the session FINISHED only after that commit;
   * the independent uploader is not joined here.
   */

  camera_gallery_aivlog_session_finish();
#endif

  camera_gallery_aivlog_model_finalize();
  free(g_aivlog_live.pixels);
  g_aivlog_live.pixels = NULL;
  camera_gallery_nx_score_clear();
  pthread_cond_destroy(&g_aivlog_live.cond);
  pthread_mutex_destroy(&g_aivlog_live.lock);
  memset(&g_aivlog_live, 0, sizeof(g_aivlog_live));
  pthread_mutex_unlock(&g_aivlog_lock);
  printf("[aivlog] live scoring stopped\n");
}

struct aivlog_top_s
{
  unsigned int number;
  struct camera_gallery_aivlog_score_s result;
};

static void aivlog_update_top(FAR struct aivlog_top_s *top, int *top_count,
                              unsigned int number,
                              FAR const struct camera_gallery_aivlog_score_s *r)
{
  int index;
  int count = *top_count;

  for (index = 0; index < count; index++)
    {
      if (r->raw_score > top[index].result.raw_score)
        {
          break;
        }
    }

  if (index >= AIVLOG_TOP_COUNT)
    {
      return;
    }

  if (count < AIVLOG_TOP_COUNT)
    {
      count++;
    }

  if (index + 1 < count)
    {
      memmove(&top[index + 1], &top[index],
              (size_t)(count - index - 1) * sizeof(top[0]));
    }

  top[index].number = number;
  top[index].result = *r;
  *top_count = count;
}

static void aivlog_show_progress(int current, int total,
                                 unsigned int number, int score)
{
  char line1[24];
  char line2[24];

  snprintf(line1, sizeof(line1), "IMG %04u %d/%d", number, current, total);
  snprintf(line2, sizeof(line2), "SCORE %d", score);
  camera_gallery_nx_page("AI VLOG", line1, line2, true);
}

int camera_gallery_aivlog_score_sd(bool show_ui)
{
  struct camera_gallery_aivlog_score_s result;
  struct aivlog_top_s top[AIVLOG_TOP_COUNT];
  FAR unsigned int *numbers = NULL;
  FAR uint16_t *pixels = NULL;
  unsigned int maximum;
  char directory[AIVLOG_PATH_SIZE];
  char path[AIVLOG_PATH_SIZE];
  int total;
  int succeeded = 0;
  int failed = 0;
  int minimum = 100;
  int maximum_score = 0;
  int top_count = 0;
  int64_t score_sum = 0;
  int ret = ERROR;
  int i;

  if (snprintf(directory, sizeof(directory), "%s/DCIM",
               CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT) >=
      sizeof(directory))
    {
      errno = ENAMETOOLONG;
      return ERROR;
    }

  ret = pthread_mutex_trylock(&g_aivlog_lock);
  if (ret != 0)
    {
      errno = ret;
      printf("[aivlog] scorer is already running\n");
      return ERROR;
    }

  ret = ERROR;
  numbers = malloc(AIVLOG_MAX_IMAGES * sizeof(*numbers));
  pixels = malloc(CAMERA_GALLERY_IMAGE_SIZE);
  if (numbers == NULL || pixels == NULL)
    {
      errno = ENOMEM;
      printf("[aivlog] image buffer allocation failed\n");
      goto out;
    }

  total = camera_gallery_scan(directory, numbers, AIVLOG_MAX_IMAGES,
                              &maximum);
  if (total < 0)
    {
      printf("[aivlog] scan %s failed: %d\n", directory, errno);
      goto out;
    }
  else if (total > AIVLOG_MAX_IMAGES)
    {
      errno = EOVERFLOW;
      printf("[aivlog] too many images: %d\n", total);
      goto out;
    }
  else if (total == 0)
    {
      errno = ENOENT;
      printf("[aivlog] no IMG_NNNN.BMP files in %s\n", directory);
      if (show_ui)
        {
          camera_gallery_nx_page("AI VLOG", "NO PHOTOS", "MENU BACK",
                                 true);
        }

      goto out;
    }

  printf("[aivlog] scoring %d SD images with embedded NIMA TFLite model\n",
         total);
  if (show_ui)
    {
      camera_gallery_nx_page("AI VLOG", "MODEL INIT", "PLEASE WAIT", true);
    }

  if (camera_gallery_aivlog_model_initialize() < 0)
    {
      printf("[aivlog] model initialization failed: %d\n", errno);
      if (show_ui)
        {
          camera_gallery_nx_page("AI VLOG", "MODEL ERROR", "MENU BACK",
                                 true);
        }

      goto out;
    }

  memset(top, 0, sizeof(top));
  for (i = 0; i < total; i++)
    {
      int path_length = snprintf(path, sizeof(path), "%s/IMG_%04u.BMP",
                                 directory, numbers[i]);
      if (path_length < 0 || (size_t)path_length >= sizeof(path))
        {
          errno = ENAMETOOLONG;
          failed++;
          printf("[aivlog] IMG_%04u.BMP failed: path too long\n",
                 numbers[i]);
          continue;
        }

      if (camera_gallery_bmp_load(path, pixels) < 0)
        {
          failed++;
          printf("[aivlog] IMG_%04u.BMP failed: load errno=%d\n",
                 numbers[i], errno);
          continue;
        }

      if (camera_gallery_aivlog_model_score(
            pixels, CAMERA_GALLERY_WIDTH, CAMERA_GALLERY_HEIGHT,
            &result) < 0)
        {
          failed++;
          printf("[aivlog] IMG_%04u.BMP failed: inference errno=%d\n",
                 numbers[i], errno);
          continue;
        }

      succeeded++;
      score_sum += result.score;
      if (result.score < minimum)
        {
          minimum = result.score;
        }

      if (result.score > maximum_score)
        {
          maximum_score = result.score;
        }

      aivlog_update_top(top, &top_count, numbers[i], &result);
      printf("[aivlog] IMG_%04u.BMP raw=%.6f score=%d latency_ms=%lu\n",
             numbers[i], (double)result.raw_score, result.score,
             (unsigned long)result.latency_ms);

      if (show_ui)
        {
          aivlog_show_progress(i + 1, total, numbers[i], result.score);
        }
    }

  if (succeeded == 0)
    {
      printf("[aivlog] summary count=%d succeeded=0 failed=%d\n",
             total, failed);
      errno = EIO;
      if (show_ui)
        {
          camera_gallery_nx_page("AI VLOG", "ALL FAILED", "MENU BACK",
                                 true);
        }

      goto finalize;
    }

  printf("[aivlog] summary count=%d succeeded=%d failed=%d min=%d max=%d avg=%.2f\n",
         total, succeeded, failed, minimum, maximum_score,
         (double)score_sum / succeeded);
  for (i = 0; i < top_count; i++)
    {
      printf("[aivlog] top%d IMG_%04u.BMP raw=%.6f score=%d latency_ms=%lu\n",
             i + 1, top[i].number, (double)top[i].result.raw_score,
             top[i].result.score,
             (unsigned long)top[i].result.latency_ms);
    }

  if (show_ui)
    {
      char line[40];
      snprintf(line, sizeof(line), "DONE %d FAIL %d", succeeded, failed);
      camera_gallery_nx_page("AI VLOG", line, "MENU BACK", true);
    }

  ret = failed == 0 ? OK : ERROR;
  if (ret < 0)
    {
      errno = EIO;
    }

finalize:
  camera_gallery_aivlog_model_finalize();
out:
  free(pixels);
  free(numbers);
  pthread_mutex_unlock(&g_aivlog_lock);
  return ret;
}
