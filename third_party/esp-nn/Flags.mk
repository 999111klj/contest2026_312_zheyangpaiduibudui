############################################################################
# apps/mlearning/esp-nn/Flags.mk
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
# ESP-NN aliases its public API to architecture specific symbol names inside
# esp_nn.h, for example esp_nn_conv_s8 becomes esp_nn_conv_s8_esp32s3.  Every
# translation unit that includes esp_nn.h must therefore agree on these
# definitions, otherwise the ESP-NN library and its callers resolve different
# symbols and the link fails.  Keep them in this single place and include this
# file from both the ESP-NN library and the TFLite Micro kernel wrappers.

ESP_NN_DEFINES := -DCONFIG_NN_OPTIMIZED
ESP_NN_DEFINES += -DCONFIG_IDF_TARGET_ARCH_XTENSA=1
ESP_NN_DEFINES += -DCONFIG_IDF_TARGET_ESP32S3=1
