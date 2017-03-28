#include <gtest/gtest.h>
#include <string.h>
#include <atomic>
#include <cstring>
#include <thread>
#include "rpc.h"
#include "util/rand.h"
#include "util/test_printf.h"

using namespace ERpc;

static constexpr uint16_t kAppNexusUdpPort = 31851;
static constexpr double kAppNexusPktDropProb = 0.0;
static constexpr size_t kAppEventLoopMs = 200;
static constexpr size_t kAppMaxEventLoopMs = 20000; /* 20 seconds */
static constexpr uint8_t kAppClientAppTid = 100;
static constexpr uint8_t kAppServerAppTid = 200;
static constexpr uint8_t kAppReqType = 3;
static constexpr size_t kAppMinMsgSize =
    Rpc<IBTransport>::max_data_per_pkt() + 1; /* At least 2 packets */

/* Shared between client and server thread */
std::atomic<bool> server_ready; /* Client starts after server is ready */
std::atomic<bool> client_done;  /* Server ends after client is done */

const uint8_t phy_port = 0;
const size_t numa_node = 0;
char local_hostname[kMaxHostnameLen];

/// Per-thread application context
class AppContext {
 public:
  bool is_client;
  Rpc<IBTransport> *rpc;
  int *session_num_arr;
  FastRand fastrand;  ///< Used for picking large message sizes

  size_t num_sm_connect_resps = 0; /* Client-only */
  size_t num_rpc_resps = 0;        /* Client-only */
};

/// Pick a random message size with at least two packets
size_t pick_large_msg_size(AppContext *app_context) {
  assert(app_context != nullptr);
  uint32_t sample = app_context->fastrand.next_u32();
  uint32_t msg_size =
      sample % (Rpc<IBTransport>::kMaxMsgSize - kAppMinMsgSize) +
      kAppMinMsgSize;

  assert(msg_size >= kAppMinMsgSize &&
         msg_size <= Rpc<IBTransport>::kMaxMsgSize);
  return (size_t)msg_size;
}

/// The common request handler for all subtests. Copies the request string to
/// the response.
void req_handler(ReqHandle *req_handle, const MsgBuffer *req_msgbuf,
                 void *_context) {
  assert(req_handle != nullptr);
  assert(req_msgbuf != nullptr);
  assert(_context != nullptr);

  auto *context = (AppContext *)_context;
  ASSERT_FALSE(context->is_client);

  size_t req_size = req_msgbuf->get_data_size();

  req_handle->prealloc_used = false;

  /* MsgBuffer allocation is thread safe */
  req_handle->dyn_resp_msgbuf = context->rpc->alloc_msg_buffer(req_size);
  ASSERT_NE(req_handle->dyn_resp_msgbuf.buf, nullptr);
  size_t user_alloc_tot = context->rpc->get_stat_user_alloc_tot();

  memcpy((char *)req_handle->dyn_resp_msgbuf.buf, (char *)req_msgbuf->buf,
         req_size);

  test_printf(
      "Server: Received request of length %zu. "
      "Rpc memory used = %zu bytes (%.3f MB)\n",
      req_size, user_alloc_tot, (double)user_alloc_tot / MB(1));

  context->rpc->enqueue_response(req_handle);
}

/// The common continuation function for all subtests. This checks that the
/// request buffer is identical to the response buffer, and increments the
/// number of responses in the context.
void cont_func(RespHandle *resp_handle, const MsgBuffer *resp_msgbuf,
               void *_context, size_t tag) {
  assert(resp_handle != nullptr);
  assert(resp_msgbuf != nullptr);
  assert(_context != nullptr);
  _unused(tag);

  test_printf("Client: Received response of length %zu.\n",
              (char *)resp_msgbuf->get_data_size());

  /*
  ASSERT_EQ(req_msgbuf->get_data_size(), resp_msgbuf->get_data_size());
  ASSERT_STREQ((char *)req_msgbuf->buf, (char *)resp_msgbuf->buf);
  */

  auto *context = (AppContext *)_context;
  ASSERT_TRUE(context->is_client);
  context->num_rpc_resps++;

  context->rpc->release_respone(resp_handle);
}

/// The common session management handler for all subtests
void sm_hander(int session_num, SessionMgmtEventType sm_event_type,
               SessionMgmtErrType sm_err_type, void *_context) {
  _unused(session_num);

  auto *context = (AppContext *)_context;
  ASSERT_TRUE(context->is_client);
  context->num_sm_connect_resps++;

  ASSERT_EQ(sm_err_type, SessionMgmtErrType::kNoError);
  ASSERT_TRUE(sm_event_type == SessionMgmtEventType::kConnected ||
              sm_event_type == SessionMgmtEventType::kDisconnected);
}

/// The server thread used for all subtests
void server_thread_func(Nexus *nexus, uint8_t app_tid) {
  AppContext context;
  context.is_client = false;

  Rpc<IBTransport> rpc(nexus, (void *)&context, app_tid, &sm_hander, phy_port,
                       numa_node);
  context.rpc = &rpc;
  server_ready = true;

  while (!client_done) { /* Wait for the client */
    rpc.run_event_loop_timeout(kAppEventLoopMs);
  }

  /* The client is done after disconnecting */
  ASSERT_EQ(rpc.num_active_sessions(), 0);
}

/**
 * @brief Launch (possibly) multiple server threads and one client thread
 *
 * @param num_sessions The number of sessions needed by the client thread,
 * equal to the number of server threads launched
 *
 * @param num_bg_threads The number of background threads in the Nexus. If
 * this is non-zero, the request handler is executed in a background thread.
 *
 * @param client_thread_func The function executed by the client threads
 */
void launch_server_client_threads(size_t num_sessions, size_t num_bg_threads,
                                  void (*client_thread_func)(Nexus *, size_t)) {
  Nexus nexus(kAppNexusUdpPort, num_bg_threads, kAppNexusPktDropProb);

  if (num_bg_threads == 0) {
    nexus.register_req_func(
        kAppReqType, ReqFunc(req_handler, ReqFuncType::kForegroundTerminal));
  } else {
    nexus.register_req_func(kAppReqType,
                            ReqFunc(req_handler, ReqFuncType::kBackground));
  }

  server_ready = false;
  client_done = false;

  test_printf("Client: Using %zu sessions\n", num_sessions);

  std::thread server_thread[num_sessions];

  /* Launch one server Rpc thread for each client session */
  for (size_t i = 0; i < num_sessions; i++) {
    server_thread[i] =
        std::thread(server_thread_func, &nexus, kAppServerAppTid + i);
  }

  std::thread client_thread(client_thread_func, &nexus, num_sessions);

  for (size_t i = 0; i < num_sessions; i++) {
    server_thread[i].join();
  }

  client_thread.join();
}

/// Initialize client context and connect sessions
void client_connect_sessions(Nexus *nexus, AppContext &context,
                             size_t num_sessions) {
  assert(nexus != nullptr);
  assert(num_sessions >= 1);

  while (!server_ready) { /* Wait for server */
    usleep(1);
  }

  context.is_client = true;
  context.rpc = new Rpc<IBTransport>(nexus, (void *)&context, kAppClientAppTid,
                                     &sm_hander, phy_port, numa_node);

  /* Connect the sessions */
  context.session_num_arr = new int[num_sessions];
  for (size_t i = 0; i < num_sessions; i++) {
    context.session_num_arr[i] = context.rpc->create_session(
        local_hostname, kAppServerAppTid + (uint8_t)i, phy_port);
  }

  while (context.num_sm_connect_resps < num_sessions) {
    context.rpc->run_event_loop_one();
  }

  /* sm handler checks that the callbacks have no errors */
  ASSERT_EQ(context.num_sm_connect_resps, num_sessions);
}

/// Run the event loop until we get \p num_resps RPC responses, or until
/// kAppMaxEventLoopMs are elapsed.
void client_wait_for_rpc_resps_or_timeout(const Nexus *nexus,
                                          AppContext &context,
                                          size_t num_resps) {
  /* Run the event loop for up to kAppMaxEventLoopMs milliseconds */
  uint64_t cycles_start = rdtsc();
  while (context.num_rpc_resps != num_resps) {
    context.rpc->run_event_loop_timeout(kAppEventLoopMs);

    double ms_elapsed = to_msec(rdtsc() - cycles_start, nexus->freq_ghz);
    if (ms_elapsed > kAppMaxEventLoopMs) {
      break;
    }
  }
}

///
/// Test: Send one large request message and check that we receive the
/// correct response
///
void one_large_rpc(Nexus *nexus, size_t num_sessions = 1) {
  /* Create the Rpc and connect the session */
  AppContext context;
  client_connect_sessions(nexus, context, num_sessions);

  Rpc<IBTransport> *rpc = context.rpc;
  int session_num = context.session_num_arr[0];

  /* Send a message */
  size_t req_size = kAppMinMsgSize;
  MsgBuffer req_msgbuf = rpc->alloc_msg_buffer(req_size);
  ASSERT_NE(req_msgbuf.buf, nullptr);

  for (size_t i = 0; i < req_size; i++) {
    req_msgbuf.buf[i] = 'a';
  }
  req_msgbuf.buf[req_size - 1] = 0;

  test_printf("Client: Sending request of size %zu\n", req_size);
  int ret =
      rpc->enqueue_request(session_num, kAppReqType, &req_msgbuf, cont_func, 0);
  if (ret != 0) {
    test_printf("Client: enqueue_request error %s\n", std::strerror(ret));
  }
  ASSERT_EQ(ret, 0);

  client_wait_for_rpc_resps_or_timeout(nexus, context, 1);
  ASSERT_EQ(context.num_rpc_resps, 1);

  rpc->free_msg_buffer(req_msgbuf);

  /* Disconnect the session */
  rpc->destroy_session(session_num);
  rpc->run_event_loop_timeout(kAppEventLoopMs);

  /* Free resources */
  delete rpc;

  client_done = true;
}

TEST(OneLargeRpc, Foreground) {
  launch_server_client_threads(1, 0, one_large_rpc);
}

TEST(OneLargeRpc, Background) {
  /* One background thread */
  launch_server_client_threads(1, 1, one_large_rpc);
}

///
/// Test: Repeat: Multiple large Rpcs on one session, with random size
///
void multi_large_rpc_one_session(Nexus *nexus, size_t num_sessions = 1) {
  /* Create the Rpc and connect the session */
  AppContext context;
  client_connect_sessions(nexus, context, num_sessions);

  Rpc<IBTransport> *rpc = context.rpc;
  int session_num = context.session_num_arr[0];

  /* Pre-create MsgBuffers so we can test reuse and resizing */
  MsgBuffer req_msgbuf[Session::kSessionCredits];
  for (size_t i = 0; i < Session::kSessionCredits; i++) {
    req_msgbuf[i] = rpc->alloc_msg_buffer(Rpc<IBTransport>::kMaxMsgSize);
    ASSERT_NE(req_msgbuf[i].buf, nullptr);
  }

  for (size_t iter = 0; iter < 2; iter++) {
    context.num_rpc_resps = 0;

    /* Enqueue as many requests as one session allows */
    for (size_t i = 0; i < Session::kSessionCredits; i++) {
      size_t req_len = pick_large_msg_size((AppContext *)&context);
      rpc->resize_msg_buffer(&req_msgbuf[i], req_len);

      for (size_t j = 0; j < req_len; j++) {
        req_msgbuf[i].buf[j] = 'a' + ((i + j) % 26);
      }
      req_msgbuf[i].buf[req_len - 1] = 0;

      test_printf("Client: Sending request of length = %zu\n", req_len);
      int ret = rpc->enqueue_request(session_num, kAppReqType, &req_msgbuf[i],
                                     cont_func, 0);
      if (ret != 0) {
        test_printf("Client: enqueue_request error %s\n", std::strerror(ret));
      }
      ASSERT_EQ(ret, 0);
    }

    /* Try to enqueue one more request - this should fail */
    int ret = rpc->enqueue_request(session_num, kAppReqType, &req_msgbuf[0],
                                   cont_func, 0);
    ASSERT_NE(ret, 0);

    client_wait_for_rpc_resps_or_timeout(nexus, context,
                                         Session::kSessionCredits);
    ASSERT_EQ(context.num_rpc_resps, Session::kSessionCredits);
  }

  /* Free the request MsgBuffers */
  for (size_t i = 0; i < Session::kSessionCredits; i++) {
    rpc->free_msg_buffer(req_msgbuf[i]);
  }

  /* Disconnect the session */
  rpc->destroy_session(session_num);
  rpc->run_event_loop_timeout(kAppEventLoopMs);

  /* Free resources */
  delete rpc;

  client_done = true;
}

TEST(MultiLargeRpcOneSession, Foreground) {
  launch_server_client_threads(1, 0, multi_large_rpc_one_session);
}

TEST(MultiLargeRpcOneSession, Background) {
  /* 2 background threads */
  launch_server_client_threads(1, 2, multi_large_rpc_one_session);
}

///
/// Test: Repeat: Multiple large Rpcs on multiple sessions
///
void multi_large_rpc_multi_session(Nexus *nexus, size_t num_sessions) {
  /* Create the Rpc and connect the session */
  AppContext context;
  client_connect_sessions(nexus, context, num_sessions);

  Rpc<IBTransport> *rpc = context.rpc;
  int *session_num_arr = context.session_num_arr;

  /* Pre-create MsgBuffers so we can test reuse and resizing */
  size_t tot_reqs_per_iter = num_sessions * Session::kSessionCredits;
  MsgBuffer req_msgbuf[tot_reqs_per_iter];
  for (size_t req_i = 0; req_i < tot_reqs_per_iter; req_i++) {
    req_msgbuf[req_i] = rpc->alloc_msg_buffer(Rpc<IBTransport>::kMaxMsgSize);
    ASSERT_NE(req_msgbuf[req_i].buf, nullptr);
  }

  for (size_t iter = 0; iter < 5; iter++) {
    context.num_rpc_resps = 0;

    for (size_t sess_i = 0; sess_i < num_sessions; sess_i++) {
      /* Enqueue as many requests as this session allows */
      for (size_t crd_i = 0; crd_i < Session::kSessionCredits; crd_i++) {
        size_t req_i = (sess_i * Session::kSessionCredits) + crd_i;
        assert(req_i < tot_reqs_per_iter);

        size_t req_len = pick_large_msg_size((AppContext *)&context);
        rpc->resize_msg_buffer(&req_msgbuf[req_i], req_len);

        for (size_t j = 0; j < req_len; j++) {
          req_msgbuf[req_i].buf[j] = 'a' + ((req_i + j) % 26);
        }
        req_msgbuf[req_i].buf[req_len - 1] = 0;

        test_printf("Client: Sending request of length = %zu\n", req_len);

        int ret = rpc->enqueue_request(session_num_arr[sess_i], kAppReqType,
                                       &req_msgbuf[req_i], cont_func, 0);
        if (ret != 0) {
          test_printf("Client: enqueue_request error %s\n", std::strerror(ret));
        }
        ASSERT_EQ(ret, 0);
      }
    }

    client_wait_for_rpc_resps_or_timeout(nexus, context, tot_reqs_per_iter);
    ASSERT_EQ(context.num_rpc_resps, tot_reqs_per_iter);
  }

  /* Free the request MsgBuffers */
  for (size_t req_i = 0; req_i < tot_reqs_per_iter; req_i++) {
    rpc->free_msg_buffer(req_msgbuf[req_i]);
  }

  /* Disconnect the sessions */
  for (size_t sess_i = 0; sess_i < num_sessions; sess_i++) {
    rpc->destroy_session(session_num_arr[sess_i]);
  }

  rpc->run_event_loop_timeout(kAppEventLoopMs);

  /* Free resources */
  delete rpc;

  client_done = true;
}

TEST(MultiLargeRpcMultiSession, Foreground) {
  assert(!kDatapathVerbose);
  /* Use enough sessions to exceed the Rpc's unexpected window */
  size_t num_sessions =
      (Rpc<IBTransport>::kRpcUnexpPktWindow / Session::kSessionCredits) + 2;
  launch_server_client_threads(num_sessions, 0, multi_large_rpc_multi_session);
}

TEST(MultiLargeRpcMultiSession, Background) {
  assert(!kDatapathVerbose);
  /* Use enough sessions to exceed the Rpc's unexpected window */
  size_t num_sessions =
      (Rpc<IBTransport>::kRpcUnexpPktWindow / Session::kSessionCredits) + 2;
  /* 3 background threads */
  launch_server_client_threads(num_sessions, 3, multi_large_rpc_multi_session);
}

///
/// Test: Repeat: Multiple large Rpcs on multiple sessions, trying to force
/// a memory leak. This test takes a long time so it's disabled by default.
///
void memory_leak(Nexus *nexus, size_t num_sessions) {
  /* Create the Rpc and connect the session */
  AppContext context;
  client_connect_sessions(nexus, context, num_sessions);

  Rpc<IBTransport> *rpc = context.rpc;
  int *session_num_arr = context.session_num_arr;

  /* Run many iterations to stress memory leaks */
  for (size_t iter = 0; iter < 500; iter++) {
    test_printf("Client: Iteration %zu\n", iter);

    /* Create new MsgBuffers in each iteration to stress leaks */
    size_t tot_reqs_per_iter = num_sessions * Session::kSessionCredits;
    MsgBuffer req_msgbuf[tot_reqs_per_iter];
    for (size_t req_i = 0; req_i < tot_reqs_per_iter; req_i++) {
      req_msgbuf[req_i] = rpc->alloc_msg_buffer(Rpc<IBTransport>::kMaxMsgSize);
      ASSERT_NE(req_msgbuf[req_i].buf, nullptr);
    }

    context.num_rpc_resps = 0;

    for (size_t sess_i = 0; sess_i < num_sessions; sess_i++) {
      /* Enqueue as many requests as this session allows */
      for (size_t crd_i = 0; crd_i < Session::kSessionCredits; crd_i++) {
        size_t req_i = (sess_i * Session::kSessionCredits) + crd_i;
        assert(req_i < tot_reqs_per_iter);

        size_t req_len = pick_large_msg_size((AppContext *)&context);
        rpc->resize_msg_buffer(&req_msgbuf[req_i], req_len);

        for (size_t j = 0; j < req_len; j++) {
          req_msgbuf[req_i].buf[j] = 'a' + ((req_i + j) % 26);
        }
        req_msgbuf[req_i].buf[req_len - 1] = 0;

        test_printf("Client: Iter %zu: Sending request of length = %zu\n", iter,
                    req_len);

        int ret = rpc->enqueue_request(session_num_arr[sess_i], kAppReqType,
                                       &req_msgbuf[req_i], cont_func, 0);
        if (ret != 0) {
          test_printf("Client: enqueue_request error %s\n", std::strerror(ret));
        }
        ASSERT_EQ(ret, 0);
      }
    }

    /* Run the event loop for up to kAppMaxEventLoopMs milliseconds */
    client_wait_for_rpc_resps_or_timeout(nexus, context, tot_reqs_per_iter);
    ASSERT_EQ(context.num_rpc_resps, tot_reqs_per_iter);

    /* Free the request MsgBuffers */
    for (size_t req_i = 0; req_i < tot_reqs_per_iter; req_i++) {
      rpc->free_msg_buffer(req_msgbuf[req_i]);
    }
  }

  /* Disconnect the sessions */
  for (size_t sess_i = 0; sess_i < num_sessions; sess_i++) {
    rpc->destroy_session(session_num_arr[sess_i]);
  }

  rpc->run_event_loop_timeout(kAppEventLoopMs);

  /* Free resources */
  delete rpc;

  client_done = true;
}

TEST(DISABLED_MemoryLeak, Foreground) {
  assert(!kDatapathVerbose);
  /* Use enough sessions to exceed the Rpc's unexpected window */
  size_t num_sessions =
      (Rpc<IBTransport>::kRpcUnexpPktWindow / Session::kSessionCredits) + 2;
  launch_server_client_threads(num_sessions, 0, memory_leak);
}

TEST(DISABLED_MemoryLeak, Background) {
  assert(!kDatapathVerbose);
  /* Use enough sessions to exceed the Rpc's unexpected window */
  size_t num_sessions =
      (Rpc<IBTransport>::kRpcUnexpPktWindow / Session::kSessionCredits) + 2;
  /* 2 background threads */
  launch_server_client_threads(num_sessions, 2, memory_leak);
}

int main(int argc, char **argv) {
  Nexus::get_hostname(local_hostname);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}