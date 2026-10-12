# EDuke32 -> SF2000 / GB300 core, built through sf2000_multicore.
#
# Produces eduke32_libretro.a in this directory, which sf2000_multicore's
# Makefile copies to $(BUILD_DIR)/libretro_core.a and links into
# core_87000000 -> system/Deimos/cores/eduke32.sf2k.
#
# Called from the multicore side as:
#   make -C sf2000_multicore CORE=cores/eduke32 CONSOLE=eduke32 FROGGY_TYPE=SF2000
# which expands to `make -C cores/eduke32 platform=sf2000`.
#
# The archive itself is produced by the port's own top-level Makefile, which
# knows the bare-metal newlib toolchain layout; this file only drives it and
# stages the result where multicore expects to find it.

TARGET_NAME := eduke32
EDUKE_SRC ?= /home/Sajnaps/eduke32-sf2000
EDUKE_TOP := $(EDUKE_SRC)/Makefile.sf2000

TARGET := $(TARGET_NAME)_libretro.a
STATIC_LINKING = 1

ifeq ($(FROGGY_TYPE),GB300V2)
VARIANT := gb300
else
VARIANT := sf2000
endif

all:
	$(MAKE) -C "$(EDUKE_SRC)" -f $(EDUKE_TOP) $(VARIANT)
	cp -f "$(EDUKE_SRC)/build/$(VARIANT)/eduke32_libretro.a" ./$(TARGET)

clean:
	rm -f *.a

.PHONY: all clean