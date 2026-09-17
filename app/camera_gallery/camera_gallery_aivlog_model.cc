/****************************************************************************
 * apps/examples/camera_gallery/camera_gallery_aivlog_model.cc
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <float.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "tensorflow/lite/micro/kernels/conv.h"
#include "tensorflow/lite/micro/kernels/depthwise_conv.h"
#include "tensorflow/lite/micro/kernels/dequantize.h"
#include "tensorflow/lite/micro/kernels/fully_connected.h"
#include "tensorflow/lite/micro/kernels/reduce.h"
#include "tensorflow/lite/micro/kernels/softmax.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "camera_gallery_aivlog_model.h"

extern "C"
{
extern const unsigned char g_camera_gallery_aivlog_model_start[];
extern const unsigned char g_camera_gallery_aivlog_model_end[];
}

namespace
{

using AivlogResolver = tflite::MicroMutableOpResolver<8>;

static constexpr float kRawScoreMaximum = 5.98f;

static AivlogResolver *g_resolver;
static tflite::MicroInterpreter *g_interpreter;
static const tflite::Model *g_model;
static TfLiteTensor *g_input;
static TfLiteTensor *g_output;
static uint8_t *g_arena;
static bool g_initialized;

static bool tensor_shape_is(FAR const TfLiteTensor *tensor, int d0, int d1,
                            int d2, int d3)
{
  return tensor != nullptr && tensor->dims != nullptr &&
         tensor->dims->size == 4 && tensor->dims->data[0] == d0 &&
         tensor->dims->data[1] == d1 && tensor->dims->data[2] == d2 &&
         tensor->dims->data[3] == d3;
}

static uint32_t elapsed_ms(FAR const struct timespec *start,
                           FAR const struct timespec *end)
{
  int64_t nanoseconds =
    (int64_t)(end->tv_sec - start->tv_sec) * 1000000000ll +
    (int64_t)end->tv_nsec - start->tv_nsec;

  if (nanoseconds < 0)
    {
      nanoseconds = 0;
    }

  return (uint32_t)((nanoseconds + 500000ll) / 1000000ll);
}

static int register_operators(AivlogResolver *resolver)
{
  if (resolver->AddQuantize() != kTfLiteOk ||
      resolver->AddConv2D(tflite::Register_CONV_2D_INT8()) != kTfLiteOk ||
      resolver->AddDepthwiseConv2D(
        tflite::Register_DEPTHWISE_CONV_2D_INT8()) != kTfLiteOk ||
      resolver->AddPad() != kTfLiteOk ||
      resolver->AddMean(tflite::Register_MEAN_INT8()) != kTfLiteOk ||
      resolver->AddFullyConnected(
        tflite::Register_FULLY_CONNECTED_INT8()) != kTfLiteOk ||
      resolver->AddSoftmax(tflite::Register_SOFTMAX_INT8()) != kTfLiteOk ||
      resolver->AddDequantize(tflite::Register_DEQUANTIZE_INT8()) !=
        kTfLiteOk)
    {
      errno = EINVAL;
      return ERROR;
    }

  return OK;
}

static void resize_rgb565_to_uint8(FAR const uint16_t *source,
                                   int source_width, int source_height,
                                   FAR uint8_t *destination)
{
  const int target_width = 224;
  const int target_height = 224;

  for (int y = 0; y < target_height; y++)
    {
      int source_y = ((2 * y + 1) * source_height) /
                     (2 * target_height);
      FAR const uint16_t *row = source + source_y * source_width;

      for (int x = 0; x < target_width; x++)
        {
          int source_x = ((2 * x + 1) * source_width) /
                         (2 * target_width);
          uint16_t pixel = row[source_x];
          uint8_t red = (uint8_t)((pixel >> 11) & 0x1f);
          uint8_t green = (uint8_t)((pixel >> 5) & 0x3f);
          uint8_t blue = (uint8_t)(pixel & 0x1f);

          *destination++ = (uint8_t)((red << 3) | (red >> 2));
          *destination++ = (uint8_t)((green << 2) | (green >> 4));
          *destination++ = (uint8_t)((blue << 3) | (blue >> 2));
        }
    }
}

}  // namespace

extern "C" int camera_gallery_aivlog_model_initialize(void)
{
  size_t model_size;

  if (g_initialized)
    {
      return OK;
    }

  model_size = (size_t)(g_camera_gallery_aivlog_model_end -
                        g_camera_gallery_aivlog_model_start);
  if (model_size < 16)
    {
      printf("[aivlog] embedded model is empty\n");
      errno = EINVAL;
      return ERROR;
    }

  g_model = tflite::GetModel(g_camera_gallery_aivlog_model_start);
  if (g_model == nullptr || g_model->version() != TFLITE_SCHEMA_VERSION)
    {
      printf("[aivlog] model schema=%ld expected=%d\n",
             g_model == nullptr ? -1l : (long)g_model->version(),
             TFLITE_SCHEMA_VERSION);
      errno = EPROTO;
      return ERROR;
    }

  g_resolver = new AivlogResolver();
  if (g_resolver == nullptr)
    {
      errno = ENOMEM;
      return ERROR;
    }

  if (register_operators(g_resolver) < 0)
    {
      delete g_resolver;
      g_resolver = nullptr;
      printf("[aivlog] operator registration failed\n");
      return ERROR;
    }

  g_arena = static_cast<uint8_t *>(
    memalign(16, CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_ARENA_SIZE));
  if (g_arena == nullptr)
    {
      delete g_resolver;
      g_resolver = nullptr;
      errno = ENOMEM;
      printf("[aivlog] arena allocation failed: %u bytes\n",
             (unsigned)CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_ARENA_SIZE);
      return ERROR;
    }

  g_interpreter = new tflite::MicroInterpreter(
    g_model, *g_resolver, g_arena,
    CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_ARENA_SIZE);
  if (g_interpreter == nullptr)
    {
      camera_gallery_aivlog_model_finalize();
      errno = ENOMEM;
      return ERROR;
    }

  if (g_interpreter->AllocateTensors() != kTfLiteOk)
    {
      printf("[aivlog] AllocateTensors failed (arena=%u)\n",
             (unsigned)CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_ARENA_SIZE);
      camera_gallery_aivlog_model_finalize();
      errno = ENOMEM;
      return ERROR;
    }

  g_input = g_interpreter->input(0);
  g_output = g_interpreter->output(0);
  if (g_interpreter->inputs_size() != 1 ||
      g_interpreter->outputs_size() != 1 ||
      g_input == nullptr || g_input->type != kTfLiteUInt8 ||
      !tensor_shape_is(g_input, 1, 224, 224, 3) ||
      g_input->params.zero_point != 127 ||
      (g_input->params.scale > (1.0f / 127.5f) ?
       g_input->params.scale - (1.0f / 127.5f) :
       (1.0f / 127.5f) - g_input->params.scale) > 0.000001f ||
      g_output == nullptr || g_output->type != kTfLiteFloat32 ||
      g_output->dims == nullptr || g_output->dims->size != 2 ||
      g_output->dims->data[0] != 1 || g_output->dims->data[1] != 1)
    {
      printf("[aivlog] unexpected model input/output tensors\n");
      camera_gallery_aivlog_model_finalize();
      errno = EINVAL;
      return ERROR;
    }

  g_initialized = true;
  printf("[aivlog] model bytes=%u arena=%u used=%u input=UINT8[1,224,224,3] output=FLOAT32[1,1]\n",
         (unsigned)model_size,
         (unsigned)CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_ARENA_SIZE,
         (unsigned)g_interpreter->arena_used_bytes());
  return OK;
}

extern "C" int camera_gallery_aivlog_model_score(
  FAR const uint16_t *rgb565, int width, int height,
  FAR struct camera_gallery_aivlog_score_s *result)
{
  struct timespec start;
  struct timespec end;
  float raw_score;
  long score;

  if (!g_initialized || rgb565 == nullptr || result == nullptr ||
      width <= 0 || height <= 0)
    {
      errno = EINVAL;
      return ERROR;
    }

  resize_rgb565_to_uint8(rgb565, width, height, g_input->data.uint8);
  clock_gettime(CLOCK_MONOTONIC, &start);
  if (g_interpreter->Invoke() != kTfLiteOk)
    {
      printf("[aivlog] Invoke failed\n");
      errno = EIO;
      return ERROR;
    }

  clock_gettime(CLOCK_MONOTONIC, &end);
  raw_score = g_output->data.f[0];
  if (raw_score != raw_score || raw_score > FLT_MAX ||
      raw_score < -FLT_MAX)
    {
      printf("[aivlog] non-finite model output\n");
      errno = ERANGE;
      return ERROR;
    }

  {
    float scaled_score = raw_score * 100.0f / kRawScoreMaximum;
    score = scaled_score >= 0.0f ? (long)(scaled_score + 0.5f) :
                                  (long)(scaled_score - 0.5f);
  }
  if (score < 0)
    {
      score = 0;
    }
  else if (score > 100)
    {
      score = 100;
    }

  result->raw_score = raw_score;
  result->score = (int)score;
  result->latency_ms = elapsed_ms(&start, &end);
  return OK;
}

extern "C" void camera_gallery_aivlog_model_finalize(void)
{
  g_initialized = false;
  g_input = nullptr;
  g_output = nullptr;
  g_model = nullptr;

  if (g_interpreter != nullptr)
    {
      delete g_interpreter;
      g_interpreter = nullptr;
    }

  free(g_arena);
  g_arena = nullptr;

  if (g_resolver != nullptr)
    {
      delete g_resolver;
      g_resolver = nullptr;
    }
}
