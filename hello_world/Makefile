#Notes:
#1. The make target depends on how eRPC was compiled:
#   * If DTRANSPORT=dpdk, use `make dpdk`
#   * If DTRANSPORT=raw, use `make raw`
#   * If DTRANSPORT=infiniband, use `make infiniband`
LIBS = -lerpc -lpthread -lnuma -ldl

PKGCONF ?= pkg-config

ifneq ($(shell $(PKGCONF) --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found")
endif

all: dpdk


CFLAGS += -O3  $(shell $(PKGCONF) --cflags libdpdk) -g -std=c++11
CFLAGS += -DALLOW_EXPERIMENTAL_API
LDFLAGS_SHARED = $(shell $(PKGCONF) --libs libdpdk)

error:
	@echo "Please choose one of the following targets: infiniband, raw, dpdk, clean"
	@exit 2
infiniband:
	g++ $(CFLAGS) -o server server.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_INFINIBAND=true
	g++ $(CFLAGS) -o client client.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_INFINIBAND=true
raw:
	g++ $(CFLAGS) -o server server.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_RAW=true
	g++ $(CFLAGS) -o client client.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_RAW=true
dpdk:
	g++ $(CFLAGS) -o server server.cc \
		-Wno-deprecated-declarations \
		-DERPC_DPDK=true -march=native \
		-I ../src -I ../third_party/asio/include -I /usr/include/dpdk \
		-L ../build $(LIBS) \
		-Wl,--whole-archive $(LDFLAGS_SHARED) -Wl,--no-whole-archive \
		-lnuma -ldl -libverbs -lmlx5

	g++ $(CFLAGS) -o client client.cc \
		-Wno-deprecated-declarations \
		-DERPC_DPDK=true -march=native \
		-I ../src -I ../third_party/asio/include -I /usr/include/dpdk \
		-L ../build $(LIBS) \
		-Wl,--whole-archive $(LDFLAGS_SHARED) -Wl,--no-whole-archive \
		-lnuma -ldl -libverbs -lmlx5
clean:
	rm server client

