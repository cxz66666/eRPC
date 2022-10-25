#ifndef PROFILE_SIMPLE_H
#define PROFILE_SIMPLE_H

#include "large_rpc_tput.h"

void connect_sessions_func_simple(AppContext *c)
{
    // All non-zero processes create one session to process #0
    if (FLAGS_process_id == 0)
        return;

    size_t rem_tid = c->thread_id_;

    c->session_num_vec_.resize(1);

    printf(
        "large_rpc_tput: Thread %zu: Creating 1 session to proc 0, thread %zu.\n",
        c->thread_id_, rem_tid);

    c->session_num_vec_[0] =
        c->rpc_->create_session(erpc::get_uri_for_process(0), rem_tid);
    erpc::rt_assert(c->session_num_vec_[0] >= 0, "create_session() failed");

    while (c->num_sm_resps_ != 1)
    {
        c->rpc_->run_event_loop(200); // 200 milliseconds
        if (ctrl_c_pressed == 1)
            return;
    }
}

#endif
