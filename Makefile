
CUDA_VER ?= 12.2
ifeq ($(CUDA_VER),)
    $(error "CUDA_VER is not set")
endif

APP := av_sync

TARGET_DEVICE = $(shell gcc -dumpmachine | cut -f1 -d -)
NVDS_VERSION := 7.0

LIB_INSTALL_DIR ?= /opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/lib/
APP_INSTALL_DIR ?= /opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/bin/

ifeq ($(TARGET_DEVICE), aarch64)
    CFLAGS += -DPLATFORM_TEGRA
endif

# ─── Redis++ and Hiredis ──────────────────────────────────────────────────────
# You may need to adjust these paths based on where you've cloned your libs.
HIREDIS_ROOT := ./libs/hiredis
REDISPP_ROOT := ./libs/redis-plus-plus

SRCS := $(wildcard *.cpp)
INCS := $(wildcard *.h)
OBJS := $(SRCS:.cpp=.o)

PKGS := gstreamer-1.0 gstreamer-app-1.0

CFLAGS += -I/opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/sources/includes \
          -I/usr/local/cuda-$(CUDA_VER)/include \
          -I$(HIREDIS_ROOT) \
          -I$(HIREDIS_ROOT)/include \
          -I$(REDISPP_ROOT)/include \
          -I$(REDISPP_ROOT)/include/sw \
          -I$(REDISPP_ROOT)/src \
          $(shell pkg-config --cflags $(PKGS))

LIBS := $(shell pkg-config --libs $(PKGS))
LIBS += -lgstapp-1.0 \
        -L/usr/local/cuda-$(CUDA_VER)/lib64/ -lcudart \
        -L$(LIB_INSTALL_DIR) \
        -lnvdsgst_meta -lnvds_meta -lnvds_yml_parser -lcuda \
        -Wl,-rpath,$(LIB_INSTALL_DIR) \
        -L$(HIREDIS_ROOT) -lhiredis \
        -L$(REDISPP_ROOT)/lib/libredis++.a


all: $(APP)

%.o: %.cpp $(INCS) Makefile
	$(CXX) -c -o $@ $(CFLAGS) $<

$(APP): $(OBJS)
	$(CXX) -o $@ $^ $(LIBS)

install: $(APP)
	cp -rv $(APP) $(APP_INSTALL_DIR)

clean:
	rm -rf $(OBJS) $(APP)
