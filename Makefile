RK_SDK ?= /home/x/Desktop/rk3566/LinuxSDK
SYSROOT := $(RK_SDK)/buildroot/output/rockchip_rk3566/host/aarch64-buildroot-linux-gnu/sysroot
HOST_BIN := $(RK_SDK)/buildroot/output/rockchip_rk3566/host/bin

CROSS_COMPILE ?= $(HOST_BIN)/aarch64-buildroot-linux-gnu-
CC := $(CROSS_COMPILE)gcc

TARGET := onvif_yolo_lcd
OBJDIR := obj
BINDIR := bin

CFLAGS := -Wall -Wextra -O2 -std=gnu11 -D_GNU_SOURCE -MMD -MP --sysroot=$(SYSROOT)
CFLAGS += -I. -Ionvif -Ipipeline -Imedia -Iinfer -Idisplay
CFLAGS += -I$(SYSROOT)/usr/include
CFLAGS += -I$(SYSROOT)/usr/include/rockchip
CFLAGS += -I$(SYSROOT)/usr/include/rga
CFLAGS += -I$(SYSROOT)/usr/include/rknn

LDFLAGS := --sysroot=$(SYSROOT) -L$(SYSROOT)/usr/lib
LDLIBS := -lavformat -lavcodec -lavutil -lrga -lrknnrt -lpthread -lm

SRCS := \
	main.c \
	onvif/onvif_auth.c \
	onvif/onvif_client.c \
	onvif/onvif_discovery.c \
	onvif/onvif_http.c \
	onvif/onvif_xml.c \
	pipeline/pipeline_state.c \
	media/capture_decode.c \
	media/rga_preprocess.c \
	infer/yolo_infer.c \
	infer/yolo_postprocess_c.c \
	display/display_overlay.c

OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all clean print

all: $(BINDIR)/$(TARGET)

$(BINDIR)/$(TARGET): $(OBJS) | $(BINDIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

$(OBJDIR)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BINDIR):
	mkdir -p $@

print:
	@echo "TARGET=$(TARGET)"
	@echo "SYSROOT=$(SYSROOT)"
	@echo "CC=$(CC)"
	@echo "SRCS=$(SRCS)"

clean:
	rm -rf $(OBJDIR) $(BINDIR)

-include $(DEPS)
