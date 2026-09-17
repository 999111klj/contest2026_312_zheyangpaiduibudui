/****************************************************************************
 * apps/mlearning/esp-nn/compat/esp_timer.h
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* The TFLite Micro ESP-NN kernel wrappers are written against ESP-IDF and use
 * esp_timer_get_time() for their per-operator profiling counters.  NuttX has
 * no esp_timer component, so provide the same monotonic microsecond source on
 * top of POSIX clock_gettime().  This header is only reachable through the
 * include path added by apps/mlearning/esp-nn/Make.defs.
 */

#ifndef __APPS_MLEARNING_ESP_NN_COMPAT_ESP_TIMER_H
#define __APPS_MLEARNING_ESP_NN_COMPAT_ESP_TIMER_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C"
{
#endif

static inline int64_t esp_timer_get_time(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
      return 0;
    }

  return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

#ifdef __cplusplus
}
#endif

#endif /* __APPS_MLEARNING_ESP_NN_COMPAT_ESP_TIMER_H */
