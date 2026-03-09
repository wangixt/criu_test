PLUGIN_NAME		:= npu_plugin
PLUGIN_SOBJ		:= npu_plugin.so

ifndef ARCH
	ARCH ?= $(shell uname -m)
	ifeq ($(ARCH), aarch64)
	ARCH := aarch64
	endif
	ifeq ($(ARCH), arm64)
	ARCH := arm
	endif
	ifeq ($(ARCH), x86_64)
	ARCH := x86
	endif
endif

PLUGIN_INCLUDE  	:= -iquote../../include
PLUGIN_INCLUDE  	+= -iquote../../criu/include
PLUGIN_INCLUDE  	+= -iquote../../criu/arch/$(ARCH)/include/
PLUGIN_INCLUDE  	+= -iquote../../
 
COMPEL                 := ../../compel/compel-host
DEPS_OK 		:= npu_plugin.so
DEPS_NOK 		:= ;
 
__nmk_dir ?= ../../scripts/nmk/scripts/
include $(__nmk_dir)msg.mk
 
CC      		:= gcc
PLUGIN_CFLAGS  	:= -g -Wall -Werror -D _GNU_SOURCE -shared -nostartfiles -fPIC \
-DCR_PLUGIN_DEFAULT="$(PLUGINDIR)" -ftrapv
PLUGIN_LDFLAGS	:= -lpthread -lrt -s
 
ifeq ($(CONFIG_NPU),y)
        all: $(DEPS_OK)
else
        all: $(DEPS_NOK)
endif
 
criu-npu.pb-c.c: criu-npu.proto
		protoc-c --proto_path=. --c_out=. criu-npu.proto
 
npu_plugin.so: npu_plugin.c criu-npu.pb-c.c
	$(CC) $(PLUGIN_CFLAGS) $(shell $(COMPEL) includes) $^ -o $@ $(PLUGIN_INCLUDE) $(PLUGIN_LDFLAGS) $(LIBDRM_INC)
 
npu_plugin_clean:
	$(call msg-clean, $@)
	$(Q) $(RM) npu_plugin.so criu-npu.pb-c*
.PHONY: npu_plugin_clean
 
clean: npu_plugin_clean
 
mrproper: clean
 
install:
ifeq ($(CONFIG_NPU),y)
	$(Q) mkdir -p $(DESTDIR)$(PLUGINDIR)
	$(E) "  INSTALL " $(PLUGIN_NAME)
	$(Q) install -m 644 $(PLUGIN_SOBJ) $(DESTDIR)$(PLUGINDIR)
endif
.PHONY: install
 
uninstall:
ifeq ($(CONFIG_NPU),y)
	$(E) " UNINSTALL" $(PLUGIN_NAME)
	$(Q) $(RM) $(DESTDIR)$(PLUGINDIR)/$(PLUGIN_SOBJ)
endif
.PHONY: uninstall

UT_BIN      := unit/npu_plugin_ut
UT_NPU_DEVICE ?= /dev/null
UT_CFLAGS   := -O0 -g --coverage -DNPU_PLUGIN_UT -Wall -Wextra -Werror -Wno-unused-parameter -D_GNU_SOURCE \
-Wno-sign-compare -Wno-sign-conversion -Wno-format-truncation -DNPU_DEVICE=\"$(UT_NPU_DEVICE)\"
UT_LDFLAGS  := --coverage -Wl,--wrap=strdup -Wl,--wrap=getpwnam_r
UT_LIBS     := -lpthread -lrt -lprotobuf-c

unit: $(UT_BIN)

$(UT_BIN): unit/npu_plugin_ut.o unit/npu_plugin_ut_stubs.o npu_plugin_ut.o criu-npu.pb-c.o
	$(CC) $(UT_LDFLAGS) -o $@ $^ $(UT_LIBS)

npu_plugin_ut.o: npu_plugin.c
	$(CC) $(UT_CFLAGS) $(shell $(COMPEL) includes) $(PLUGIN_INCLUDE) -c -o $@ $<

criu-npu.pb-c.o: criu-npu.pb-c.c
	$(CC) $(UT_CFLAGS) -c -o $@ $<

unit/%.o: unit/%.c
	$(CC) $(UT_CFLAGS) $(shell $(COMPEL) includes) $(PLUGIN_INCLUDE) -c -o $@ $<

unit-run: unit-clean unit
	$(RM) -f *.gcda *.gcov unit/*.gcda unit/*.gcov
	./$(UT_BIN)

unit-gcov: unit-run
	gcov -b -c npu_plugin.c

unit-clean:
	$(RM) -f $(UT_BIN) unit/*.o npu_plugin_ut.o criu-npu.pb-c-ut.o *.gcno *.gcda *.gcov unit/*.gcno \
unit/*.gcda unit/*.gcov

.PHONY: unit unit-run unit-gcov unit-clean
