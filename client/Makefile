#---------------------------------------------------------------------------------
# DiizerU Wii U client — builds a .wuhb (devkitPro + wut).
# Requires: $DEVKITPRO with wut, and the wiiu SDL2 portlibs:
#   sudo dkp-pacman -S --needed wiiu-sdl2 wiiu-sdl2_ttf wiiu-sdl2_image wiiu-sdl2_mixer
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO is not set. Source the devkitPro environment.")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/wut/share/wut_rules

#---------------------------------------------------------------------------------
TARGET   := DiizerU
BUILD    := build
SOURCES  := src src/platform src/core src/audio src/ui third_party/cjson
DATA     := data
INCLUDES := src include third_party

# .wuhb metadata + bundled content (romfs at /vol/content)
APP_CONTENT   := $(TOPDIR)/content
APP_NAME      := DiizerU
APP_SHORTNAME := DiizerU
APP_AUTHOR    := cycl0o0
#---------------------------------------------------------------------------------

ARCH := -mcpu=750 -meabi -mhard-float

PKGCONF := $(DEVKITPRO)/portlibs/wiiu/bin/powerpc-eabi-pkg-config
# --static pulls private deps (freetype, harfbuzz, png, ogg, ...) needed when
# linking the portlib .a archives.
SDL_CFLAGS := $(shell $(PKGCONF) --cflags          sdl2 SDL2_ttf SDL2_image SDL2_mixer harfbuzz 2>/dev/null)
SDL_LIBS   := $(shell $(PKGCONF) --libs --static   sdl2 SDL2_ttf SDL2_image SDL2_mixer harfbuzz 2>/dev/null)

CFLAGS   := -g -Wall -O2 -ffunction-sections $(ARCH) \
            $(SDL_CFLAGS) -D__WIIU__ -D__WUT__
CFLAGS   += $(INCLUDE)
CXXFLAGS := $(CFLAGS) -std=gnu++17
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -g $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)

# libcurl is already in wiiu portlibs (used later for the relay API client).
LIBS := $(SDL_LIBS) -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lwut -lm

LIBDIRS := $(PORTLIBS) $(WUT_ROOT)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                  $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
# Embedded binary assets (font, CA bundle) -> linked into the RPX. The .wuhb
# /vol/content mount is not reachable via fopen on Wii U, so we bake assets in.
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))
export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o) $(OFILES_BIN)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).wuhb $(TARGET).rpx $(TARGET).elf

#---------------------------------------------------------------------------------
else
DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).wuhb

# Rebuild the .wuhb when bundled content (font, cacert) changes, not just code.
$(OUTPUT).wuhb: $(OUTPUT).rpx $(wildcard $(TOPDIR)/content/*)
$(OUTPUT).rpx: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

$(OFILES_SRC) $(CPPFILES:.cpp=.o) $(CFILES:.c=.o): $(HFILES_BIN)

# Embed binary assets (one rule per extension used in data/).
%.ttf.o %_ttf.h : %.ttf
	@echo $(notdir $<)
	@$(bin2o)

%.pem.o %_pem.h : %.pem
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)
endif
