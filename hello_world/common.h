#include <stdio.h>
#include "rpc.h"

static const std::string kServerHostname = "r2";
static const std::string kClientHostname = "r3";

static constexpr uint16_t kSUDPPort = 31850;
static constexpr uint16_t kCUDPPort = 31851;

static constexpr uint8_t kReqType = 2;
static constexpr size_t kMsgSize = 16;
