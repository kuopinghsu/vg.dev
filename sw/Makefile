# sw/Makefile – top-level build for the sw sub-tree.
#
# All artefacts land under <project-root>/build/:
#
#   build/
#     ri/                  – libOpenVG.a, tiger binary, object files
#     examples/
#       <test>/            – test_ri, test_cmodel, ri.ppm, cmodel.ppm, diff.ppm
#
# Targets:
#   make all      – build RI library + tiger sample + all example binaries
#   make run      – build + run examples + pixel-compare RI vs CModel
#   make tiger    – build + run the tiger sample  → build/ri/tiger.ppm
#   make clean    – remove the entire build/ tree

ROOT      := $(abspath ..)
BUILD     := $(ROOT)/build

.PHONY: all run tiger clean ri examples cts

# ---------------------------------------------------------------------------
# Default: build everything
# ---------------------------------------------------------------------------
all: ri examples

# ---------------------------------------------------------------------------
# RI library + tiger sample
# ---------------------------------------------------------------------------
ri:
	$(MAKE) -C ri BUILD=$(BUILD)/ri

# ---------------------------------------------------------------------------
# Example test binaries (depends on RI being built first)
# ---------------------------------------------------------------------------
examples: ri
	$(MAKE) -C examples BUILD=$(BUILD)/examples RI_LIB=$(BUILD)/ri/libOpenVG.a

# ---------------------------------------------------------------------------
# Run all examples and compare RI vs CModel pixel outputs
# ---------------------------------------------------------------------------
run: ri
	$(MAKE) -C examples BUILD=$(BUILD)/examples RI_LIB=$(BUILD)/ri/libOpenVG.a run

# ---------------------------------------------------------------------------
# Render the tiger sample → build/ri/tiger.ppm
# ---------------------------------------------------------------------------
tiger: ri
	$(BUILD)/ri/tiger $(BUILD)/ri/tiger.ppm 512

# ---------------------------------------------------------------------------
# Remove all generated artefacts
# ---------------------------------------------------------------------------
clean:
	rm -rf $(BUILD)

# ---------------------------------------------------------------------------
# CTS compliance test generators (RI + cmodel)
# ---------------------------------------------------------------------------
cts: ri
	$(MAKE) -C cts BUILD=$(BUILD) RI_LIB=$(BUILD)/ri/libOpenVG.a
