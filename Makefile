# Root delegating Makefile.
#
# Targets are addressed by their full path: `make examples/<name>/<target>`.
# The recipe forwards to that example's own Makefile via $(MAKE) -C.
# Adding a new example needs no change here.

examples/%:
	$(MAKE) -C $(@D) $(@F)
