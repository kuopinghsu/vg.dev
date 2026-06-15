# vg.dev – Top-level Makefile
#
# Targets:
#   make cmodel-gen      – generate CTS answer files (RI + cmodel)
#   make cmodel-compare  – compare all RI vs cmodel answers
#   make cmodel-report   – generate summary report
#   make cmodel-all      – full pipeline: gen + compare + report
#   make cts             – alias for cmodel-all
#   make clean           – remove all build artefacts
#   make help            – show all targets

.PHONY: cmodel-gen cmodel-compare cmodel-report cmodel-all cts clean help

cmodel-gen:
	$(MAKE) -C sw/cmodel gen

cmodel-compare:
	$(MAKE) -C sw/cmodel compare

cmodel-report:
	$(MAKE) -C sw/cmodel report

cmodel-all cts:
	$(MAKE) -C sw/cmodel cts

clean:
	$(MAKE) -C sw/cmodel clean

help:
	@echo "Targets:"
	@echo "  make cmodel-gen      Generate CTS answer files (RI + cmodel)"
	@echo "  make cmodel-compare  Compare all RI vs cmodel answers (no exit on mismatch)"
	@echo "  make cmodel-report   Generate summary report"
	@echo "  make cmodel-all      Full pipeline: gen + compare + report"
	@echo "  make cts             Alias for cmodel-all"
	@echo "  make clean           Remove all build artefacts"
	@echo "  make help            Show this help"
