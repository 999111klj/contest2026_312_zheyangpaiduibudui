/****************************************************************************
 * apps/examples/camera_gallery/camera_gallery_nx.c
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/boardctl.h>
#include <nuttx/nx/nx.h>
#include <nuttx/nx/nxglib.h>

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera_gallery.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240
#define FONT_WIDTH      5
#define FONT_HEIGHT     7
#define FONT_SCALE      3
#define FONT_ADVANCE    ((FONT_WIDTH + 1) * FONT_SCALE)
#define ZH_FONT_ADVANCE (CAMERA_GALLERY_ZH_FONT_WIDTH + 2)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct camera_gallery_nx_s
{
  NXHANDLE hnx;
  NXWINDOW window;
  pthread_t listener;
  pthread_mutex_t lock;
  sem_t sem;
  nxgl_coord_t xres;
  nxgl_coord_t yres;
  bool connected;
  bool failed;
  bool positioned;
  bool running;
  bool listener_started;
  bool lock_initialized;
  bool level_visible;
  bool score_visible;
  enum camera_gallery_level_status_e level_status;
  int level_tilt_permille;
  uint32_t score_generation;
  char score_line[16];
  char score_advice[CAMERA_GALLERY_ADVICE_BUFFER_SIZE];
  FAR nxgl_mxpixel_t *canvas;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct camera_gallery_nx_s g_nx;

/* Five column, seven row uppercase font.  Bits run from top to bottom. */

static const uint8_t g_digits[10][FONT_WIDTH] =
{
  {0x3e, 0x51, 0x49, 0x45, 0x3e},
  {0x00, 0x42, 0x7f, 0x40, 0x00},
  {0x42, 0x61, 0x51, 0x49, 0x46},
  {0x21, 0x41, 0x45, 0x4b, 0x31},
  {0x18, 0x14, 0x12, 0x7f, 0x10},
  {0x27, 0x45, 0x45, 0x45, 0x39},
  {0x3c, 0x4a, 0x49, 0x49, 0x30},
  {0x01, 0x71, 0x09, 0x05, 0x03},
  {0x36, 0x49, 0x49, 0x49, 0x36},
  {0x06, 0x49, 0x49, 0x29, 0x1e}
};

static const uint8_t g_letters[26][FONT_WIDTH] =
{
  {0x7e, 0x11, 0x11, 0x11, 0x7e}, /* A */
  {0x7f, 0x49, 0x49, 0x49, 0x36}, /* B */
  {0x3e, 0x41, 0x41, 0x41, 0x22}, /* C */
  {0x7f, 0x41, 0x41, 0x22, 0x1c}, /* D */
  {0x7f, 0x49, 0x49, 0x49, 0x41}, /* E */
  {0x7f, 0x09, 0x09, 0x09, 0x01}, /* F */
  {0x3e, 0x41, 0x49, 0x49, 0x7a}, /* G */
  {0x7f, 0x08, 0x08, 0x08, 0x7f}, /* H */
  {0x00, 0x41, 0x7f, 0x41, 0x00}, /* I */
  {0x20, 0x40, 0x41, 0x3f, 0x01}, /* J */
  {0x7f, 0x08, 0x14, 0x22, 0x41}, /* K */
  {0x7f, 0x40, 0x40, 0x40, 0x40}, /* L */
  {0x7f, 0x02, 0x0c, 0x02, 0x7f}, /* M */
  {0x7f, 0x04, 0x08, 0x10, 0x7f}, /* N */
  {0x3e, 0x41, 0x41, 0x41, 0x3e}, /* O */
  {0x7f, 0x09, 0x09, 0x09, 0x06}, /* P */
  {0x3e, 0x41, 0x51, 0x21, 0x5e}, /* Q */
  {0x7f, 0x09, 0x19, 0x29, 0x46}, /* R */
  {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
  {0x01, 0x01, 0x7f, 0x01, 0x01}, /* T */
  {0x3f, 0x40, 0x40, 0x40, 0x3f}, /* U */
  {0x1f, 0x20, 0x40, 0x20, 0x1f}, /* V */
  {0x3f, 0x40, 0x38, 0x40, 0x3f}, /* W */
  {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
  {0x07, 0x08, 0x70, 0x08, 0x07}, /* Y */
  {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int camera_gallery_nx_blit(void)
{
  FAR const void *src[CONFIG_NX_NPLANES];
  struct nxgl_rect_s dest;
  struct nxgl_point_s origin;
  nxgl_coord_t width;
  nxgl_coord_t height;

  if (g_nx.window == NULL)
    {
      return ERROR;
    }

  width = g_nx.xres < DISPLAY_WIDTH ? g_nx.xres : DISPLAY_WIDTH;
  height = g_nx.yres < DISPLAY_HEIGHT ? g_nx.yres : DISPLAY_HEIGHT;
  dest.pt1.x = (g_nx.xres - width) / 2;
  dest.pt1.y = (g_nx.yres - height) / 2;
  dest.pt2.x = dest.pt1.x + width - 1;
  dest.pt2.y = dest.pt1.y + height - 1;
  origin.x = dest.pt1.x;
  origin.y = dest.pt1.y;
  src[0] = g_nx.canvas;

  return nx_bitmap(g_nx.window, &dest, src, &origin,
                   DISPLAY_WIDTH * sizeof(nxgl_mxpixel_t));
}

static void camera_gallery_nx_redraw(NXWINDOW hwnd,
                                     FAR const struct nxgl_rect_s *rect,
                                     bool more, FAR void *arg)
{
  pthread_mutex_lock(&g_nx.lock);
  camera_gallery_nx_blit();
  pthread_mutex_unlock(&g_nx.lock);
}

static void camera_gallery_nx_position(NXWINDOW hwnd,
                                       FAR const struct nxgl_size_s *size,
                                       FAR const struct nxgl_point_s *pos,
                                       FAR const struct nxgl_rect_s *bounds,
                                       FAR void *arg)
{
  if (!g_nx.positioned)
    {
      g_nx.window = hwnd;
      g_nx.xres = bounds->pt2.x + 1;
      g_nx.yres = bounds->pt2.y + 1;
      g_nx.positioned = true;
      sem_post(&g_nx.sem);
    }
}

static const struct nx_callback_s g_nx_callbacks =
{
  camera_gallery_nx_redraw,
  camera_gallery_nx_position
#ifdef CONFIG_NX_XYINPUT
  , NULL
#endif
#ifdef CONFIG_NX_KBD
  , NULL
#endif
};

static FAR void *camera_gallery_nx_listener(FAR void *arg)
{
  while (g_nx.running)
    {
      if (nx_eventhandler(g_nx.hnx) < 0)
        {
          if (g_nx.running)
            {
              printf("camera_gallery: NX connection lost: %d\n", errno);
              g_nx.failed = true;
              sem_post(&g_nx.sem);
            }

          break;
        }

      if (!g_nx.connected)
        {
          g_nx.connected = true;
          sem_post(&g_nx.sem);
        }
    }

  return NULL;
}

static int camera_gallery_sem_wait(FAR sem_t *sem)
{
  int ret;

  do
    {
      ret = sem_wait(sem);
    }
  while (ret < 0 && errno == EINTR);

  return ret;
}

static FAR const uint8_t *camera_gallery_glyph(char ch)
{
  if (ch >= 'A' && ch <= 'Z')
    {
      return g_letters[ch - 'A'];
    }

  if (ch >= '0' && ch <= '9')
    {
      return g_digits[ch - '0'];
    }

  return NULL;
}

static void camera_gallery_draw_char(int x, int y, char ch,
                                     nxgl_mxpixel_t color)
{
  FAR const uint8_t *glyph = camera_gallery_glyph(ch);
  int column;
  int row;
  int sx;
  int sy;

  if (ch == '>')
    {
      static const uint8_t arrow[FONT_WIDTH] =
      {
        0x00, 0x22, 0x14, 0x08, 0x00
      };

      glyph = arrow;
    }

  if (glyph == NULL)
    {
      return;
    }

  for (column = 0; column < FONT_WIDTH; column++)
    {
      for (row = 0; row < FONT_HEIGHT; row++)
        {
          if ((glyph[column] & (1 << row)) == 0)
            {
              continue;
            }

          for (sy = 0; sy < FONT_SCALE; sy++)
            {
              for (sx = 0; sx < FONT_SCALE; sx++)
                {
                  int px = x + column * FONT_SCALE + sx;
                  int py = y + row * FONT_SCALE + sy;

                  if (px >= 0 && px < DISPLAY_WIDTH &&
                      py >= 0 && py < DISPLAY_HEIGHT)
                    {
                      g_nx.canvas[py * DISPLAY_WIDTH + px] = color;
                    }
                }
            }
        }
    }
}

static uint32_t camera_gallery_utf8_next(FAR const char **cursor)
{
  FAR const uint8_t *text = (FAR const uint8_t *)*cursor;
  uint32_t codepoint;

  if (text[0] == '\0')
    {
      return 0;
    }

  if (text[0] < 0x80)
    {
      *cursor += 1;
      return text[0];
    }

  if ((text[0] & 0xf0) == 0xe0 && text[1] != '\0' &&
      text[2] != '\0' && (text[1] & 0xc0) == 0x80 &&
      (text[2] & 0xc0) == 0x80)
    {
      codepoint = ((uint32_t)(text[0] & 0x0f) << 12) |
                  ((uint32_t)(text[1] & 0x3f) << 6) |
                  (uint32_t)(text[2] & 0x3f);
      *cursor += 3;
      return codepoint;
    }

  *cursor += 1;
  return 0xfffd;
}

static int camera_gallery_text_width(FAR const char *text)
{
  FAR const char *cursor = text;
  uint32_t codepoint;
  int width = 0;

  while ((codepoint = camera_gallery_utf8_next(&cursor)) != 0)
    {
      width += codepoint < 0x80 ? FONT_ADVANCE : ZH_FONT_ADVANCE;
    }

  return width;
}

static void camera_gallery_draw_zh_char(int x, int y, uint32_t codepoint,
                                        nxgl_mxpixel_t color)
{
  FAR const uint16_t *glyph = camera_gallery_zh_glyph(codepoint);
  bool pixel;
  int column;
  int row;

  for (row = 0; row < CAMERA_GALLERY_ZH_FONT_HEIGHT; row++)
    {
      for (column = 0; column < CAMERA_GALLERY_ZH_FONT_WIDTH; column++)
        {
          if (glyph != NULL)
            {
              pixel = (glyph[row] & (0x8000u >> column)) != 0;
            }
          else
            {
              pixel = row == 0 ||
                      row == CAMERA_GALLERY_ZH_FONT_HEIGHT - 1 ||
                      column == 0 ||
                      column == CAMERA_GALLERY_ZH_FONT_WIDTH - 1;
            }

          if (pixel && x + column >= 0 && x + column < DISPLAY_WIDTH &&
              y + row >= 0 && y + row < DISPLAY_HEIGHT)
            {
              g_nx.canvas[(y + row) * DISPLAY_WIDTH + x + column] = color;
            }
        }
    }
}

static void camera_gallery_draw_text_color(int y, FAR const char *text,
                                           nxgl_mxpixel_t color,
                                           bool marked)
{
  FAR const char *cursor = text;
  uint32_t codepoint;
  int width = camera_gallery_text_width(text);
  int x = (DISPLAY_WIDTH - width) / 2;

  if (marked)
    {
      camera_gallery_draw_char(x - FONT_ADVANCE, y, '>', color);
    }

  while ((codepoint = camera_gallery_utf8_next(&cursor)) != 0)
    {
      if (codepoint < 0x80)
        {
          camera_gallery_draw_char(x, y, (char)codepoint, color);
          x += FONT_ADVANCE;
        }
      else
        {
          camera_gallery_draw_zh_char(x, y, codepoint, color);
          x += ZH_FONT_ADVANCE;
        }
    }
}

static void camera_gallery_draw_text(int y, FAR const char *text,
                                     bool marked)
{
  camera_gallery_draw_text_color(y, text,
                                 marked ? 0xffe0 : 0xffff, marked);
}

static void camera_gallery_draw_level(void)
{
  FAR const char *label;
  nxgl_mxpixel_t color;
  int tilt;
  int x;
  int y;

  if (!g_nx.level_visible)
    {
      return;
    }

  switch (g_nx.level_status)
    {
      case CAMERA_GALLERY_LEVEL_READY:
        label = "SHOOT";
        color = 0x07e0;
        break;

      case CAMERA_GALLERY_LEVEL_ADJUST:
        label = "ADJUST";
        color = 0xffe0;
        break;

      case CAMERA_GALLERY_LEVEL_HOLD:
        label = "HOLD";
        color = 0xffe0;
        break;

      default:
        label = "NO IMU";
        color = 0xf800;
        break;
    }

  camera_gallery_draw_text_color(8, label, color, false);

  tilt = g_nx.level_tilt_permille;
  if (tilt > 350)
    {
      tilt = 350;
    }
  else if (tilt < -350)
    {
      tilt = -350;
    }

  /* Draw a two-pixel horizon guide.  The line becomes horizontal when the
   * filtered lateral acceleration is within the capture threshold.
   */

  for (x = 40; x < 200; x++)
    {
      y = 120 + (x - 120) * tilt / 1000;
      if (y >= 0 && y + 1 < DISPLAY_HEIGHT)
        {
          g_nx.canvas[y * DISPLAY_WIDTH + x] = color;
          g_nx.canvas[(y + 1) * DISPLAY_WIDTH + x] = color;
        }
    }

  for (y = 114; y <= 126; y++)
    {
      g_nx.canvas[y * DISPLAY_WIDTH + 120] = color;
    }
}

static void camera_gallery_draw_advice(void)
{
  FAR const char *cursor = g_nx.score_advice;
  int line;

  for (line = 0; line < CAMERA_GALLERY_ADVICE_MAX_LINES &&
                 *cursor != '\0'; line++)
    {
      char text[CAMERA_GALLERY_ADVICE_CHARS_PER_LINE * 4 + 1];
      FAR const char *start = cursor;
      size_t length;
      int characters = 0;

      while (*cursor != '\0' &&
             characters < CAMERA_GALLERY_ADVICE_CHARS_PER_LINE)
        {
          camera_gallery_utf8_next(&cursor);
          characters++;
        }

      length = (size_t)(cursor - start);
      memcpy(text, start, length);
      text[length] = '\0';
      camera_gallery_draw_text(184 + line * 19, text, false);
    }
}

static void camera_gallery_draw_score(void)
{
  int y;

  if (!g_nx.score_visible)
    {
      return;
    }

  for (y = 150; y < DISPLAY_HEIGHT; y++)
    {
      memset(&g_nx.canvas[y * DISPLAY_WIDTH], 0,
             DISPLAY_WIDTH * sizeof(*g_nx.canvas));
    }

  camera_gallery_draw_text(157, g_nx.score_line, true);
  camera_gallery_draw_advice();
}

static void camera_gallery_score_update(uint32_t generation,
                                        FAR const char *line,
                                        FAR const char *advice)
{
  pthread_mutex_lock(&g_nx.lock);

  if (g_nx.score_visible && generation == g_nx.score_generation)
    {
      snprintf(g_nx.score_line, sizeof(g_nx.score_line), "%s", line);
      snprintf(g_nx.score_advice, sizeof(g_nx.score_advice), "%s", advice);
      camera_gallery_draw_score();

      if (camera_gallery_nx_blit() < 0)
        {
          printf("camera_gallery: nx_bitmap failed: %d\n", errno);
        }
    }

  pthread_mutex_unlock(&g_nx.lock);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int camera_gallery_nx_initialize(void)
{
  nxgl_mxpixel_t background = 0;
  int ret;

  memset(&g_nx, 0, sizeof(g_nx));
  ret = pthread_mutex_init(&g_nx.lock, NULL);
  if (ret != 0)
    {
      printf("camera_gallery: canvas mutex init failed: %d\n", ret);
      return ERROR;
    }

  g_nx.lock_initialized = true;
  sem_init(&g_nx.sem, 0, 0);

  g_nx.canvas = malloc(DISPLAY_WIDTH * DISPLAY_HEIGHT *
                       sizeof(*g_nx.canvas));
  if (g_nx.canvas == NULL)
    {
      printf("camera_gallery: canvas allocation failed\n");
      camera_gallery_nx_finalize();
      return ERROR;
    }

  ret = boardctl(BOARDIOC_NX_START, 0);
  if (ret < 0)
    {
      printf("camera_gallery: BOARDIOC_NX_START failed: %d\n", errno);
      camera_gallery_nx_finalize();
      return ERROR;
    }

  g_nx.hnx = nx_connect();
  if (g_nx.hnx == NULL)
    {
      printf("camera_gallery: nx_connect failed: %d\n", errno);
      camera_gallery_nx_finalize();
      return ERROR;
    }

  g_nx.running = true;
  ret = pthread_create(&g_nx.listener, NULL,
                       camera_gallery_nx_listener, NULL);
  if (ret != 0)
    {
      printf("camera_gallery: listener create failed: %d\n", ret);
      camera_gallery_nx_finalize();
      return ERROR;
    }

  g_nx.listener_started = true;
  if (camera_gallery_sem_wait(&g_nx.sem) < 0 ||
      g_nx.failed || !g_nx.connected)
    {
      printf("camera_gallery: NX listener failed during connect\n");
      camera_gallery_nx_finalize();
      return ERROR;
    }

  nx_setbgcolor(g_nx.hnx, &background);
  ret = nx_requestbkgd(g_nx.hnx, &g_nx_callbacks, NULL);
  if (ret < 0 || camera_gallery_sem_wait(&g_nx.sem) < 0 ||
      g_nx.failed || !g_nx.positioned)
    {
      printf("camera_gallery: background window failed: %d\n", errno);
      camera_gallery_nx_finalize();
      return ERROR;
    }

  printf("camera_gallery: NX ready %dx%d\n",
         g_nx.xres, g_nx.yres);
  camera_gallery_nx_page("HOME", "CAMERA", "ALBUM", true);
  return OK;
}

void camera_gallery_nx_draw(FAR const uint16_t *pixels,
                            int width, int height)
{
  int copy_width;
  int copy_height;
  int source_x;
  int source_y;
  int dest_x;
  int dest_y;
  int x;
  int y;

  if (pixels == NULL || width <= 0 || height <= 0)
    {
      return;
    }

  pthread_mutex_lock(&g_nx.lock);
  memset(g_nx.canvas, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(*g_nx.canvas));
  copy_width = width < DISPLAY_WIDTH ? width : DISPLAY_WIDTH;
  copy_height = height < DISPLAY_HEIGHT ? height : DISPLAY_HEIGHT;
  source_x = (width - copy_width) / 2;
  source_y = (height - copy_height) / 2;
  dest_x = (DISPLAY_WIDTH - copy_width) / 2;
  dest_y = (DISPLAY_HEIGHT - copy_height) / 2;

  for (y = 0; y < copy_height; y++)
    {
      for (x = 0; x < copy_width; x++)
        {
          g_nx.canvas[(dest_y + y) * DISPLAY_WIDTH + dest_x + x] =
            pixels[(source_y + y) * width + source_x + x];
        }
    }

  camera_gallery_draw_level();
  camera_gallery_draw_score();

  if (camera_gallery_nx_blit() < 0)
    {
      printf("camera_gallery: nx_bitmap failed: %d\n", errno);
    }

  pthread_mutex_unlock(&g_nx.lock);
}

void camera_gallery_nx_home(int selection)
{
  pthread_mutex_lock(&g_nx.lock);
  g_nx.score_generation++;
  g_nx.level_visible = false;
  g_nx.score_visible = false;
  memset(g_nx.canvas, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(*g_nx.canvas));
  camera_gallery_draw_text(20, "HOME", false);
  camera_gallery_draw_text(75, "CAMERA", selection == 0);
  camera_gallery_draw_text(120, "ALBUM", selection == 1);
#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
  camera_gallery_draw_text(165, "AI VLOG", selection == 2);
#endif

  if (camera_gallery_nx_blit() < 0)
    {
      printf("camera_gallery: nx_bitmap failed: %d\n", errno);
    }

  pthread_mutex_unlock(&g_nx.lock);
}

void camera_gallery_nx_page(FAR const char *title, FAR const char *line1,
                            FAR const char *line2, bool selected)
{
  pthread_mutex_lock(&g_nx.lock);
  g_nx.score_generation++;
  g_nx.level_visible = false;
  g_nx.score_visible = false;
  memset(g_nx.canvas, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(*g_nx.canvas));
  camera_gallery_draw_text(30, title, false);
  camera_gallery_draw_text(100, line1, selected);
  camera_gallery_draw_text(145, line2, !selected);

  if (camera_gallery_nx_blit() < 0)
    {
      printf("camera_gallery: nx_bitmap failed: %d\n", errno);
    }

  pthread_mutex_unlock(&g_nx.lock);
}

void camera_gallery_nx_level_update(
  enum camera_gallery_level_status_e status, int tilt_permille)
{
  pthread_mutex_lock(&g_nx.lock);
  g_nx.level_visible = true;
  g_nx.level_status = status;
  g_nx.level_tilt_permille = tilt_permille;
  pthread_mutex_unlock(&g_nx.lock);
}

void camera_gallery_nx_level_clear(void)
{
  pthread_mutex_lock(&g_nx.lock);
  g_nx.level_visible = false;
  g_nx.level_tilt_permille = 0;
  pthread_mutex_unlock(&g_nx.lock);
}

static uint32_t camera_gallery_nx_score_begin(FAR const char *advice)
{
  uint32_t generation;

  pthread_mutex_lock(&g_nx.lock);
  g_nx.score_generation++;
  if (g_nx.score_generation == 0)
    {
      g_nx.score_generation++;
    }

  generation = g_nx.score_generation;
  g_nx.level_visible = false;
  g_nx.score_visible = true;
  snprintf(g_nx.score_line, sizeof(g_nx.score_line), "AI WAIT");
  snprintf(g_nx.score_advice, sizeof(g_nx.score_advice), "%s", advice);
  camera_gallery_draw_score();
  if (camera_gallery_nx_blit() < 0)
    {
      printf("camera_gallery: nx_bitmap failed: %d\n", errno);
    }

  pthread_mutex_unlock(&g_nx.lock);
  return generation;
}

uint32_t camera_gallery_nx_score_pending(void)
{
  return camera_gallery_nx_score_begin("PHOTO SAVED");
}

uint32_t camera_gallery_nx_score_live_pending(void)
{
  return camera_gallery_nx_score_begin("PLAY PHOTO");
}

void camera_gallery_nx_score_result(uint32_t generation, int score,
                                    FAR const char *advice)
{
  char line[16];

  snprintf(line, sizeof(line), "SCORE %d", score);
  camera_gallery_score_update(generation, line, advice);
}

void camera_gallery_nx_score_error(uint32_t generation,
                                   FAR const char *message)
{
  camera_gallery_score_update(generation, message, "MENU BACK");
}

void camera_gallery_nx_score_clear(void)
{
  pthread_mutex_lock(&g_nx.lock);
  g_nx.score_generation++;
  g_nx.score_visible = false;
  g_nx.score_line[0] = '\0';
  g_nx.score_advice[0] = '\0';
  pthread_mutex_unlock(&g_nx.lock);
}

void camera_gallery_nx_finalize(void)
{
  if (g_nx.hnx != NULL)
    {
      g_nx.running = false;
      nx_disconnect(g_nx.hnx);
      g_nx.hnx = NULL;
    }

  if (g_nx.listener_started)
    {
      pthread_join(g_nx.listener, NULL);
      g_nx.listener_started = false;
    }

  free(g_nx.canvas);
  g_nx.canvas = NULL;
  sem_destroy(&g_nx.sem);

  if (g_nx.lock_initialized)
    {
      pthread_mutex_destroy(&g_nx.lock);
      g_nx.lock_initialized = false;
    }
}
