############################################################################
# apps/examples/camera_gallery/Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_EXAMPLES_CAMERA_GALLERY_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_CAMERA_GALLERY_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_CAMERA_GALLERY_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_CAMERA_GALLERY)
CXXEXT    = .cc

CSRCS   = camera_gallery_bmp.c camera_gallery_level.c camera_gallery_nx.c \
          camera_gallery_zh_font.c
MAINSRC = camera_gallery_main.c

ifneq ($(CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG),)
CSRCS += camera_gallery_aivlog.c
CXXSRCS += camera_gallery_aivlog_model.cc
ASRCS += camera_gallery_aivlog_model_data.S
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/mlearning/tflite-micro/tflite-micro
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/math/gemmlowp/gemmlowp
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/math/kissfft/kissfft
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/math/ruy/ruy
CXXFLAGS += -DTFLITE_EMULATE_FLOAT -DTF_LITE_DISABLE_X86_NEON
CXXFLAGS += -DTF_LITE_STRIP_ERROR_STRINGS
endif

ifneq ($(CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_CLOUD),)
CSRCS += camera_gallery_aivlog_cloud.c
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../packages/ai_agent/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../packages/ai_agent/src
endif

ifneq ($(CONFIG_EXAMPLES_CAMERA_GALLERY_AIVLOG_PROVISION),)
CSRCS += camera_gallery_aivlog_provision.c
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../packages/ai_agent/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../packages/ai_agent/src
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/cjson/cJSON
endif

ifneq ($(CONFIG_EXAMPLES_CAMERA_GALLERY_CLOUD_SCORE),)
CSRCS += camera_gallery_cloud.c camera_gallery_upload.c
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../packages/ai_agent/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../packages/ai_agent/src
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/cjson/cJSON
endif

include $(APPDIR)/Application.mk
