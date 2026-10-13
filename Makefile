ifneq ($(shell pkg-config --exists vinylapi 2>/dev/null && echo y),)
API      := vinylapi
FLAVOUR  := vinyl
VTEST    ?= vinyltest
else ifneq ($(shell pkg-config --exists varnishapi 2>/dev/null && echo y),)
API      := varnishapi
VTEST    ?= varnishtest
else
$(error no Varnish development files found: pkg-config knows neither vinylapi nor varnishapi)
endif
INC      ?= $(shell pkg-config --variable=pkgincludedir $(API))
VMODTOOL ?= $(shell pkg-config --variable=vmodtool $(API))
VMODDIR  ?= $(shell pkg-config --variable=vmoddir $(API))
FLAVOUR  ?= $(if $(findstring varnish-plus,$(INC)),enterprise,varnish)
ABIHASH  := $(shell grep -h 'define VMOD_ABI_Version' $(INC)/vmod_abi.h 2>/dev/null | cksum | cut -d' ' -f1)
B        ?= build/$(FLAVOUR)-$(ABIHASH)

# SHA-256 comes from OpenSSL when its development files are installed (hardware accelerated, and
# the same libcrypto that varnishd itself has loaded), else from Varnish's built-in implementation.
# gzip inflate uses a vendored copy of libdeflate, linked statically with hidden symbols.
HAVE_CRYPTO := $(shell pkg-config --exists libcrypto 2>/dev/null && echo y)
PERF_CFLAGS :=
PERF_LIBS   :=
ifeq ($(HAVE_CRYPTO),y)
PERF_CFLAGS += -DGC_HAVE_OPENSSL $(shell pkg-config --cflags libcrypto)
PERF_LIBS   += $(shell pkg-config --libs libcrypto)
endif

LD_DIR  := third_party/libdeflate
LD_SRCS := lib/utils.c lib/crc32.c lib/deflate_decompress.c lib/gzip_decompress.c \
    lib/arm/cpu_features.c lib/x86/cpu_features.c
LD_OBJS := $(addprefix $(B)/ld/,$(LD_SRCS:.c=.o))
INCS    := -Isrc -I$(LD_DIR)

$(B)/ld/%.o: $(LD_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) -O2 -fPIC -fvisibility=hidden -DNDEBUG -I$(LD_DIR) -I$(LD_DIR)/lib -c $< -o $@

CC       ?= gcc
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter

all: $(B)/libvmod_gitcache.so

$(B)/vcc_gitcache_if.c $(B)/vcc_gitcache_if.h: src/vmod_gitcache.vcc
	mkdir -p $(B)
	cd $(B) && python3 $(VMODTOOL) -o vcc_gitcache_if $(CURDIR)/src/vmod_gitcache.vcc

$(B)/libvmod_gitcache.so: src/vmod_gitcache.c src/pktline.c src/route.c src/pktline.h src/route.h src/compat.h src/vmod.map $(B)/vcc_gitcache_if.c $(LD_OBJS)
	$(CC) $(CFLAGS) $(PERF_CFLAGS) -shared -fPIC -Wl,--version-script=$(CURDIR)/src/vmod.map -I$(INC) $(INCS) -I$(B) -o $@ \
	    src/vmod_gitcache.c src/pktline.c src/route.c $(B)/vcc_gitcache_if.c $(LD_OBJS) $(PERF_LIBS)

SAN      ?= -fsanitize=address,undefined
unit: $(LD_OBJS)
	mkdir -p $(B)
	$(CC) -Wall -Wextra -g $(SAN) $(PERF_CFLAGS) $(INCS) -o $(B)/unit tests/unit.c tests/unit_route.c src/pktline.c src/route.c $(LD_OBJS) -lz -pthread
	$(B)/unit

install: all
	install -m 755 $(B)/libvmod_gitcache.so $(VMODDIR)/

e2e: all
	SRC=$(CURDIR) GC_BUILD=$(CURDIR)/$(B) tests/e2e/run.sh

upstream: all
	SRC=$(CURDIR) GC_BUILD=$(CURDIR)/$(B) tests/e2e/upstream.sh

cachebench: all
	SRC=$(CURDIR) GC_BUILD=$(CURDIR)/$(B) tests/bench/cache_vs_direct.sh

FUZZ_TIME ?= 60
fuzz: $(LD_OBJS)
	mkdir -p $(B)/corpus-canon $(B)/corpus-route
	cp tests/fuzz/corpus/* tests/fuzz/regress/* $(B)/corpus-canon/ 2>/dev/null || true
	cp tests/fuzz/corpus-route/* $(B)/corpus-route/
	clang -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -o $(B)/fuzz_canon tests/fuzz/fuzz_canon.c src/pktline.c
	clang -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined $(PERF_CFLAGS) $(INCS) -o $(B)/fuzz_route tests/fuzz/fuzz_route.c src/pktline.c src/route.c $(LD_OBJS) -lz
	$(B)/fuzz_canon $(B)/corpus-canon -max_total_time=$(FUZZ_TIME) -timeout=5 -artifact_prefix=tests/fuzz/crash-canon-
	$(B)/fuzz_route $(B)/corpus-route -max_total_time=$(FUZZ_TIME) -timeout=5 -artifact_prefix=tests/fuzz/crash-route-

vtc: all
	rm -rf $(B)/vtc && mkdir -p $(B)/vtc
	for f in tests/*.vtc; do \
	  if [ "$(FLAVOUR)" = vinyl ]; then sed 's/^varnish v\([0-9]\)/vinyl v\1/' $$f > $(B)/vtc/$$(basename $$f); \
	  else cp $$f $(B)/vtc/; fi; done
	$(VTEST) -Dvmod=$(CURDIR)/$(B)/libvmod_gitcache.so $(B)/vtc/*.vtc

check: install
	$(MAKE) vtc

clean:
	rm -rf $(B)
