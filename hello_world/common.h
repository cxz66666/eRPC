#include <stdio.h>
#include "rpc.h"

static const std::string kServerHostname = "192.168.189.8";
static const std::string kClientHostname = "192.168.189.9";

static constexpr uint16_t kSUDPPort = 31851;
static constexpr uint16_t kCUDPPort = 31851;

static constexpr uint8_t kReqType = 2;
static constexpr size_t kMsgSize = 16;
