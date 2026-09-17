############################################################################
# apps/mlearning/esp-nn/Makefile
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################

include $(APPDIR)/Make.defs
include $(APPDIR)/mlearning/esp-nn/Flags.mk

# ESP-NN upstream: https://github.com/espressif/esp-nn
# Vendored at commit 2c222c5e02225177b44ebf21169bc66df3c8b573

ESP_NN_DIR = esp-nn

# Portable reference kernels, always built.

CSRCS += $(ESP_NN_DIR)/src/activation_functions/esp_nn_relu_ansi.c
CSRCS += $(ESP_NN_DIR)/src/activation_functions/esp_nn_hard_swish_ansi.c
CSRCS += $(ESP_NN_DIR)/src/common/esp_nn_mean_ansi.c
CSRCS += $(ESP_NN_DIR)/src/basic_math/esp_nn_add_ansi.c
CSRCS += $(ESP_NN_DIR)/src/basic_math/esp_nn_mul_ansi.c
CSRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_conv_ansi.c
CSRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_conv_opt.c
CSRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_depthwise_conv_ansi.c
CSRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_depthwise_conv_opt.c
CSRCS += $(ESP_NN_DIR)/src/fully_connected/esp_nn_fully_connected_ansi.c
CSRCS += $(ESP_NN_DIR)/src/softmax/esp_nn_softmax_ansi.c
CSRCS += $(ESP_NN_DIR)/src/softmax/esp_nn_softmax_opt.c
CSRCS += $(ESP_NN_DIR)/src/logistic/esp_nn_logistic_ansi.c
CSRCS += $(ESP_NN_DIR)/src/pooling/esp_nn_avg_pool_ansi.c
CSRCS += $(ESP_NN_DIR)/src/pooling/esp_nn_max_pool_ansi.c

# ESP32-S3 vector unit kernels.  The file list mirrors the s3_srcs set of the
# upstream CMakeLists.txt.

CSRCS += $(ESP_NN_DIR)/src/activation_functions/esp_nn_hard_swish_s8_esp32s3.c
CSRCS += $(ESP_NN_DIR)/src/common/esp_nn_mean_s8_esp32s3.c
CSRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_conv_esp32s3.c
CSRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_conv_s8_1x1_esp32s3.c
CSRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_conv_s8_3x3_opt_esp32s3.c
CSRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_depthwise_conv_s8_esp32s3.c
CSRCS += $(ESP_NN_DIR)/src/fully_connected/esp_nn_fully_connected_esp32s3.c
CSRCS += $(ESP_NN_DIR)/src/pooling/esp_nn_avg_pool_s8_esp32s3.c
CSRCS += $(ESP_NN_DIR)/src/softmax/esp_nn_softmax_s8_esp32s3.c

ASRCS += $(ESP_NN_DIR)/src/common/esp_nn_common_functions_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/common/esp_nn_dot_s8_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/common/esp_nn_multiply_by_quantized_mult_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/common/esp_nn_multiply_by_quantized_mult_ver1_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/activation_functions/esp_nn_relu_s8_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/basic_math/esp_nn_add_s8_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/basic_math/esp_nn_mul_s8_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/basic_math/esp_nn_mul_broadcast_s8_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_conv_s16_mult8_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_conv_s8_mult8_1x1_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_conv_s16_mult4_1x1_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_conv_s8_filter_aligned_input_padded_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_depthwise_conv_s8_kx1_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_depthwise_conv_s8_mult1_3x3_padded_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_depthwise_conv_s16_mult1_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_depthwise_conv_s16_mult1_3x3_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_depthwise_conv_s16_mult1_3x3_no_pad_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_depthwise_conv_s16_mult8_3x3_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_depthwise_conv_s16_mult4_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/convolution/esp_nn_depthwise_conv_s16_mult8_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/fully_connected/esp_nn_fc_s8_mac16_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/fully_connected/esp_nn_fully_connected_s8_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/fully_connected/esp_nn_fully_connected_per_ch_s8_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/pooling/esp_nn_max_pool_s8_esp32s3.S
ASRCS += $(ESP_NN_DIR)/src/pooling/esp_nn_avg_pool_s8_esp32s3.S

ESP_NN_FLAGS := $(ESP_NN_DEFINES)
ESP_NN_FLAGS += ${INCDIR_PREFIX}$(APPDIR)/mlearning/esp-nn/$(ESP_NN_DIR)/include
ESP_NN_FLAGS += ${INCDIR_PREFIX}$(APPDIR)/mlearning/esp-nn/$(ESP_NN_DIR)/src/common

# -mlongcalls is required by the hand written assembly, -O2 and
# -fno-unroll-loops match the upstream component build.  NuttX compiles the
# whole tree with -Os by default, so the trailing -O2 deliberately overrides it
# for these compute kernels only.

ESP_NN_FLAGS += -mlongcalls -fno-unroll-loops -O2
ESP_NN_FLAGS += -Wno-undef -Wno-unused-function

CFLAGS += $(ESP_NN_FLAGS)
AFLAGS += $(ESP_NN_FLAGS)

include $(APPDIR)/Application.mk
