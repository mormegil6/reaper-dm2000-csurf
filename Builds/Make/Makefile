# Makefile - macOS build for reaper_csurf_dm2000 (universal: arm64 + x86_64).
#
# This is a hand-written SWELL Makefile, the convention used by Cockos's own
# reaper extensions (SWS, ReaPack). It lives in its own Builds/Make directory and
# does NOT touch the Visual Studio solutions under Builds/VisualStudio201x - the
# Windows build is unaffected.
#
# Model: REAPER hosts SWELL. The extension compiles against SWELL's Win32-shaped
# headers (swell.h) and links exactly ONE SWELL source - swell-modstub.mm, built
# with -DSWELL_PROVIDED_BY_APP - which binds every SWELL call to REAPER's own
# implementation at load time. We do NOT build the full SWELL library.
#
# Prerequisites (see MACOS_BUILD.md):
#   - Xcode command line tools (clang++)
#   - php           (brew install php)  -- runs the resource generator
#   - WDL cloned to  ../../Source/WDL    (so Source/WDL/swell/swell.h exists)
#
# Usage:
#   make            # build the universal dylib
#   make install    # copy it to ~/Library/Application Support/REAPER/UserPlugins
#   make clean

SRCDIR  = ../../Source/jmde/csurf
JMDE    = ../../Source/jmde
WDL     = ../../Source/WDL
OUTDIR  = .
PLUGIN  = reaper_csurf_dm2000.dylib

# resgen is named swell_resgen.php in current WDL, mac_resgen.php in older trees.
PHP     = php
RESGEN  = $(firstword $(wildcard $(WDL)/swell/swell_resgen.php $(WDL)/swell/mac_resgen.php))
RC      = $(SRCDIR)/res.rc
RESDLG  = $(SRCDIR)/res.rc_mac_dlg
RESMENU = $(SRCDIR)/res.rc_mac_menu

# One fat invocation: -mmacosx-version-min=10.9 covers older Intel; the arm64
# 11.0 floor is only enforced at runtime (always satisfied on Apple Silicon).
# If clang on the build Mac rejects 10.9 for the arm64 slice, build the two
# arches separately (x86_64@10.9, arm64@11.0) and `lipo -create` them - see
# MACOS_BUILD.md.
ARCHS   = -arch x86_64 -arch arm64
VERMIN  = -mmacosx-version-min=10.9

CXX      = clang++
DEFS     = -DSWELL_PROVIDED_BY_APP
INCS     = -I$(SRCDIR) -I$(JMDE) -I$(WDL) -I$(WDL)/swell -I../../Source
CXXFLAGS = $(ARCHS) $(VERMIN) -fPIC -fvisibility=hidden -O2 -std=c++14 \
           -Wno-deprecated-declarations -Wno-write-strings $(DEFS) $(INCS)
LDFLAGS  = $(ARCHS) $(VERMIN) -dynamiclib -framework Cocoa

# Every csurf surface registered in csurf_main.cpp must be compiled so all the
# reg structs resolve at link time (same source set as the VS project).
CPPSRCS = $(SRCDIR)/csurf_main.cpp $(SRCDIR)/csurf_dm2000.cpp \
          $(SRCDIR)/csurf_mcu.cpp $(SRCDIR)/csurf_bcf2000.cpp \
          $(SRCDIR)/csurf_faderport.cpp $(SRCDIR)/csurf_babyhui.cpp \
          $(SRCDIR)/csurf_tranzport.cpp $(SRCDIR)/csurf_alphatrack.cpp \
          $(SRCDIR)/csurf_01X.cpp
MMSRCS  = $(WDL)/swell/swell-modstub.mm

OBJS    = $(CPPSRCS:.cpp=.o) $(MMSRCS:.mm=.o)

all: $(OUTDIR)/$(PLUGIN)

# Transpile res.rc -> res.rc_mac_dlg / res.rc_mac_menu (the files csurf_main.cpp
# #includes under #ifndef _WIN32). Regenerated whenever res.rc changes.
$(RESDLG) $(RESMENU): $(RC)
	@test -n "$(RESGEN)" || { echo "ERROR: swell_resgen.php not found under $(WDL)/swell - is WDL cloned to Source/WDL?"; exit 1; }
	$(PHP) $(RESGEN) --quiet $(RC)

# csurf_main.cpp pulls in the generated resource files, so it must wait for them.
$(SRCDIR)/csurf_main.o: $(RESDLG) $(RESMENU)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.mm
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUTDIR)/$(PLUGIN): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)
	@echo "built $@ ($$(lipo -archs $@ 2>/dev/null))"

install: all
	mkdir -p "$$HOME/Library/Application Support/REAPER/UserPlugins"
	cp $(OUTDIR)/$(PLUGIN) "$$HOME/Library/Application Support/REAPER/UserPlugins/"
	@if [ ! -f "$$HOME/Library/Application Support/REAPER/dm2000_keys.ini" ]; then \
		cp ../../doc/dm2000_keys.ini.example "$$HOME/Library/Application Support/REAPER/dm2000_keys.ini"; \
		echo "installed dm2000_keys.ini to ~/Library/Application Support/REAPER/"; \
	else \
		echo "dm2000_keys.ini already exists, skipped"; \
	fi
	@echo "installed to ~/Library/Application Support/REAPER/UserPlugins/$(PLUGIN)"

clean:
	rm -f $(OBJS) $(OUTDIR)/$(PLUGIN) $(RESDLG) $(RESMENU)

.PHONY: all install clean
