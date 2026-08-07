#---------------------------------------------------------------------------------
# Torfoil — client BitTorrent pour Nintendo Switch
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO n'est pas défini. Exemple : export DEVKITPRO=/opt/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# Métadonnées du .nro (affichées dans le menu homebrew)
#---------------------------------------------------------------------------------
APP_TITLE   := Torfoil
APP_AUTHOR  := tensorcrush
APP_VERSION := 0.1.0

TARGET   := torfoil
BUILD    := build
SOURCES  := source source/bt source/net source/net/wg source/util source/install source/ui \
            source/vpn source/diag \
            third_party/lwip/src/core third_party/lwip/src/core/ipv4
DATA     := data
INCLUDES := include third_party/lwip/src/include

# Pas de ROMFS, et c'est délibéré : l'application n'embarque aucune ressource
# (la police vient de pl:u, la page web est dans le binaire). Le laisser pointer
# vers un dossier absent faisait échouer build_romfs après une compilation et
# une édition de liens pourtant réussies — « Failed to open .../romfs! » tout à
# la fin, quand tout le reste avait marché.
ROMFS    :=

#---------------------------------------------------------------------------------
# Options de compilation
#---------------------------------------------------------------------------------
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# -Wno-missing-field-initializers : les en-têtes libnx en génèrent des centaines,
# le vrai signal se noierait dedans.
CFLAGS := -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
          -O2 -ffunction-sections -fdata-sections $(ARCH) $(DEFINES)

CFLAGS += $(INCLUDE) -D__SWITCH__

# RTTI désactivé (on n'en a pas besoin, ça allège le binaire) mais exceptions
# conservées : la STL s'en sert pour signaler les échecs d'allocation, et sur une
# console à mémoire contrainte on préfère une erreur propre à un terminate().
CXXFLAGS := $(CFLAGS) -fno-rtti -std=gnu++17

ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# freetype et harfbuzz se référencent mutuellement : --start-group laisse
# l'éditeur de liens boucler dessus au lieu d'exiger un ordre impossible.
LIBS := -lSDL2_ttf -Wl,--start-group -lfreetype -lharfbuzz -Wl,--end-group \
        -lpng16 -lbz2 \
        -lSDL2 -lEGL -lglapi -ldrm_nouveau \
        -lmbedtls -lmbedx509 -lmbedcrypto \
        -lzstd -lz -lm -lnx

LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# Plomberie du template devkitPro — ne rien toucher en dessous
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ROMFS)),)
    export NROFLAGS :=
else
    export NROFLAGS := --romfsdir=$(CURDIR)/$(ROMFS)
endif

ifeq ($(strip $(APP_TITLE)),)
    export APP_TITLE := $(TARGET)
endif

export NACPFLAGS := --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $(CURDIR)/$(TARGET).nacp
export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp

ifneq ($(wildcard $(TOPDIR)/icon.jpg),)
    export APP_ICON := $(TOPDIR)/icon.jpg
    export NROFLAGS += --icon=$(APP_ICON)
endif

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo nettoyage...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)

$(OFILES_SRC): $(HFILES_BIN)

%.bin.o %_bin.h: %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
