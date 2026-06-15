# sw/cmodel/Makefile – build the C-model library and run CTS compliance tests.
#
# Stage targets (run independently, resume from any stage):
#   make gen         – build generators + run RI and cmodel answer generation
#   make compare     – compare RI vs cmodel answer files (all tests, no exit on mismatch)
#   make report      – generate summary report only (no re-compare)
#   make cts         – full pipeline: gen + compare + report
#
# Filtering (combine with any stage target):
#   TEST=A10101      – only generate/compare test A10101
#   GROUP=G          – only tests in group G (e.g. GROUP=G3)
#   PATTERN=gradient – only tests whose path contains "gradient"
#
# Other targets:
#   make              – build cmodel library only
#   make clean        – remove build artefacts
#   make help         – show all targets

ROOT     := $(abspath ../..)
BUILD    ?= $(ROOT)/build

CM_SRCS  := vg_cmodel.c vg_openvg.c
CM_OBJS  := $(patsubst %.c,$(BUILD)/cmodel/%.o,$(CM_SRCS))
CM_LIB   := $(BUILD)/cmodel/libcmodel.a

RI_INC   := $(ROOT)/sw/ri/ri/include
RI_LIB   ?= $(BUILD)/ri/libOpenVG.a
DRIVER   := $(ROOT)/sw/driver

CC       := gcc
CFLAGS   := -O2 -std=c11 -Wall -w \
            -I$(ROOT)/sw/cmodel -I$(DRIVER) -I$(RI_INC) \
            -DOPENVG_STATIC_LIBRARY -DEGL_STATIC_LIBRARY -D_GNU_SOURCE

COMPARE := python3 $(ROOT)/sw/cts/compare_all.py
REPORT   := $(BUILD)/cts/report.txt
RI_ANS   := $(BUILD)/cts/ri/answer
CM_ANS   := $(BUILD)/cts/cmodel/answer
RI_LOG   := $(BUILD)/cts/ri.log
CM_LOG   := $(BUILD)/cts/cmodel.log

.PHONY: all gen compare report cts clean help dirs

# ---------------------------------------------------------------------------
# Default: build the cmodel static library
# ---------------------------------------------------------------------------
all: $(CM_LIB)

$(BUILD)/cmodel/%.o: %.c | $(BUILD)/cmodel
	$(CC) $(CFLAGS) -c $< -o $@

$(CM_LIB): $(CM_OBJS)
	ar rcs $@ $^

$(BUILD)/cmodel $(BUILD)/cts $(BUILD)/cts/ri $(BUILD)/cts/cmodel:
	mkdir -p $@

dirs: $(BUILD)/cts/ri $(BUILD)/cts/cmodel

# ---------------------------------------------------------------------------
# Stage 1: Generate answer files (RI + cmodel)
# ---------------------------------------------------------------------------
$(BUILD)/ri/libOpenVG.a:
	$(MAKE) -C $(ROOT)/sw/ri BUILD=$(BUILD)/ri

$(BUILD)/cts/ri/generator $(BUILD)/cts/cmodel/generator: $(BUILD)/ri/libOpenVG.a
	$(MAKE) -C $(ROOT)/sw/cts BUILD=$(BUILD) RI_LIB=$(RI_LIB)

gen: $(BUILD)/cts/ri/generator $(BUILD)/cts/cmodel/generator
	@echo "=== Running RI generator ==="
	cd $(BUILD)/cts/ri && ./generator 2>&1 | awk '/Config : #|Succeeded|Skipped|does not support|EXIT/{print;fflush()}' | tee $(RI_LOG) &
	@echo "=== Running cmodel generator ==="
	cd $(BUILD)/cts/cmodel && ./generator 2>&1 | awk '/Config : #|Succeeded|Skipped|does not support|overflow|exceed|EXIT/{print;fflush()}' | tee $(CM_LOG) &
	@wait
	@echo "=== Generation complete ==="
	@echo "RI answers:  $$(find $(RI_ANS) -type f 2>/dev/null | wc -l | tr -d ' ') files"
	@echo "CM answers:  $$(find $(CM_ANS) -type f 2>/dev/null | wc -l | tr -d ' ') files"

# ---------------------------------------------------------------------------
# Stage 2: Compare all answer files (never exit on mismatch)
# ---------------------------------------------------------------------------
compare:
	@echo "=== Comparing RI vs cmodel ==="
	@$(COMPARE) $(RI_ANS) $(CM_ANS) || true
	@echo "=== Compare complete ==="

# ---------------------------------------------------------------------------
# Stage 3: Generate summary report (reads existing comparison output)
# ---------------------------------------------------------------------------
report:
	@echo "=== Generating report ==="
	@echo "CTS Test Report"                            > $(REPORT)
	@echo "========================================"  >> $(REPORT)
	@echo "Date: $$(date)"                            >> $(REPORT)
	@echo ""                                         >> $(REPORT)
	@$(COMPARE) $(RI_ANS) $(CM_ANS) >> $(REPORT) 2>&1 || true
	@echo ""                                         >> $(REPORT)
	@echo "--- RI generator log ---"                 >> $(REPORT)
	@cat $(RI_LOG) 2>/dev/null >> $(REPORT) || true
	@echo ""                                         >> $(REPORT)
	@echo "--- cmodel generator log ---"             >> $(REPORT)
	@cat $(CM_LOG) 2>/dev/null >> $(REPORT) || true
	@echo ""                                         >> $(REPORT)
	@echo "Report saved to: $(REPORT)"
	@cat $(REPORT)

# ---------------------------------------------------------------------------
# Full pipeline: generate + compare + report
# ---------------------------------------------------------------------------
cts: gen compare report

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------
clean:
	rm -rf $(BUILD)/cmodel $(BUILD)/cts

# ---------------------------------------------------------------------------
# Help
# ---------------------------------------------------------------------------
help:
	@echo "Targets:"
	@echo "  make              Build cmodel library"
	@echo "  make gen          Generate CTS answer files (RI + cmodel)"
	@echo "  make compare      Compare all RI vs cmodel answers (no exit on mismatch)"
	@echo "  make report       Generate summary report"
	@echo "  make cts          Full pipeline: gen + compare + report"
	@echo "  make clean        Remove build artefacts"
	@echo ""
	@echo "Filtering (combine with gen/compare/report/cts):"
	@echo "  TEST=A10101       Only test A10101"
	@echo "  GROUP=G           Only group G (e.g. GROUP=G3)"
	@echo "  PATTERN=gradient  Only tests matching 'gradient'"
	@echo ""
	@echo "Examples:"
	@echo "  make gen TEST=A10101    Generate only test A10101"
	@echo "  make compare GROUP=G   Compare only group G tests"
	@echo "  make cts               Full pipeline"
