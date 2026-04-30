# sw/examples/Makefile
#
# Builds all RI and cmodel test binaries, runs them to produce PPM images,
# then compares the two outputs pixel-by-pixel with compare.py.
#
# All artifacts (binaries + PPM images) are written to $(BUILD)/<test>/.
# BUILD and RI_LIB may be overridden on the command line or by a parent
# Makefile (e.g. sw/Makefile).
#
# Targets:
#   make all   – build all binaries
#   make run   – build + run + compare
#   make clean – remove $(BUILD)

ROOT    := $(abspath ../..)

# Absolute path to this directory (sw/examples/) – used to reference
# sources and compare.py regardless of where make is invoked from.
SRCDIR  := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Build output root; parent Makefile passes BUILD=<prj>/build/examples
BUILD   ?= $(ROOT)/build/examples

RI_INC  := $(ROOT)/sw/ri/ri/include
# RI_LIB may be passed in; default matches the project build layout.
RI_LIB  ?= $(ROOT)/build/ri/libOpenVG.a
CM_SRC  := $(ROOT)/sw/cmodel/vg_cmodel.c
CM_INC  := -I$(ROOT)/sw/cmodel -I$(ROOT)/driver

RI_CFLAGS := -O2 -std=c99 -Wall \
             -I$(RI_INC) -I$(RI_INC)/VG -I$(RI_INC)/EGL \
             -DOPENVG_STATIC_LIBRARY -DEGL_STATIC_LIBRARY -D_GNU_SOURCE

CM_CFLAGS := -O2 -std=c11 -Wall $(CM_INC) -D_GNU_SOURCE

TESTS_GENERIC := 01_fill_solid 02_fill_curve 03_gradient_linear \
                 04_gradient_radial 05_even_odd 06_blend 08_text 09_lines \
                 10_mask_tex_xform
TESTS         := $(TESTS_GENERIC) 07_tiger

# ---------------------------------------------------------------------------
# Per-test build rules (binaries land in $(BUILD)/$(test)/)
# ---------------------------------------------------------------------------
define RI_RULE
$(BUILD)/$(1)/test_ri: $(SRCDIR)$(1)/test_ri.c $(SRCDIR)ri_helper.h $(RI_LIB)
	@mkdir -p $(BUILD)/$(1)
	gcc $(RI_CFLAGS) -o $$@ $$< $(RI_LIB) -lm -lstdc++
endef

define CM_RULE
$(BUILD)/$(1)/test_cmodel: $(SRCDIR)$(1)/test_cmodel.c $(SRCDIR)cmodel_helper.h $(CM_SRC)
	@mkdir -p $(BUILD)/$(1)
	gcc $(CM_CFLAGS) -o $$@ $(CM_SRC) $$< -lm
endef

$(foreach t,$(TESTS_GENERIC),$(eval $(call RI_RULE,$(t))))
$(foreach t,$(TESTS_GENERIC),$(eval $(call CM_RULE,$(t))))

# ---------------------------------------------------------------------------
# Top-level targets
# ---------------------------------------------------------------------------
.PHONY: all run clean

all: $(TESTS:%=$(BUILD)/%/test_ri) $(TESTS:%=$(BUILD)/%/test_cmodel)

run: all
	@echo ""
	@echo "================================================================"
	@echo "  OpenVG Examples: RI vs CModel Pixel Comparison"
	@echo "================================================================"
	@PASS=0; FAIL=0; \
	for t in $(TESTS); do \
	    echo ""; echo "--- $$t ---"; \
	    (cd $(BUILD)/$$t && $(BUILD)/$$t/test_ri     ri.ppm     2>&1); \
	    (cd $(BUILD)/$$t && $(BUILD)/$$t/test_cmodel cmodel.ppm 2>&1); \
	    if (cd $(BUILD)/$$t && python3 $(SRCDIR)compare.py ri.ppm cmodel.ppm "$$t"); then \
	        PASS=$$((PASS + 1)); \
	    else \
	        FAIL=$$((FAIL + 1)); \
	    fi; \
	done; \
	echo ""; \
	echo "================================================================"; \
	echo "  Results: $$PASS passed, $$FAIL failed"; \
	echo "================================================================"; \
	test $$FAIL -eq 0

clean:
	rm -rf $(BUILD)

# ---------------------------------------------------------------------------
# 07_tiger: override generated rules to also compile tiger.c
# ---------------------------------------------------------------------------
TIGER_SRC := $(SRCDIR)07_tiger/tiger.c

$(BUILD)/07_tiger/test_ri: $(SRCDIR)07_tiger/test_ri.c $(SRCDIR)ri_helper.h \
                            $(TIGER_SRC) $(RI_LIB)
	@mkdir -p $(BUILD)/07_tiger
	gcc $(RI_CFLAGS) -o $@ $< $(TIGER_SRC) $(RI_LIB) -lm -lstdc++

$(BUILD)/07_tiger/test_cmodel: $(SRCDIR)07_tiger/test_cmodel.c \
                                $(SRCDIR)cmodel_helper.h $(TIGER_SRC) $(CM_SRC)
	@mkdir -p $(BUILD)/07_tiger
	gcc $(CM_CFLAGS) -o $@ $(CM_SRC) $(TIGER_SRC) $< -lm
