EXTRA_DIST = pocketsphinx.c \
	pocketsphinx.pyx

if BUILD_PYTHON
all-local: pymod-build-stamp

install-exec-local: pymod-build-stamp
	$(PYTHON) setup.py install --prefix $(DESTDIR)$(prefix)

uninstall-local:
	$(PYTHON) setup.py bogus_uninstall --prefix $(DESTDIR)$(prefix)

clean-local:
	$(PYTHON) setup.py clean --all
# This is dumb, but distutils is completely incapable of VPATH building
	test -z "$(VPATH)" || $(RM) pocketsphinx.c
	$(RM) pymod-build-stamp

pymod-build-stamp: pocketsphinx.c
# This is dumb, but distutils is completely incapable of VPATH building
	test -z "$(VPATH)" || cp "$(srcdir)/pocketsphinx.c" pocketsphinx.c
	$(PYTHON) setup.py build
	touch $@
endif

if BUILD_PYREX
$(srcdir)/pocketsphinx.c: pocketsphinx.pyx
	pyrexc -o $@ $< $(CPPFLAGS) -I$(sphinxbase)/python
endif