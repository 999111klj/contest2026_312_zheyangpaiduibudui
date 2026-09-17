/****************************************************************************
 * apps/examples/camera_gallery/camera_gallery_cloud.c
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "infra/network_manager.h"
#include "infra/vela_tls.h"
#include "llm/llm_proxy.h"

#include "camera_gallery.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAMERA_GALLERY_SCORE_RESPONSE_SIZE  512
#define CAMERA_GALLERY_MIMO_HOST             "api.llm.mioffice.cn"
#define CAMERA_GALLERY_MIMO_MODEL            "xiaomi/mimo-v2.5"

#define CAMERA_GALLERY_SCORE_PROMPT                                  \
  "请从清晰度、曝光、构图、主体表现和背景干扰等方面分析照片。"  \
  "返回且仅返回一个JSON对象："                                      \
  "{\"score\":0到100的整数,\"advice\":\"不超过36个简体中文字符的" \
  "具体拍摄建议\"}。建议必须指出主要问题并给出可执行改进方法。" \
  "不要返回Markdown、换行说明或JSON以外的内容。"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct camera_gallery_cloud_s
{
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  FAR uint8_t *bmp;
  size_t bmp_size;
  uint32_t generation;
  bool initialized;
  bool thread_started;
  bool pending;
  bool busy;
  bool stopping;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct camera_gallery_cloud_s g_cloud;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR const char *camera_gallery_default_advice(int score)
{
  if (score >= 90)
    {
      return "画面优秀，继续保持当前拍摄方式";
    }

  if (score >= 60)
    {
      return "调整构图，让主体更突出";
    }

  return "保持相机稳定，并增加环境光线";
}

static bool camera_gallery_utf8_decode(FAR const char *text, size_t length,
                                       FAR size_t *index,
                                       FAR uint32_t *codepoint)
{
  FAR const uint8_t *bytes = (FAR const uint8_t *)text;
  uint8_t first;

  if (*index >= length)
    {
      return false;
    }

  first = bytes[*index];
  if (first < 0x80)
    {
      *codepoint = first;
      (*index)++;
      return true;
    }

  if ((first & 0xe0) == 0xc0 && *index + 1 < length &&
      first >= 0xc2 && (bytes[*index + 1] & 0xc0) == 0x80)
    {
      *codepoint = ((uint32_t)(first & 0x1f) << 6) |
                   (uint32_t)(bytes[*index + 1] & 0x3f);
      *index += 2;
      return true;
    }

  if ((first & 0xf0) == 0xe0 && *index + 2 < length &&
      (bytes[*index + 1] & 0xc0) == 0x80 &&
      (bytes[*index + 2] & 0xc0) == 0x80 &&
      !(first == 0xe0 && bytes[*index + 1] < 0xa0) &&
      !(first == 0xed && bytes[*index + 1] >= 0xa0))
    {
      *codepoint = ((uint32_t)(first & 0x0f) << 12) |
                   ((uint32_t)(bytes[*index + 1] & 0x3f) << 6) |
                   (uint32_t)(bytes[*index + 2] & 0x3f);
      *index += 3;
      return true;
    }

  return false;
}

static bool camera_gallery_advice_codepoint_allowed(uint32_t codepoint)
{
  if (codepoint >= 0x4e00 && codepoint <= 0x9fff)
    {
      return camera_gallery_zh_glyph(codepoint) != NULL;
    }

  switch (codepoint)
    {
      case 0x2014: /* — */
      case 0x2018: /* ‘ */
      case 0x2019: /* ’ */
      case 0x201c: /* “ */
      case 0x201d: /* ” */
      case 0x2026: /* … */
      case 0x3001: /* 、 */
      case 0x3002: /* 。 */
      case 0x300a: /* 《 */
      case 0x300b: /* 》 */
      case 0x3010: /* 【 */
      case 0x3011: /* 】 */
      case 0xff01: /* ！ */
      case 0xff08: /* （ */
      case 0xff09: /* ） */
      case 0xff0c: /* ， */
      case 0xff1a: /* ： */
      case 0xff1b: /* ； */
      case 0xff1f: /* ？ */
        return camera_gallery_zh_glyph(codepoint) != NULL;

      default:
        return false;
    }
}

static int camera_gallery_copy_advice(FAR const char *source,
                                      FAR char *destination,
                                      size_t destination_size)
{
  size_t source_length;
  size_t source_index = 0;
  size_t destination_length = 0;
  size_t character_count = 0;

  if (source == NULL || destination == NULL || destination_size == 0)
    {
      errno = EINVAL;
      return ERROR;
    }

  source_length = strlen(source);
  while (source_index < source_length)
    {
      uint32_t codepoint;
      size_t character_start = source_index;
      size_t character_length;

      if (!camera_gallery_utf8_decode(source, source_length, &source_index,
                                      &codepoint) ||
          !camera_gallery_advice_codepoint_allowed(codepoint))
        {
          errno = EILSEQ;
          return ERROR;
        }

      character_length = source_index - character_start;
      if (character_count < CAMERA_GALLERY_ADVICE_MAX_CHARS)
        {
          if (destination_length + character_length >= destination_size)
            {
              errno = ENOSPC;
              return ERROR;
            }

          memcpy(destination + destination_length,
                 source + character_start, character_length);
          destination_length += character_length;
        }

      character_count++;
    }

  if (character_count == 0)
    {
      errno = EINVAL;
      return ERROR;
    }

  destination[destination_length] = '\0';
  return OK;
}

static int camera_gallery_parse_score(FAR const char *response,
                                      FAR int *score,
                                      FAR char *advice,
                                      size_t advice_size)
{
  FAR const char *selected_advice;
  FAR cJSON *root;
  FAR cJSON *score_item;
  FAR cJSON *advice_item;
  int value;

  if (response == NULL || score == NULL || advice == NULL ||
      advice_size == 0)
    {
      errno = EINVAL;
      return ERROR;
    }

  root = cJSON_ParseWithOpts(response, NULL, true);
  if (!cJSON_IsObject(root) || cJSON_GetArraySize(root) != 2)
    {
      cJSON_Delete(root);
      errno = EINVAL;
      return ERROR;
    }

  score_item = cJSON_GetObjectItemCaseSensitive(root, "score");
  advice_item = cJSON_GetObjectItemCaseSensitive(root, "advice");
  if (!cJSON_IsNumber(score_item) || !cJSON_IsString(advice_item))
    {
      cJSON_Delete(root);
      errno = EINVAL;
      return ERROR;
    }

  value = score_item->valueint;
  if ((double)value != score_item->valuedouble || value < 0 || value > 100)
    {
      cJSON_Delete(root);
      errno = ERANGE;
      return ERROR;
    }

  selected_advice = advice_item->valuestring;
  if (camera_gallery_copy_advice(selected_advice, advice,
                                 advice_size) < 0)
    {
      selected_advice = camera_gallery_default_advice(value);
      if (camera_gallery_copy_advice(selected_advice, advice,
                                     advice_size) < 0)
        {
          cJSON_Delete(root);
          return ERROR;
        }
    }

  *score = value;
  cJSON_Delete(root);
  return OK;
}

static FAR const char *camera_gallery_cloud_error(FAR const char *response)
{
  if (response != NULL)
    {
      if (strstr(response, "No API key") != NULL ||
          strstr(response, "HTTP 401") != NULL)
        {
          return "KEY ERROR";
        }

      if (strstr(response, "HTTP 429") != NULL)
        {
          return "AI BUSY";
        }

      if (strstr(response, "HTTP 400") != NULL)
        {
          return "API ERROR";
        }
    }

  return "NET ERROR";
}

static FAR void *camera_gallery_cloud_worker(FAR void *arg)
{
  char response[CAMERA_GALLERY_SCORE_RESPONSE_SIZE];
  char advice[CAMERA_GALLERY_ADVICE_BUFFER_SIZE];
  FAR uint8_t *bmp;
  size_t bmp_size;
  uint32_t generation;
  int score;
  int ret;

  for (; ; )
    {
      pthread_mutex_lock(&g_cloud.lock);
      while (!g_cloud.pending && !g_cloud.stopping)
        {
          pthread_cond_wait(&g_cloud.cond, &g_cloud.lock);
        }

      if (g_cloud.stopping)
        {
          pthread_mutex_unlock(&g_cloud.lock);
          break;
        }

      bmp = g_cloud.bmp;
      bmp_size = g_cloud.bmp_size;
      generation = g_cloud.generation;
      g_cloud.bmp = NULL;
      g_cloud.bmp_size = 0;
      g_cloud.pending = false;
      pthread_mutex_unlock(&g_cloud.lock);

      response[0] = '\0';
      if (!network_is_connected() && network_wifi_reconnect() < 0)
        {
          camera_gallery_nx_score_error(generation, "NET ERROR");
        }
      else
        {
          ret = llm_chat_vision_raw(CAMERA_GALLERY_SCORE_PROMPT,
                                    bmp, bmp_size, "image/bmp",
                                    response, sizeof(response));
          if (ret < 0)
            {
              printf("camera_gallery: MiMo request failed: %s\n", response);
              camera_gallery_nx_score_error(
                generation, camera_gallery_cloud_error(response));
            }
          else if (camera_gallery_parse_score(response, &score,
                                              advice, sizeof(advice)) < 0)
            {
              printf("camera_gallery: invalid MiMo score/advice: %s\n",
                     response);
              camera_gallery_nx_score_error(
                generation,
                errno == ERANGE ? "SCORE ERROR" : "JSON ERROR");
            }
          else
            {
              printf("camera_gallery: MiMo score %d advice %s\n",
                     score, advice);
              camera_gallery_nx_score_result(generation, score, advice);
            }
        }

      free(bmp);
      pthread_mutex_lock(&g_cloud.lock);
      g_cloud.busy = false;
      pthread_mutex_unlock(&g_cloud.lock);
    }

  return NULL;
}

static int camera_gallery_cloud_config_init(void)
{
  if (config_store_init() < 0 || http_proxy_init() < 0 ||
      llm_proxy_init() < 0)
    {
      return ERROR;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int camera_gallery_cloud_initialize(void)
{
  pthread_attr_t attr;
  int ret;

  memset(&g_cloud, 0, sizeof(g_cloud));
  ret = pthread_mutex_init(&g_cloud.lock, NULL);
  if (ret != 0)
    {
      errno = ret;
      return ERROR;
    }

  ret = pthread_cond_init(&g_cloud.cond, NULL);
  if (ret != 0)
    {
      pthread_mutex_destroy(&g_cloud.lock);
      errno = ret;
      return ERROR;
    }

  g_cloud.initialized = true;
  if (camera_gallery_cloud_config_init() < 0)
    {
      camera_gallery_cloud_finalize();
      return ERROR;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(
    &attr, CONFIG_EXAMPLES_CAMERA_GALLERY_CLOUD_STACKSIZE);
  ret = pthread_create(&g_cloud.thread, &attr,
                       camera_gallery_cloud_worker, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      errno = ret;
      camera_gallery_cloud_finalize();
      return ERROR;
    }

  g_cloud.thread_started = true;
  return OK;
}

void camera_gallery_cloud_finalize(void)
{
  if (!g_cloud.initialized)
    {
      return;
    }

  pthread_mutex_lock(&g_cloud.lock);
  g_cloud.stopping = true;
  pthread_cond_signal(&g_cloud.cond);
  pthread_mutex_unlock(&g_cloud.lock);

  if (g_cloud.thread_started)
    {
      pthread_join(g_cloud.thread, NULL);
      g_cloud.thread_started = false;
    }

  free(g_cloud.bmp);
  g_cloud.bmp = NULL;
  pthread_cond_destroy(&g_cloud.cond);
  pthread_mutex_destroy(&g_cloud.lock);
  g_cloud.initialized = false;
}

int camera_gallery_cloud_submit(FAR const uint16_t *pixels,
                                uint32_t generation)
{
  FAR uint8_t *bmp;

  if (!g_cloud.initialized || !g_cloud.thread_started || pixels == NULL)
    {
      errno = ENODEV;
      return ERROR;
    }

  pthread_mutex_lock(&g_cloud.lock);
  if (g_cloud.busy)
    {
      pthread_mutex_unlock(&g_cloud.lock);
      errno = EBUSY;
      return ERROR;
    }

  g_cloud.busy = true;
  pthread_mutex_unlock(&g_cloud.lock);

  bmp = malloc(CAMERA_GALLERY_BMP_SIZE);
  if (bmp == NULL ||
      camera_gallery_bmp_encode(pixels, bmp,
                                CAMERA_GALLERY_BMP_SIZE) < 0)
    {
      free(bmp);
      pthread_mutex_lock(&g_cloud.lock);
      g_cloud.busy = false;
      pthread_mutex_unlock(&g_cloud.lock);
      errno = ENOMEM;
      return ERROR;
    }

  pthread_mutex_lock(&g_cloud.lock);
  g_cloud.bmp = bmp;
  g_cloud.bmp_size = CAMERA_GALLERY_BMP_SIZE;
  g_cloud.generation = generation;
  g_cloud.pending = true;
  pthread_cond_signal(&g_cloud.cond);
  pthread_mutex_unlock(&g_cloud.lock);
  return OK;
}

int camera_gallery_cloud_command(int argc, FAR char *argv[])
{
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD
  int aivlog_result = camera_gallery_aivlog_cloud_command(argc, argv);

  if (aivlog_result != -ENOSYS)
    {
      return aivlog_result;
    }
#endif

  if (argc >= 2 && strcmp(argv[1], "upload") == 0)
    {
      return camera_gallery_upload_command(argc, argv);
    }

  if (argc == 3 && strcmp(argv[1], "http_test") == 0)
    {
      char response[256] = { 0 };
      int status;

      if (!network_is_connected() && network_wifi_reconnect() < 0)
        {
          printf("camera_gallery: network unavailable\n");
          return EXIT_FAILURE;
        }

      printf("camera_gallery: testing plain HTTP to %s:80\n", argv[2]);
      status = vela_http_post_json(argv[2], "80", "/", NULL, "{}",
                                  response, sizeof(response));
      if (status < 0)
        {
          printf("camera_gallery: HTTP test failed for %s\n", argv[2]);
          return EXIT_FAILURE;
        }

      printf("camera_gallery: HTTP test OK for %s (status=%d)\n",
             argv[2], status);
      return EXIT_SUCCESS;
    }

  if (argc == 3 && strcmp(argv[1], "tls_test") == 0)
    {
      char date[64] = { 0 };

      if (!network_is_connected() && network_wifi_reconnect() < 0)
        {
          printf("camera_gallery: network unavailable\n");
          return EXIT_FAILURE;
        }

      printf("camera_gallery: testing TLS to %s:443\n", argv[2]);
      if (vela_https_head_date(argv[2], "443", "/",
                              date, sizeof(date)) < 0)
        {
          printf("camera_gallery: TLS test failed for %s\n", argv[2]);
          return EXIT_FAILURE;
        }

      printf("camera_gallery: TLS test OK for %s (Date: %s)\n",
             argv[2], date[0] != '\0' ? date : "unavailable");
      return EXIT_SUCCESS;
    }

  if (argc == 3 && strcmp(argv[1], "set_mimo") == 0)
    {
      if (config_store_init() < 0 ||
          llm_set_vision_model(CAMERA_GALLERY_MIMO_HOST,
                               CAMERA_GALLERY_MIMO_MODEL, argv[2]) < 0)
        {
          printf("camera_gallery: failed to save MiMo configuration\n");
          return EXIT_FAILURE;
        }

      printf("MiMo vision configured for %s.\n",
             CAMERA_GALLERY_MIMO_MODEL);
      return EXIT_SUCCESS;
    }

  if (argc == 4 && strcmp(argv[1], "set_wifi") == 0)
    {
      if (config_store_init() < 0 ||
          network_wifi_connect(NULL, argv[2], argv[3]) < 0)
        {
          printf("camera_gallery: Wi-Fi configuration failed\n");
          return EXIT_FAILURE;
        }

      printf("Wi-Fi connected and credentials saved.\n");
      return EXIT_SUCCESS;
    }

  if (argc == 2 && strcmp(argv[1], "clear_mimo") == 0)
    {
      if (config_store_init() < 0 ||
          llm_set_vision_model(NULL, NULL, NULL) < 0)
        {
          return EXIT_FAILURE;
        }

      printf("MiMo vision configuration cleared.\n");
      return EXIT_SUCCESS;
    }

  printf("Usage:\n"
         "  camera_gallery upload <0-9999> <pc_ip> [port]\n"
         "  camera_gallery http_test <host>\n"
         "  camera_gallery tls_test <host>\n"
         "  camera_gallery set_wifi <ssid> <password>\n"
         "  camera_gallery set_mimo <api_key>\n"
         "  camera_gallery clear_mimo\n"
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD
         "  camera_gallery set_aivlog_cloud <host> <token> "
         "<cert_sha256> [base_path]\n"
         "  camera_gallery clear_aivlog_cloud\n"
         "  camera_gallery aivlog_cloud_status\n"
#endif
         );
  return EXIT_FAILURE;
}
