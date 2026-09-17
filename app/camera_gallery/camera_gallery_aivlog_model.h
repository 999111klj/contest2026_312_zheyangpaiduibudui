/****************************************************************************
 * apps/examples/camera_gallery/camera_gallery_aivlog_model.h
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_CAMERA_GALLERY_CAMERA_GALLERY_AIVLOG_MODEL_H
#define __APPS_EXAMPLES_CAMERA_GALLERY_CAMERA_GALLERY_AIVLOG_MODEL_H

#include <nuttx/config.h>
#include <stdint.h>

struct camera_gallery_aivlog_score_s
{
  int score;
  float raw_score;
  uint32_t latency_ms;
};

#ifdef __cplusplus
extern "C"
{
#endif

int camera_gallery_aivlog_model_initialize(void);
int camera_gallery_aivlog_model_score(
  FAR const uint16_t *rgb565, int width, int height,
  FAR struct camera_gallery_aivlog_score_s *result);
void camera_gallery_aivlog_model_finalize(void);

#ifdef __cplusplus
}
#endif

#endif
