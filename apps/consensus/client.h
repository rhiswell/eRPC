/**
 * @file client.h
 * @brief The client for the replicated service
 */

#include "consensus.h"

#ifndef CLIENT_H
#define CLIENT_H

void client_cont(ERpc::RespHandle *, void *, size_t);  // Forward declaration

// Change the leader to a different Raft server that we are connected to
void change_leader_to_any(AppContext *c) {
  size_t cur_leader_idx = c->client.leader_idx;
  printf("consensus: Client change_leader_to_any() from current leader %zu.\n",
         c->client.leader_idx);

  // Pick the next session to a Raft server that is not disconnected
  for (size_t i = 1; i < FLAGS_num_raft_servers; i++) {
    size_t next_leader_idx = (cur_leader_idx + i) % FLAGS_num_raft_servers;
    if (!c->conn_vec[next_leader_idx].disconnected) {
      c->client.leader_idx = next_leader_idx;

      printf("consensus: Client changed leader view to %zu.\n",
             c->client.leader_idx);
      return;
    }
  }

  printf(
      "consensus: Client failed to change leader to any Raft server. "
      "Exiting.\n");
  exit(0);
}

// Change the leader to a server with the given node ID
bool change_leader_to_node(AppContext *c, int node_id) {
  // Pick the next session to a Raft server that is not disconnected
  for (size_t i = 0; i < FLAGS_num_raft_servers; i++) {
    std::string node_i_hostname = get_hostname_for_machine(i);
    int node_i_id = get_raft_node_id_from_hostname(node_i_hostname);

    if (node_i_id == node_id) {
      if (c->conn_vec[i].disconnected) {
        // We're being redirected to a failed Raft server
        return false;
      }

      c->client.leader_idx = i;
      return true;
    }
  }

  printf("consensus: Client could not find node %d. Exiting.\n", node_id);
  exit(0);
}

void send_req_one(AppContext *c) {
  assert(c != nullptr && c->check_magic());
  c->client.req_start_tsc = ERpc::rdtsc();

  // Format the client's PUT request. Key and value are identical.
  auto *req = reinterpret_cast<client_req_t *>(c->client.req_msgbuf.buf);
  req->key[0] = c->client.last_key;
  req->value[0] = c->client.last_key;
  c->client.last_key++;
  if (unlikely(c->client.last_key == kAppNumKeys)) c->client.last_key = 0;

  if (kAppVerbose) {
    printf("consensus: Client sending request %s to leader index %zu [%s].\n",
           req->to_string().c_str(), c->client.leader_idx,
           ERpc::get_formatted_time().c_str());
  }

  connection_t &conn = c->conn_vec[c->client.leader_idx];
  int ret = c->rpc->enqueue_request(
      conn.session_num, static_cast<uint8_t>(ReqType::kClientReq),
      &c->client.req_msgbuf, &c->client.resp_msgbuf, client_cont, 0);  // 0 tag
  assert(ret == 0);
  _unused(ret);
}

void client_cont(ERpc::RespHandle *resp_handle, void *_context, size_t) {
  assert(resp_handle != nullptr && _context != nullptr);
  auto *c = static_cast<AppContext *>(_context);
  assert(c->check_magic());

  double latency_us = ERpc::to_usec(ERpc::rdtsc() - c->client.req_start_tsc,
                                    c->rpc->get_freq_ghz());
  c->client.req_us_vec.push_back(latency_us);
  c->client.num_resps++;

  if (c->client.num_resps == 10000) {
    // At this point, there is no request outstanding, so long compute is OK
    auto &lat_vec = c->client.req_us_vec;
    std::sort(lat_vec.begin(), lat_vec.end());

    double us_min = lat_vec.at(0);
    double us_median = lat_vec.at(lat_vec.size() / 2);
    double us_99 = lat_vec.at(lat_vec.size() * .99);
    double us_999 = lat_vec.at(lat_vec.size() * .999);
    double us_max = lat_vec.at(lat_vec.size() - 1);

    printf(
        "consensus: Latency us = "
        "{%.2f min, %.2f 50, %.2f 99, %.2f 99.9, %.2f max}. "
        "Request window = %zu (best 1). Inline size = %zu (best 120).\n",
        us_min, us_median, us_99, us_999, us_max,
        ERpc::Session::kSessionReqWindow, ERpc::IBTransport::kMaxInline);
    c->client.num_resps = 0;
    c->client.req_us_vec.clear();
  }

  if (likely(c->client.resp_msgbuf.get_data_size() > 0)) {
    // The RPC was successful
    auto *client_resp =
        reinterpret_cast<client_resp_t *>(c->client.resp_msgbuf.buf);

    if (kAppVerbose) {
      printf("consensus: Client received resp %s [%s].\n",
             client_resp->to_string().c_str(),
             ERpc::get_formatted_time().c_str());
    }

    switch (client_resp->resp_type) {
      case ClientRespType::kSuccess: {
        break;
      }

      case ClientRespType::kFailRedirect: {
        printf(
            "consensus: Client request to server %zu failed with code = "
            "redirect. Trying to change leader to %s.\n",
            c->client.leader_idx,
            node_id_to_name_map[client_resp->leader_node_id].c_str());

        bool success = change_leader_to_node(c, client_resp->leader_node_id);
        if (!success) {
          printf(
              "consensus: Client failed to change leader to %s. "
              "Retrying to current leader %zu after 200 ms.\n",
              node_id_to_name_map[client_resp->leader_node_id].c_str(),
              c->client.leader_idx);
          usleep(200000);
        }
        break;
      }

      case ClientRespType::kFailTryAgain: {
        printf(
            "consensus: Client request to server %zu failed with code = "
            "try again. Trying again after 200 ms.\n",
            c->client.leader_idx);
        usleep(200000);
        break;
      }
    }
  } else {
    // This is a continuation-with-failure
    printf("consensus: Client RPC to server %zu failed to complete [%s].\n",
           c->client.leader_idx, ERpc::get_formatted_time().c_str());
    change_leader_to_any(c);
  }

  c->rpc->release_response(resp_handle);
  send_req_one(c);
}

void client_func(size_t thread_id, ERpc::Nexus *nexus, AppContext *c) {
  assert(nexus != nullptr && c != nullptr);
  assert(c->conn_vec.size() == FLAGS_num_raft_servers);
  assert(thread_id <= ERpc::kMaxRpcId);

  c->client.thread_id = thread_id;
  c->client.leader_idx = 0;  // Start with leader = 0

  c->rpc = new ERpc::Rpc<ERpc::IBTransport>(
      nexus, static_cast<void *>(c), static_cast<uint8_t>(thread_id),
      sm_handler, kAppPhyPort, kAppNumaNode);
  c->rpc->retry_connect_on_invalid_rpc_id = true;

  // Pre-allocate MsgBuffers
  c->client.req_msgbuf = c->rpc->alloc_msg_buffer(sizeof(client_req_t));
  assert(c->client.req_msgbuf.buf != nullptr);

  c->client.resp_msgbuf = c->rpc->alloc_msg_buffer(sizeof(client_resp_t));
  assert(c->client.resp_msgbuf.buf != nullptr);

  // Raft client: Create session to each Raft server
  for (size_t i = 0; i < FLAGS_num_raft_servers; i++) {
    std::string hostname = get_hostname_for_machine(i);

    printf("consensus: Client %zu creating session to %s, index = %zu.\n",
           thread_id, hostname.c_str(), i);

    c->conn_vec[i].session_idx = i;
    c->conn_vec[i].session_num =
        c->rpc->create_session(hostname, 0, kAppPhyPort);
    assert(c->conn_vec[i].session_num >= 0);
  }

  while (c->num_sm_resps != FLAGS_num_raft_servers) {
    c->rpc->run_event_loop(200);  // 200 ms
    if (ctrl_c_pressed == 1) {
      delete c->rpc;
      exit(0);
    }
  }

  printf("consensus: Client %zu connected to all servers. Sending requests.\n",
         thread_id);

  send_req_one(c);
  while (ctrl_c_pressed == 0) {
    c->rpc->run_event_loop(200);  // 200 milliseconds
  }

  delete c->rpc;
}

#endif
