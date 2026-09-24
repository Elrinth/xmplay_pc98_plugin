# xmp-pc98 — native MinGW i686 XMPlay plugin
# S98 (cisc fmgen) + PMD (pmdmini / PMDWin) + FMP + BGMDRV/NA/MFD/N3G/PAI/MSB/MD/NTL/FMD + Hoot
#
# /usr/bin/make          # host tests + 32-bit DLL
# /usr/bin/make dll      # dist/xmp-pc98.dll
# /usr/bin/make test     # host render tests
# /usr/bin/make pack     # xmp-pc98-1.0.27.zip
# /usr/bin/make scan     # dist/pc98-scan.exe (or host binary)
# /usr/bin/make fetch-xml

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
DIST := $(ROOT)/dist
SRC  := $(ROOT)/src
INC  := $(ROOT)/include/xmplay
PMD  := $(ROOT)/third_party/pmdmini/src
FMGEN := $(ROOT)/third_party/fmgen
OBJ  := $(DIST)/obj
OBJW := $(DIST)/obj-i686

I686_HOST := i686-w64-mingw32
I686_CC   := $(I686_HOST)-gcc
I686_CXX  := $(I686_HOST)-g++
I686_WINDRES := $(I686_HOST)-windres
# MSYS2 mingw32 (this machine): use it for host tests and the DLL.
MINGW32 ?= /c/msys64/mingw32/bin
ifneq ($(wildcard $(MINGW32)/i686-w64-mingw32-g++.exe),)
  CC  := $(MINGW32)/i686-w64-mingw32-gcc
  CXX := $(MINGW32)/i686-w64-mingw32-g++
  I686_CC := $(MINGW32)/i686-w64-mingw32-gcc
  I686_CXX := $(MINGW32)/i686-w64-mingw32-g++
  I686_WINDRES := $(firstword $(wildcard $(MINGW32)/$(I686_HOST)-windres.exe $(MINGW32)/windres.exe))
endif

TSF  := $(ROOT)/third_party/tinysoundfont
INCS = -I$(SRC) -I$(INC) -I$(PMD) -I$(PMD)/pmdwin -I$(PMD)/ymfm -I$(FMGEN) -I$(TSF)

# GCC writes %TEMP% asm scratch files; keep them out of C:\Windows.
export TMP := $(DIST)/tmp
export TEMP := $(DIST)/tmp
$(shell mkdir -p $(DIST)/tmp)

CFLAGS_COM = -O2 -fno-strict-aliasing \
	-Wall -Wno-unused-function -Wno-unused-parameter \
	-Wno-unused-variable -Wno-unused-but-set-variable \
	-Wno-sign-compare -Wno-unknown-pragmas \
	-DNDEBUG
CXXFLAGS_COM = $(CFLAGS_COM) -std=c++14

CFLAGS_L = $(CFLAGS_COM)
CFLAGS_W = $(CFLAGS_COM) -DWIN32 -D_WIN32
CXXFLAGS_L = $(CXXFLAGS_COM)
CXXFLAGS_W = $(CXXFLAGS_COM) -DWIN32 -D_WIN32

# pmdmini ymfm + PMDWin core (not pmdwin.cpp — it has DllMain)
YMFM_CXX = ymfm_adpcm.cpp ymfm_opn.cpp ymfm_ssg.cpp ymfm_opl.cpp ymfm_pcm.cpp opna.cpp file_fmgen.cpp sjis2utf.cpp
PMDWIN_CXX = pmdwincore.cpp opnaw.cpp p86drv.cpp ppsdrv.cpp ppz8l.cpp table.cpp util.cpp

OUR_CXX = player.cpp engines/s98_engine.cpp engines/pmd_engine.cpp \
	engines/fmp_engine.cpp engines/hoot_engine.cpp engines/bgmdrv_engine.cpp \
	engines/na_engine.cpp engines/mfd_engine.cpp engines/n3g_engine.cpp \
	engines/pai_engine.cpp engines/msb_engine.cpp engines/md_engine.cpp \
	engines/ntl_engine.cpp engines/gntl_engine.cpp engines/fmd_engine.cpp engines/msdrv_engine.cpp
OUR_C = config.c

# cisc fmgen (in_s98 core). Skip opm.cpp — S98 OPM stays silent.
# Rename namespace FM → CISCFM so it does not collide with pmdmini's ymfm shim.
FMGEN_CXX = fmgen.cpp opna.cpp psg.cpp fmtimer.cpp
FMGEN_FLAGS = -DFM=CISCFM

.PHONY: all dll test pack scan fetch-xml clean

all: test dll

dll: $(DIST)/xmp-pc98.dll

scan: $(DIST)/pc98-scan$(if $(filter Windows_NT,$(OS)),.exe,)

fetch-xml:
	$(ROOT)/scripts/fetch_xml.sh $(DIST)/hoot-xml

# ---- host objects ----
define HOST_YMFM
$(OBJ)/ymfm/$(basename $(1)).o: $(PMD)/ymfm/$(1)
	@mkdir -p $$(dir $$@)
	$(CXX) $(CXXFLAGS_L) $(INCS) -c -o $$@ $$<
endef
define HOST_PMDWIN
$(OBJ)/pmdwin/$(basename $(1)).o: $(PMD)/pmdwin/$(1)
	@mkdir -p $$(dir $$@)
	$(CXX) $(CXXFLAGS_L) $(INCS) -c -o $$@ $$<
endef
define HOST_OUR_CXX
$(OBJ)/$(basename $(1)).o: $(SRC)/$(1)
	@mkdir -p $$(dir $$@)
	$(CXX) $(CXXFLAGS_L) $(INCS) -c -o $$@ $$<
endef
define HOST_FMGEN
$(OBJ)/fmgen/$(basename $(1)).o: $(FMGEN)/fmgen/$(1)
	@mkdir -p $$(dir $$@)
	$(CXX) $(CXXFLAGS_L) $(INCS) $(FMGEN_FLAGS) -c -o $$@ $$<
endef

$(foreach f,$(YMFM_CXX),$(eval $(call HOST_YMFM,$(f))))
$(foreach f,$(PMDWIN_CXX),$(eval $(call HOST_PMDWIN,$(f))))
$(foreach f,$(OUR_CXX),$(eval $(call HOST_OUR_CXX,$(f))))
$(foreach f,$(FMGEN_CXX),$(eval $(call HOST_FMGEN,$(f))))

$(OBJ)/config.o: $(SRC)/config.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_L) $(INCS) -c -o $@ $<

HOST_LIB = $(addprefix $(OBJ)/ymfm/,$(addsuffix .o,$(basename $(YMFM_CXX)))) \
	$(addprefix $(OBJ)/pmdwin/,$(addsuffix .o,$(basename $(PMDWIN_CXX)))) \
	$(addprefix $(OBJ)/fmgen/,$(addsuffix .o,$(basename $(FMGEN_CXX)))) \
	$(addprefix $(OBJ)/,$(addsuffix .o,$(basename $(OUR_CXX)))) \
	$(OBJ)/config.o

$(DIST)/test_s98_render: $(ROOT)/tests/test_s98_render.c $(HOST_LIB)
	mkdir -p $(DIST)
	$(CXX) $(CXXFLAGS_L) $(INCS) -o $@ $^

$(DIST)/test_pmd_render: $(ROOT)/tests/test_pmd_render.c $(HOST_LIB)
	mkdir -p $(DIST)
	$(CXX) $(CXXFLAGS_L) $(INCS) -o $@ $^

$(DIST)/test_pc98_render: $(ROOT)/tests/test_pc98_render.c $(HOST_LIB)
	mkdir -p $(DIST)
	$(CXX) $(CXXFLAGS_L) $(INCS) -o $@ $^

$(DIST)/pc98-scan: $(SRC)/scan/pc98-scan.c $(HOST_LIB)
	mkdir -p $(DIST)
	$(CXX) $(CXXFLAGS_L) $(INCS) -o $@ $^

test: $(DIST)/test_s98_render $(DIST)/test_pmd_render $(DIST)/test_pc98_render
	$(DIST)/test_s98_render
	$(DIST)/test_pmd_render
	$(DIST)/test_pc98_render

# ---- MinGW i686 ----
define WIN_YMFM
$(OBJW)/ymfm/$(basename $(1)).o: $(PMD)/ymfm/$(1)
	@mkdir -p $$(dir $$@)
	$(I686_CXX) $(CXXFLAGS_W) $(INCS) -c -o $$@ $$<
endef
define WIN_PMDWIN
$(OBJW)/pmdwin/$(basename $(1)).o: $(PMD)/pmdwin/$(1)
	@mkdir -p $$(dir $$@)
	$(I686_CXX) $(CXXFLAGS_W) $(INCS) -c -o $$@ $$<
endef
define WIN_OUR_CXX
$(OBJW)/$(basename $(1)).o: $(SRC)/$(1)
	@mkdir -p $$(dir $$@)
	$(I686_CXX) $(CXXFLAGS_W) $(INCS) -c -o $$@ $$<
endef
define WIN_FMGEN
$(OBJW)/fmgen/$(basename $(1)).o: $(FMGEN)/fmgen/$(1)
	@mkdir -p $$(dir $$@)
	$(I686_CXX) $(CXXFLAGS_W) $(INCS) $(FMGEN_FLAGS) -c -o $$@ $$<
endef

$(foreach f,$(YMFM_CXX),$(eval $(call WIN_YMFM,$(f))))
$(foreach f,$(PMDWIN_CXX),$(eval $(call WIN_PMDWIN,$(f))))
$(foreach f,$(OUR_CXX),$(eval $(call WIN_OUR_CXX,$(f))))
$(foreach f,$(FMGEN_CXX),$(eval $(call WIN_FMGEN,$(f))))

$(OBJW)/config.o: $(SRC)/config.c
	@mkdir -p $(dir $@)
	$(I686_CC) $(CFLAGS_W) $(INCS) -c -o $@ $<

$(OBJW)/xmp-pc98.res: $(SRC)/xmp-pc98.rc
	@mkdir -p $(dir $@)
	$(I686_WINDRES) -O coff -o $@ $<

WIN_LIB = $(addprefix $(OBJW)/ymfm/,$(addsuffix .o,$(basename $(YMFM_CXX)))) \
	$(addprefix $(OBJW)/pmdwin/,$(addsuffix .o,$(basename $(PMDWIN_CXX)))) \
	$(addprefix $(OBJW)/fmgen/,$(addsuffix .o,$(basename $(FMGEN_CXX)))) \
	$(addprefix $(OBJW)/,$(addsuffix .o,$(basename $(OUR_CXX)))) \
	$(OBJW)/config.o

$(DIST)/xmp-pc98.dll: $(SRC)/xmp-pc98.cpp $(SRC)/xmp-pc98.def $(WIN_LIB) $(OBJW)/xmp-pc98.res
	mkdir -p $(DIST)
	$(I686_CXX) -shared -O2 -DNDEBUG -std=c++14 \
		-static -static-libgcc -static-libstdc++ \
		$(INCS) -DWIN32 -D_WIN32 \
		-o $@ $(SRC)/xmp-pc98.cpp $(SRC)/xmp-pc98.def \
		$(WIN_LIB) $(OBJW)/xmp-pc98.res \
		-Wl,--kill-at -Wl,--add-stdcall-alias \
		-luser32 -lgdi32 -lwinmm -Wl,-s
	-$(firstword $(wildcard $(MINGW32)/objdump.exe) objdump) -p $@ | grep -E 'DllName|XMPIN_GetInterface|file format' || true
	-file $@

$(DIST)/pc98-scan.exe: $(SRC)/scan/pc98-scan.c $(WIN_LIB)
	mkdir -p $(DIST)
	$(I686_CXX) -O2 -DNDEBUG -std=c++14 -static -static-libgcc -static-libstdc++ \
		$(INCS) -DWIN32 -D_WIN32 -o $@ $^ -luser32

pack: dll
	rm -rf $(DIST)/pack
	mkdir -p $(DIST)/pack
	cp -f $(DIST)/xmp-pc98.dll $(ROOT)/README.md $(ROOT)/LICENSE $(DIST)/pack/
	-cp -f $(DIST)/pc98-scan.exe $(DIST)/pack/
	-cp -f $(FMGEN)/readme.txt $(DIST)/pack/fmgen-readme.txt
	rm -f $(DIST)/xmp-pc98-1.0.27.zip
	powershell.exe -NoProfile -Command \
		"Compress-Archive -Path '$(DIST)/pack/*' -DestinationPath '$(DIST)/xmp-pc98-1.0.27.zip' -Force"
	rm -rf $(DIST)/pack
	ls -l $(DIST)/xmp-pc98.dll $(DIST)/xmp-pc98-1.0.27.zip

clean:
	rm -rf $(DIST)/xmp-pc98.dll $(DIST)/pc98-scan $(DIST)/pc98-scan.exe \
		$(DIST)/test_s98_render $(DIST)/test_pmd_render $(DIST)/test_pc98_render \
		$(DIST)/obj $(DIST)/obj-i686 $(DIST)/pack $(DIST)/xmp-pc98-1.0.27.zip
