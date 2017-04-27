/*
 * @file rpc_fault_inject.cc
 * @brief Functions to allow users to inject faults into ERpc
 */
#include "rpc.h"

namespace ERpc {

template <class TTr>
void Rpc<TTr>::fault_inject_check_ok() const {
  if (!kFaultInjection) {
    throw std::runtime_error("eRPC Rpc: Fault injection is disabled.");
  }

  if (!in_creator()) {
    throw std::runtime_error(
        "eRPC Rpc: Non-creator threads cannot inject faults.");
  }
}

template <class TTr>
void Rpc<TTr>::fault_inject_resolve_server_rinfo_st() {
  fault_inject_check_ok();
  faults.resolve_server_rinfo = true;
}

template <class TTr>
void Rpc<TTr>::fault_inject_drop_tx_local_st(size_t pkt_countdown) {
  fault_inject_check_ok();
  faults.drop_tx_local = true;
  faults.drop_tx_local_countdown = pkt_countdown;
}

template <class TTr>
void Rpc<TTr>::fault_inject_drop_tx_remote_st(int session_num,
                                              size_t pkt_countdown) {
  fault_inject_check_ok();
  assert(is_usr_session_num_in_range(session_num));

  // We don't grab session lock because, for this session, other management ops
  // are handled by this thread, and we don't care about datapath operations
  Session *session = session_vec[static_cast<size_t>(session_num)];
  assert(session != nullptr);
  assert(session->is_connected() && session->is_client());

  erpc_dprintf(
      "eRPC Rpc %u: Sending drop-TX-remote fault (countdown = %zu) "
      "for session %u to [%s, %u].\n",
      rpc_id, pkt_countdown, session->local_session_num,
      session->server.hostname, session->server.rpc_id);

  enqueue_sm_req_st(session, SmPktType::kFaultDropTxRemote, pkt_countdown);
}

template <class TTr>
void Rpc<TTr>::fault_inject_reset_remote_epeer_st(int session_num) {
  fault_inject_check_ok();
  assert(is_usr_session_num_in_range(session_num));

  // We don't grab session lock because, for this session, other management ops
  // are handled by this thread, and we don't care about datapath operations
  Session *session = session_vec[static_cast<size_t>(session_num)];
  assert(session != nullptr);
  assert(session->is_client() && session->is_connected());

  erpc_dprintf(
      "eRPC Rpc %u: Sending reset-remote-peer fault for session %u "
      "to [%s, %u].\n",
      rpc_id, session->local_session_num, session->server.hostname,
      session->server.rpc_id);

  enqueue_sm_req_st(session, SmPktType::kFaultResetPeerReq);
}

}  // End ERpc