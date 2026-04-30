# sw/cts/Makefile – build the CTS answer-file generators for both RI and cmodel.
#
# Usage (invoked by sw/Makefile):
#   make -C cts BUILD=<root>/build RI_LIB=<root>/build/ri/libOpenVG.a
#
# Produces:
#   $(BUILD)/cts/ri/generator      – runs CTS against the RI OpenVG library
#   $(BUILD)/cts/cmodel/generator  – runs CTS against the cmodel wrapper
#
# After building, run each generator to produce answer files:
#   $(BUILD)/cts/ri/generator
#   $(BUILD)/cts/cmodel/generator
# Answer files land in $(BUILD)/cts/ri/answer/ and $(BUILD)/cts/cmodel/answer/

# ---- Paths -----------------------------------------------------------------

ROOT     := $(abspath ../..)
BUILD    ?= $(ROOT)/build
RI_LIB   ?= $(ROOT)/build/ri/libOpenVG.a

CTS_SRC  := $(CURDIR)/generation/source
NATIVE   := $(CTS_SRC)/adopter/native/native_null.c

RI_INC   := $(ROOT)/sw/ri/ri/include
CM_DIR   := $(ROOT)/sw/cmodel
CM_INC   := $(CM_DIR) $(ROOT)/sw/driver $(RI_INC)

RI_BUILD  := $(BUILD)/cts/ri
CM_BUILD  := $(BUILD)/cts/cmodel

# ---- Source lists ----------------------------------------------------------

# All .c files in generation/source root + util/ + functions/ (glob-safe make)
CTS_CORE  := $(wildcard $(CTS_SRC)/*.c) \
             $(wildcard $(CTS_SRC)/util/*.c) \
             $(wildcard $(CTS_SRC)/functions/*.c)

RI_SRCS  := $(NATIVE) $(CTS_CORE)
CM_SRCS  := $(NATIVE) $(CTS_CORE) \
             $(CM_DIR)/vg_cmodel.c \
             $(CM_DIR)/vg_openvg.c

# ---- Compiler flags --------------------------------------------------------

CC       := gcc
CFLAGS_COMMON := \
    -DTEST_OPTION_VGU=1 \
    -DEGL_STATIC_LIBRARY \
    -I$(CTS_SRC) \
    -I$(CTS_SRC)/adopter/native \
    -I$(CTS_SRC)/adopter/platform \
    -include sys/stat.h \
    -include sys/types.h \
    -w

RI_CFLAGS := $(CFLAGS_COMMON) \
    -DANSWER_DEFAULT_DIR=\"$(RI_BUILD)/answer\" \
    -DINFO_DEFAULT_DIR=\"$(RI_BUILD)/info\" \
    -I$(RI_INC)

CM_CFLAGS := $(CFLAGS_COMMON) \
    -DANSWER_DEFAULT_DIR=\"$(CM_BUILD)/answer\" \
    -DINFO_DEFAULT_DIR=\"$(CM_BUILD)/info\" \
    $(addprefix -I,$(CM_INC))

# ---- Targets ---------------------------------------------------------------

.PHONY: all ri cmodel clean

all: ri cmodel

ri: $(RI_BUILD)/generator

cmodel: $(CM_BUILD)/generator

$(RI_BUILD)/generator: $(RI_SRCS) $(RI_LIB)
	@mkdir -p $(RI_BUILD)/answer $(RI_BUILD)/info
	$(CC) $(RI_CFLAGS) $(RI_SRCS) $(RI_LIB) -lstdc++ -lm -o $@

$(CM_BUILD)/generator: $(CM_SRCS)
	@mkdir -p $(CM_BUILD)/answer $(CM_BUILD)/info
	$(CC) $(CM_CFLAGS) $(CM_SRCS) -lm -o $@

# ---- Run targets (generate answer files) -----------------------------------

.PHONY: run run-ri run-cmodel

run: run-ri run-cmodel

run-ri: $(RI_BUILD)/generator
	cd $(RI_BUILD) && ./generator

run-cmodel: $(CM_BUILD)/generator
	cd $(CM_BUILD) && ./generator

# ---- Compare results -------------------------------------------------------

.PHONY: compare

compare: run
	@echo "Comparing RI vs cmodel answer files..."
	@python3 $(ROOT)/sw/examples/compare.py $(RI_BUILD)/answer $(CM_BUILD)/answer 2>/dev/null || \
	 python3 $(ROOT)/sw/cts/compare_cts.py $(RI_BUILD)/answer $(CM_BUILD)/answer

# ---- Clean -----------------------------------------------------------------

clean:
	rm -rf $(BUILD)/cts
