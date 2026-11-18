# Portable core in src/, the Linux backend in linux/, measuring tools in tests/.
# Binaries land in the root because they are what you type — ./v7dvs, ./tcdec.
# VERSION is the one place the number lives; the tag confirms it. Never edit it
# by hand — `/bump` writes it, the CHANGELOG and the tag in the same breath.
CFLAGS = -std=gnu17 -O2 -Wall -Isrc -DV7DVS_VERSION=\"$(shell cat VERSION)\"
XWAX = ../xwax

# libusb through pkg-config where it exists. The dev Mac has no Homebrew and
# libusb built from source into ~/.local (dev/notes.md), so fall back there.
LIBUSB_CFLAGS ?= $(shell pkg-config --cflags libusb-1.0 2>/dev/null || echo -I$(HOME)/.local/include/libusb-1.0)
LIBUSB_LIBS ?= $(shell pkg-config --libs libusb-1.0 2>/dev/null || echo -L$(HOME)/.local/lib -lusb-1.0)
# The MIDI port is the one platform-specific piece of v7d: ALSA seq or CoreMIDI.
ifeq ($(shell uname),Darwin)
PORT_LIBS = -framework CoreMIDI -framework CoreFoundation -Wno-deprecated-declarations
# The dev Mac has libusb built from source into ~/.local and its dylib carries
# an absolute install name under one user's home, which a .app cannot ship.
# The same `make install` left the archive next to it: link that when present.
LIBUSB_A = $(HOME)/.local/lib/libusb-1.0.a
ifneq ($(wildcard $(LIBUSB_A)),)
LIBUSB_LIBS = $(LIBUSB_A) -lobjc -framework IOKit -framework CoreFoundation -framework Security
endif
else
PORT_LIBS = -lasound -lpthread
endif

# The portable core as a shared library, so usb/v7tc.py can call it through
# ctypes on macOS, where there is no JACK and the encoder feeds iso OUT directly.
SOEXT = $(if $(filter Darwin,$(shell uname)),.dylib,.so)
CORE = libv7core$(SOEXT)

# linux/v7dvs.c is ALSA seq in, JACK out: mode B's timecode, Linux only.
LINUX_ONLY = $(if $(filter Darwin,$(shell uname)),,v7dvs)

all: $(LINUX_ONLY) v7d tcgen platter_test midiparse_test encoder_test $(CORE)

$(CORE): src/encoder.c src/platter.c src/encoder.h src/platter.h
	cc $(CFLAGS) -shared -fPIC -o $@ src/encoder.c src/platter.c -lm

v7dvs: linux/v7dvs.c src/encoder.c src/platter.c src/encoder.h src/platter.h VERSION
	cc $(CFLAGS) -o $@ linux/v7dvs.c src/encoder.c src/platter.c \
		-ljack -lasound -lpthread -lm

# The USB daemon: the V7 as a MIDI port plus, with --timecode, mode A rendered
# straight into iso OUT. One binary on Linux, macOS and the Pi.
v7d: usb/v7d.c src/midiparse.c src/encoder.c src/platter.c \
     src/midiparse.h src/encoder.h src/platter.h VERSION
	cc $(CFLAGS) $(LIBUSB_CFLAGS) -o $@ usb/v7d.c src/midiparse.c \
		src/encoder.c src/platter.c $(LIBUSB_LIBS) $(PORT_LIBS) -lm

tcgen: tests/tcgen.c src/encoder.c src/encoder.h
	cc $(CFLAGS) -o $@ tests/tcgen.c src/encoder.c -lm

platter_test: tests/platter_test.c src/platter.c src/platter.h
	cc $(CFLAGS) -o $@ tests/platter_test.c src/platter.c -lm

tracereplay: tests/tracereplay.c src/midiparse.c src/platter.c src/midiparse.h src/platter.h
	cc $(CFLAGS) -o $@ tests/tracereplay.c src/midiparse.c src/platter.c -lm

midiparse_test: tests/midiparse_test.c src/midiparse.c src/midiparse.h
	cc $(CFLAGS) -o $@ tests/midiparse_test.c src/midiparse.c

encoder_test: tests/encoder_test.c src/encoder.c src/encoder.h
	cc $(CFLAGS) -o $@ tests/encoder_test.c src/encoder.c -lm

# The decoder is xwax's own, linked as-is, so a check is against the real thing
# and not against our idea of it. Needs an xwax checkout next door; everything
# else builds without it.
tcdec: tests/tcdec.c
	cc $(CFLAGS) -I$(XWAX) -o $@ tests/tcdec.c $(XWAX)/timecoder.c $(XWAX)/lut.c -lm

check: all
	./check.sh

# Packages: .deb, .rpm and Arch, from packaging/nfpm.yaml, through nfpm's own
# container so nothing needs installing. pkg-test then installs each one in a
# fresh container of its distribution and runs the self-tests -- the only way
# the dependency names in that file get proven.
NFPM = docker run --rm -u $$(id -u):$$(id -g) -v $(CURDIR):/w -w /w \
       -e VERSION=$(shell cat VERSION) goreleaser/nfpm:latest
pkg: v7dvs v7d
	rm -rf dist && mkdir dist
	for f in deb rpm archlinux; do \
	    $(NFPM) package -f packaging/nfpm.yaml -p $$f -t dist/ || exit 1; done
	ls -1 dist/

pkg-test: pkg
	packaging/test.sh

# The Mac bundle, for testers who install nothing: py2app wraps launcher.py
# with python.org's Python (the only one whose Tk opens a window on Big Sur,
# relocated into the bundle) and carries ./v7d as a resource. Signed ad-hoc:
# Apple Silicon refuses an unsigned binary outright, and Gatekeeper's
# "unidentified developer" is what packaging/macos/README-macos.md explains.
# x86_64 throughout, so nothing in the bundle is mixed; Rosetta runs it on
# Apple Silicon. Only runs on macOS, and only with python.org's python3.
PY ?= /usr/local/bin/python3
APP = dist/v7dvs.app
ZIP = dist/v7dvs-$(shell cat VERSION)-macos.zip
app: v7d
	@test "$$(uname)" = Darwin || { echo "make app is for macOS"; exit 1; }
	@otool -L v7d | grep -q libusb && \
	    { echo "v7d links libusb dynamically; the bundle needs $(LIBUSB_A)"; exit 1; } || true
	@$(PY) -c "import py2app" 2>/dev/null || \
	    { echo "py2app missing: $(PY) -m pip install --user py2app"; exit 1; }
	rm -rf $(APP) build/py2app
	$(PY) packaging/macos/setup.py py2app -q -d dist -b build/py2app
	chmod 755 $(APP)/Contents/Resources/v7d
	@# py2app leaves a cache of its own site.py compiled here, with the builder's
	@# home directory in it. Nothing loads it: the bundle imports site.pyc.
	rm -rf $(APP)/Contents/Resources/__pycache__
	codesign --force --deep --sign - $(APP)
	codesign --verify --deep --strict $(APP)
	rm -f $(ZIP) && ditto -c -k --keepParent $(APP) $(ZIP)
	@echo $(ZIP)

# The guides as PDFs for people who will never open the repo: every docs/*.md
# becomes dist/v7dvs-<name>-guide.pdf, diagrams and all. Chrome does the
# printing; python3-markdown turns the page into HTML first.
CHROME ?= google-chrome
GUIDES = $(patsubst docs/%.md,dist/v7dvs-%-guide.pdf,$(wildcard docs/*.md))
guide: $(GUIDES)
dist/v7dvs-%-guide.pdf: docs/%.md packaging/guide2pdf.py VERSION $(wildcard docs/img/*.svg)
	@mkdir -p dist
	python3 packaging/guide2pdf.py $< dist/$*.html $(shell cat VERSION)
	$(CHROME) --headless --disable-gpu --no-pdf-header-footer \
	    --print-to-pdf=$(abspath $@) file://$(abspath dist/$*.html) 2>/dev/null
	@rm -f dist/$*.html
	@echo $@

clean:
	rm -f v7dvs v7d tcgen tcdec tracereplay platter_test midiparse_test encoder_test libv7core.so libv7core.dylib
	rm -rf dist

.PHONY: all check clean pkg pkg-test app guide
