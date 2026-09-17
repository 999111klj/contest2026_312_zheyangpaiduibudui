/****************************************************************************
 * Embedded NIMA INT8 TFLite model.
 ****************************************************************************/

  .section .rodata.camera_gallery_aivlog_model,"a",@progbits
  .balign 16
  .global g_camera_gallery_aivlog_model_start
  .global g_camera_gallery_aivlog_model_end
  .type g_camera_gallery_aivlog_model_start,@object

g_camera_gallery_aivlog_model_start:
  .incbin "nima_exp4_int8.tflite"
g_camera_gallery_aivlog_model_end:
  .size g_camera_gallery_aivlog_model_start, \
        g_camera_gallery_aivlog_model_end - g_camera_gallery_aivlog_model_start

  .section .note.GNU-stack,"",@progbits
