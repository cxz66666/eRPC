#include <gflags/gflags.h>
#include <signal.h>
#include <cstring>
#include <unordered_set>
#include "../apps_common.h"
#include "rpc.h"
#include "util/autorun_helpers.h"
#include "util/numautils.h"
#include "util/timer.h"
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memcpy.h>
#include <chrono>

static constexpr size_t kAppEvLoopMs = 1000; // Duration of event loop
static constexpr bool kAppVerbose = false;   // Print debug info on datapath
static constexpr bool kAppMeasureLatency = false;
static constexpr double kAppLatFac = 3.0;       // Precision factor for latency
static constexpr bool kAppPayloadCheck = false; // Check full request/response

// Optimization knobs. Set to true to disable optimization.
static constexpr bool kAppOptDisablePreallocResp = false;
static constexpr bool kAppOptDisableRxRingReq = false;

static constexpr size_t kAppReqType = 1;   // eRPC request type
static constexpr uint8_t kAppDataByte = 3; // Data transferred in req & resp
static constexpr size_t kAppMaxConcurrency = 256;

DEFINE_uint64(msg_size, 0, "Request and response size");
DEFINE_uint64(num_threads, 1, "Number of threads at the server machine");
DEFINE_uint64(concurrency, 0, "Concurrent batches per thread");
DEFINE_uint64(loop, 0, "loop for test");
volatile sig_atomic_t ctrl_c_pressed = 0;
void ctrl_c_handler(int) { ctrl_c_pressed++; }

// Per-thread application context
class ClientContext : public BasicAppContext
{
public:
  long total_delay;
  long num;
  ~ClientContext()
  {
  }
};

void memcpy_func(size_t thread_id, erpc::Nexus *nexus)
{
  ClientContext c;
  c.thread_id_ = thread_id;

  std::vector<size_t> port_vec = flags_get_numa_ports(FLAGS_numa_node);
  erpc::rt_assert(port_vec.size() > 0);
  uint8_t phy_port = port_vec.at(thread_id % port_vec.size());

  erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(&c),
                                  static_cast<uint8_t>(thread_id),
                                  basic_sm_handler, phy_port);

  rpc.retry_connect_on_invalid_rpc_id_ = true;
  c.rpc_ = &rpc;

  erpc::MsgBuffer msg_buffer = c.rpc_->alloc_msg_buffer(FLAGS_msg_size);
  rte_mempool *mempool = rte_mempool_lookup((std::string("erpc-mp-") + std::to_string(phy_port) +
                                             std::string("-") + std::to_string(thread_id))
                                                .c_str());
  const long batch_size = 512;

  rte_mbuf *tx_mbufs[batch_size];
  const size_t pkt_size = FLAGS_msg_size;

  long loop = static_cast<long>(FLAGS_loop);
  while (loop--)
  {
    if (ctrl_c_pressed)
    {
      break;
    }
    for (size_t i = 0; i < batch_size; i++)
    {
      tx_mbufs[i] = rte_pktmbuf_alloc(mempool);
    }
    auto start = std::chrono::system_clock::now();
    for (size_t i = 0; i < batch_size; i++)
    {
      rte_memcpy_aligned(rte_pktmbuf_mtod(tx_mbufs[i], uint8_t *), msg_buffer.buf_ - 42, pkt_size);
    }
    auto end = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    c.total_delay += duration.count();

    c.num += batch_size;
    for (size_t i = 0; i < batch_size; i++)
    {
      rte_pktmbuf_free(tx_mbufs[i]);
    }
  }
  printf(
      "Process %zu, thread %zu: %.3f ns \n",
      FLAGS_process_id, c.thread_id_, c.total_delay * 1.0 / c.num);
}

int main(int argc, char **argv)
{
  signal(SIGINT, ctrl_c_handler);
  signal(SIGTERM, ctrl_c_handler);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  erpc::rt_assert(FLAGS_concurrency <= kAppMaxConcurrency, "Invalid cncrrncy.");
  erpc::rt_assert(FLAGS_numa_node <= 1, "Invalid NUMA node");

  // We create a bit fewer sessions
  const size_t num_sessions = 1;
  erpc::rt_assert(num_sessions * erpc::kSessionCredits <=
                      erpc::Transport::kNumRxRingEntries,
                  "Too few ring buffers");

  erpc::Nexus nexus(erpc::get_uri_for_process(FLAGS_process_id),
                    FLAGS_numa_node, 0);
  std::vector<std::thread> threads(FLAGS_num_threads);

  threads[0] = std::thread(memcpy_func, 0, &nexus);
  // wait for dpdk init
  usleep(2e6);

  erpc::bind_to_core(threads[0], FLAGS_numa_node, 0);

  for (size_t i = 1; i < FLAGS_num_threads; i++)
  {
    threads[i] = std::thread(memcpy_func, i, &nexus);
    erpc::bind_to_core(threads[i], FLAGS_numa_node, i);
  }
  for (size_t i = 0; i < FLAGS_num_threads; i++)
  {
    threads[i].join();
  }
}
