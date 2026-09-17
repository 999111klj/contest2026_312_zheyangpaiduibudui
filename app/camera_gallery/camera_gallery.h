/****************************************************************************
 * apps/examples/camera_gallery/camera_gallery.h
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_CAMERA_GALLERY_CAMERA_GALLERY_H
#define __APPS_EXAMPLES_CAMERA_GALLERY_CAMERA_GALLERY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_EXAMPLES_CAMERA_GALLERY_DEVPATH
#  define CONFIG_EXAMPLES_CAMERA_GALLERY_DEVPATH "/dev/video0"
#endif

#ifndef CONFIG_EXAMPLES_CAMERA_GALLERY_BUTTONS_DEVPATH
#  define CONFIG_EXAMPLES_CAMERA_GALLERY_BUTTONS_DEVPATH "/dev/buttons"
#endif

#ifndef CONFIG_EXAMPLES_CAMERA_GALLERY_ADC_DEVPATH
#  define CONFIG_EXAMPLES_CAMERA_GALLERY_ADC_DEVPATH "/dev/adc0"
#endif

#ifndef CONFIG_EXAMPLES_CAMERA_GALLERY_SD_DEVPATH
#  define CONFIG_EXAMPLES_CAMERA_GALLERY_SD_DEVPATH "/dev/mmcsd1"
#endif

#ifndef CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT
#  define CONFIG_EXAMPLES_CAMERA_GALLERY_SD_MOUNTPOINT "/mnt/sd"
#endif

#ifndef CONFIG_EXAMPLES_CAMERA_GALLERY_LEVEL_DEVPATH
#  define CONFIG_EXAMPLES_CAMERA_GALLERY_LEVEL_DEVPATH "/dev/accel0"
#endif

#define CAMERA_GALLERY_WIDTH       320
#define CAMERA_GALLERY_HEIGHT      240
#define CAMERA_GALLERY_IMAGE_SIZE  (CAMERA_GALLERY_WIDTH * \
                                    CAMERA_GALLERY_HEIGHT * 2)
#define CAMERA_GALLERY_BMP_HEADER_SIZE  66
#define CAMERA_GALLERY_BMP_SIZE         (CAMERA_GALLERY_BMP_HEADER_SIZE + \
                                         CAMERA_GALLERY_IMAGE_SIZE)
#define CAMERA_GALLERY_ZH_FONT_WIDTH          16
#define CAMERA_GALLERY_ZH_FONT_HEIGHT         16
#define CAMERA_GALLERY_ADVICE_MAX_CHARS       36
#define CAMERA_GALLERY_ADVICE_CHARS_PER_LINE  12
#define CAMERA_GALLERY_ADVICE_MAX_LINES       3
#define CAMERA_GALLERY_ADVICE_BUFFER_SIZE     128

#define CAMERA_GALLERY_BUTTON_BOOT  (1u << 0)
#define CAMERA_GALLERY_BUTTON_MENU  (1u << 1)
#define CAMERA_GALLERY_BUTTON_PLAY  (1u << 2)
#define CAMERA_GALLERY_BUTTON_DOWN  (1u << 3)
#define CAMERA_GALLERY_BUTTON_UP    (1u << 4)
#define CAMERA_GALLERY_BUTTON_MASK  (CAMERA_GALLERY_BUTTON_MENU | \
                                     CAMERA_GALLERY_BUTTON_PLAY | \
                                     CAMERA_GALLERY_BUTTON_DOWN | \
                                     CAMERA_GALLERY_BUTTON_UP)

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum camera_gallery_state_e
{
  CAMERA_GALLERY_HOME = 0,
  CAMERA_GALLERY_CAMERA_PREVIEW,
  CAMERA_GALLERY_PHOTO_REVIEW,
  CAMERA_GALLERY_GALLERY,
  CAMERA_GALLERY_AIVLOG
};

enum camera_gallery_level_status_e
{
  CAMERA_GALLERY_LEVEL_UNAVAILABLE = 0,
  CAMERA_GALLERY_LEVEL_HOLD,
  CAMERA_GALLERY_LEVEL_ADJUST,
  CAMERA_GALLERY_LEVEL_READY
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int camera_gallery_bmp_save(FAR const char *path,
                            FAR const uint16_t *pixels);
int camera_gallery_bmp_encode(FAR const uint16_t *pixels,
                              FAR uint8_t *bmp, size_t bmp_size);
int camera_gallery_bmp_load(FAR const char *path, FAR uint16_t *pixels);
int camera_gallery_scan(FAR const char *directory,
                        FAR unsigned int *numbers, int capacity,
                        FAR unsigned int *maximum);

int camera_gallery_level_initialize(void);
enum camera_gallery_level_status_e
camera_gallery_level_update(FAR int *tilt_permille);
int camera_gallery_level_calibrate(void);
bool camera_gallery_level_capture_allowed(
  enum camera_gallery_level_status_e status);
void camera_gallery_level_finalize(void);

int camera_gallery_nx_initialize(void);
void camera_gallery_nx_draw(FAR const uint16_t *pixels, int width,
                            int height);
void camera_gallery_nx_home(int selection);
void camera_gallery_nx_page(FAR const char *title, FAR const char *line1,
                            FAR const char *line2, bool selected);
void camera_gallery_nx_level_update(
  enum camera_gallery_level_status_e status, int tilt_permille);
void camera_gallery_nx_level_clear(void);
uint32_t camera_gallery_nx_score_pending(void);
uint32_t camera_gallery_nx_score_live_pending(void);
void camera_gallery_nx_score_result(uint32_t generation, int score,
                                    FAR const char *advice);
void camera_gallery_nx_score_error(uint32_t generation,
                                   FAR const char *message);
void camera_gallery_nx_score_clear(void);
void camera_gallery_nx_finalize(void);
FAR const uint16_t *camera_gallery_zh_glyph(uint32_t codepoint);

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG
int camera_gallery_aivlog_live_start(void);
int camera_gallery_aivlog_live_submit(FAR const uint16_t *pixels);
void camera_gallery_aivlog_live_stop(void);
int camera_gallery_aivlog_score_sd(bool show_ui);
#endif

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD
struct camera_gallery_aivlog_score_s;
int camera_gallery_aivlog_cloud_initialize(void);
void camera_gallery_aivlog_cloud_finalize(void);
int camera_gallery_aivlog_cloud_configure(FAR const char *scheme,
                                          FAR const char *host,
                                          uint16_t port,
                                          FAR const char *token,
                                          FAR const char *cert_pin,
                                          FAR const char *path);
int camera_gallery_aivlog_session_start(void);
int camera_gallery_aivlog_session_candidate(
  FAR const uint16_t *pixels,
  FAR const struct camera_gallery_aivlog_score_s *result);
void camera_gallery_aivlog_session_finish(void);
void camera_gallery_aivlog_cloud_wake(void);
int camera_gallery_aivlog_cloud_command(int argc, FAR char *argv[]);
#endif

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_PROVISION
int camera_gallery_aivlog_provision_initialize(void);
void camera_gallery_aivlog_provision_finalize(void);
#endif

#ifdef CONFIG_EXAMPLES_CAMERA_GALLERY_CLOUD_SCORE
int camera_gallery_cloud_initialize(void);
void camera_gallery_cloud_finalize(void);
int camera_gallery_cloud_submit(FAR const uint16_t *pixels,
                                uint32_t generation);
int camera_gallery_cloud_command(int argc, FAR char *argv[]);
int camera_gallery_upload_command(int argc, FAR char *argv[]);
#endif

#endif
