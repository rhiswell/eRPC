#include <gflags/gflags.h>
#include <signal.h>
#include <cstring>
#include <pcg/pcg_random.hpp>
#include "../apps_common.h"
#include "pmica.h"
#include "rpc.h"
#include "util/autorun_helpers.h"
#include "util/latency.h"
#include "util/math_utils.h"
#include "util/numautils.h"

static constexpr size_t kAppEvLoopMs = 1000;     // Duration of event loop
static constexpr bool kAppVerbose = false;       // Print debug info on datapath
static constexpr double kAppLatFac = 10.0;       // Precision factor for latency
static constexpr size_t kAppReqType = 1;         // eRPC request type
static constexpr size_t kAppMaxWindowSize = 32;  // Max pending reqs per client
static constexpr double kAppMicaOverhead = 0.2;  // Extra bucket fraction

// Maximum requests processed by server before issuing a response
static constexpr size_t kAppMaxServerBatch = 16;

DEFINE_string(pmem_file, "/dev/dax12.0", "Persistent memory file path");
DEFINE_uint64(keys_per_server_thread, 1, "Keys in each server partition");
DEFINE_uint64(num_server_threads, 1, "Number of threads at the server machine");
DEFINE_uint64(num_client_threads, 1, "Number of threads per client machine");
DEFINE_uint64(window_size, 1, "Outstanding requests per client");
DEFINE_string(workload, "set", "set/get/5050");

volatile sig_atomic_t ctrl_c_pressed = 0;
void ctrl_c_handler(int) { ctrl_c_pressed = 1; }

// MICA's ``small'' workload: 16-byte keys and 64-byte values
class Key {
 public:
  size_t key_frag[2];
  bool operator==(const Key &rhs) const {
    return memcmp(this, &rhs, sizeof(Key)) == 0;
  }
  bool operator!=(const Key &rhs) const {
    return memcmp(this, &rhs, sizeof(Key)) != 0;
  }
  Key() { memset(key_frag, 0, sizeof(Key)); }
};

class Value {
 public:
  size_t val_frag[4];
  Value() { memset(val_frag, 0, sizeof(Value)); }
};
typedef pmica::HashMap<Key, Value> HashMap;

enum class Result : size_t { kGetFail = 1, kSetSuccess, kSetFail };

// We use response size to distinguish between response types
static_assert(sizeof(Result) < sizeof(Value), "");

enum class Workload { kGets, kSets, k5050 };

/// Given a random number \p rand, return a random number
static inline uint64_t fastrange64(uint64_t rand, uint64_t n) {
  return static_cast<uint64_t>(
      static_cast<__uint128_t>(rand) * static_cast<__uint128_t>(n) >> 64);
}

class ServerContext : public BasicAppContext {
 public:
  size_t num_reqs_tot = 0;  // Reqs for which the handler has been called
  HashMap *hashmap;

  // Batch info
  size_t num_reqs_in_batch = 0;
  erpc::ReqHandle *req_handle_arr[kAppMaxServerBatch];
  bool is_set_arr[kAppMaxServerBatch];
  Key *key_ptr_arr[kAppMaxServerBatch];
  Value *val_ptr_arr[kAppMaxServerBatch];
  size_t keyhash_arr[kAppMaxServerBatch];

  struct {
    size_t num_resps_tot = 0;  // Total responses sent
  } stats;

  void reset_stats() { memset(&stats, 0, sizeof(stats)); }
};

class ClientContext : public BasicAppContext {
 public:
  size_t num_resps = 0;
  size_t thread_id;
  Workload workload;
  pcg64_fast pcg;

  size_t start_tsc[kAppMaxWindowSize];
  Key key_arr[kAppMaxWindowSize];
  bool is_set_arr[kAppMaxWindowSize];
  erpc::MsgBuffer req_msgbuf[kAppMaxWindowSize], resp_msgbuf[kAppMaxWindowSize];

  struct {
    size_t num_get_reqs = 0;
    size_t num_get_success = 0;
    size_t num_set_reqs = 0;
    size_t num_set_success = 0;
  } stats;

  void reset_stats() { memset(&stats, 0, sizeof(stats)); }
  std::string get_stats_string() {
    std::ostringstream ret;
    ret << "[get_reqs " << stats.num_get_reqs << ", get_success "
        << stats.num_get_success << ", set_reqs " << stats.num_set_reqs
        << ", set_success " << stats.num_set_success << "]";
    return ret.str();
  }

  erpc::Latency latency;
  ~ClientContext() {}
};

// Do hash table operations and send responses for all requests in the batch.
// This must reset num_reqs_in_batch.
inline void drain_batch(ServerContext *c) {
  assert(c->num_reqs_in_batch > 0);
  bool success_arr[kAppMaxServerBatch];
  c->hashmap->batch_op_drain_helper(
      c->is_set_arr, c->keyhash_arr, const_cast<const Key **>(c->key_ptr_arr),
      c->val_ptr_arr, success_arr, c->num_reqs_in_batch);

  for (size_t i = 0; i < c->num_reqs_in_batch; i++) {
    erpc::ReqHandle *req_handle = c->req_handle_arr[i];
    req_handle->prealloc_used = true;
    erpc::MsgBuffer &resp = req_handle->pre_resp_msgbuf;

    if (c->is_set_arr[i]) {
      // SET request
      c->rpc->resize_msg_buffer(&resp, sizeof(Result));
      *reinterpret_cast<Result *>(resp.buf) =
          success_arr[i] ? Result::kSetSuccess : Result::kSetFail;
    } else {
      // GET request
      if (!success_arr[i]) {
        c->rpc->resize_msg_buffer(&resp, sizeof(Result));
        *reinterpret_cast<Result *>(resp.buf) = Result::kGetFail;
      }
    }

    c->rpc->enqueue_response(req_handle);
  }

  c->stats.num_resps_tot += c->num_reqs_in_batch;
  c->num_reqs_in_batch = 0;
}

void req_handler(erpc::ReqHandle *req_handle, void *_context) {
  auto *c = static_cast<ServerContext *>(_context);

  const erpc::MsgBuffer *req = req_handle->get_req_msgbuf();
  size_t req_size = req->get_data_size();

  req_handle->prealloc_used = true;
  erpc::MsgBuffer &resp = req_handle->pre_resp_msgbuf;
  c->rpc->resize_msg_buffer(&resp, sizeof(Value));  // sizeof(Result) is smaller

  const size_t batch_i = c->num_reqs_in_batch;

  // Common for both GETs and SETs
  Key *key = reinterpret_cast<Key *>(req->buf);

  c->req_handle_arr[batch_i] = req_handle;
  c->key_ptr_arr[batch_i] = key;
  c->keyhash_arr[batch_i] = c->hashmap->get_hash(key);
  c->hashmap->prefetch(c->keyhash_arr[batch_i]);

  if (req_size == sizeof(Key)) {
    if (kAppVerbose) printf("Thread %zu: received GET request\n", c->thread_id);
    // GET request
    c->is_set_arr[batch_i] = false;
    Value *value = reinterpret_cast<Value *>(resp.buf);
    c->val_ptr_arr[batch_i] = value;
  } else if (req_size == sizeof(Key) + sizeof(Value)) {
    if (kAppVerbose) printf("Thread %zu: received SET request\n", c->thread_id);
    // PUT request
    c->is_set_arr[batch_i] = true;
    Value *value = reinterpret_cast<Value *>(req->buf + sizeof(Key));
    c->val_ptr_arr[batch_i] = value;
  } else {
    assert(false);
  }

  // Tracking
  c->num_reqs_tot++;
  c->num_reqs_in_batch++;
  if (c->num_reqs_in_batch == kAppMaxServerBatch) drain_batch(c);
}

// Populate a map with keys {1, ..., FLAGS_keys_per_server_thread}
size_t populate(HashMap *hashmap, size_t thread_id) {
  bool is_set_arr[pmica::kMaxBatchSize];
  Key key_arr[pmica::kMaxBatchSize];
  Value val_arr[pmica::kMaxBatchSize];
  Key *key_ptr_arr[pmica::kMaxBatchSize];
  Value *val_ptr_arr[pmica::kMaxBatchSize];
  bool success_arr[pmica::kMaxBatchSize];

  size_t num_success = 0;

  for (size_t i = 0; i < pmica::kMaxBatchSize; i++) {
    key_ptr_arr[i] = &key_arr[i];
    val_ptr_arr[i] = &val_arr[i];
  }

  const size_t num_keys_to_insert =
      erpc::round_up<pmica::kMaxBatchSize>(FLAGS_keys_per_server_thread);
  size_t progress_console_lim = num_keys_to_insert / 10;

  for (size_t i = 1; i <= num_keys_to_insert; i += pmica::kMaxBatchSize) {
    for (size_t j = 0; j < pmica::kMaxBatchSize; j++) {
      is_set_arr[j] = true;
      key_arr[j].key_frag[0] = i + j;
      val_arr[j].val_frag[0] = i + j;
    }

    hashmap->batch_op_drain(is_set_arr, const_cast<const Key **>(key_ptr_arr),
                            val_ptr_arr, success_arr, pmica::kMaxBatchSize);

    if (i >= progress_console_lim) {
      printf("thread %zu: %.2f percent done\n", thread_id,
             i * 1.0 / num_keys_to_insert);
      progress_console_lim += num_keys_to_insert / 10;
    }

    for (size_t j = 0; j < pmica::kMaxBatchSize; j++) {
      num_success += success_arr[j];
      if (!success_arr[j]) return num_success;
    }
  }

  return num_keys_to_insert;  // All keys were added
}

void server_func(erpc::Nexus *nexus, size_t thread_id) {
  std::vector<size_t> port_vec = flags_get_numa_ports(FLAGS_numa_node);

  const size_t bytes_per_map = HashMap::get_required_bytes(
      FLAGS_keys_per_server_thread, kAppMicaOverhead);

  ServerContext c;
  c.hashmap = new HashMap(FLAGS_pmem_file, thread_id * bytes_per_map,
                          FLAGS_keys_per_server_thread, kAppMicaOverhead);
  const size_t num_keys_inserted = populate(c.hashmap, thread_id);
  printf("thread %zu: %.2f fraction of keys inserted\n", thread_id,
         num_keys_inserted * 1.0 / FLAGS_keys_per_server_thread);

  erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(&c), thread_id,
                                  basic_sm_handler, port_vec.at(0));
  c.rpc = &rpc;
  const double freq_ghz = c.rpc->get_freq_ghz();
  const size_t tsc_per_sec = erpc::ms_to_cycles(1000, freq_ghz);

  while (true) {
    c.stats.num_resps_tot = 0;
    size_t start_tsc = erpc::rdtsc();

    while (erpc::rdtsc() - start_tsc <= tsc_per_sec) {
      size_t num_reqs_tot_start = c.num_reqs_tot;
      rpc.run_event_loop_once();

      // If no new requests were received in this iteration of the event loop,
      // and we have responses to send, send them now.
      if (c.num_reqs_tot == num_reqs_tot_start && c.num_reqs_in_batch > 0) {
        drain_batch(&c);
      }
    }

    const double seconds = erpc::to_sec(erpc::rdtsc() - start_tsc, freq_ghz);
    printf("thread %zu: %.2f M/s. rx batch %.2f, tx batch %.2f\n", thread_id,
           c.stats.num_resps_tot / (seconds * Mi(1)), c.rpc->get_avg_rx_batch(),
           c.rpc->get_avg_tx_batch());

    c.rpc->reset_dpath_stats();
    c.stats.num_resps_tot = 0;

    if (ctrl_c_pressed == 1) break;
  }

  delete c.hashmap;
}

void app_cont_func(erpc::RespHandle *, void *, size_t);
inline void send_req(ClientContext &c, size_t ws_i) {
  c.start_tsc[ws_i] = erpc::rdtsc();

  erpc::MsgBuffer &req = c.req_msgbuf[ws_i];
  Key *key = reinterpret_cast<Key *>(req.buf);
  Value *value = reinterpret_cast<Value *>(req.buf + sizeof(Key));

  bool &is_set = c.is_set_arr[ws_i];
  switch (c.workload) {
    case Workload::kGets: is_set = false; break;
    case Workload::kSets: is_set = true; break;
    case Workload::k5050: is_set = c.pcg() % 2 == 0; break;
  }
  is_set ? c.stats.num_set_reqs++ : c.stats.num_get_reqs++;
  if (kAppVerbose) {
    printf("Thread %zu: sending %s request. Window slot %zu\n", c.thread_id,
           is_set ? "SET" : "GET", ws_i);
  }

  key->key_frag[0] = 1 + fastrange64(c.pcg(), FLAGS_keys_per_server_thread);
  value->val_frag[0] = key->key_frag[0];
  c.key_arr[ws_i] = *key;

  size_t req_size = is_set ? sizeof(Key) + sizeof(Value) : sizeof(Key);
  c.rpc->resize_msg_buffer(&req, req_size);

  // Send request to a random server
  c.rpc->enqueue_request(c.fast_get_rand_session_num(), kAppReqType,
                         &c.req_msgbuf[ws_i], &c.resp_msgbuf[ws_i],
                         app_cont_func, ws_i);
}

void app_cont_func(erpc::RespHandle *resp_handle, void *_context, size_t ws_i) {
  const erpc::MsgBuffer *resp = resp_handle->get_resp_msgbuf();
  _unused(resp);

  auto *c = static_cast<ClientContext *>(_context);
  if (c->is_set_arr[ws_i]) {
    // SET response
    assert(resp->get_data_size() == sizeof(Result));
    auto result = *reinterpret_cast<Result *>(resp->buf);
    if (result == Result::kSetSuccess) c->stats.num_set_success++;
  } else {
    // GET response
    assert(resp->get_data_size() == sizeof(Value) ||
           resp->get_data_size() == sizeof(Result));
    if (resp->get_data_size() == sizeof(Value)) {
      Value *value = reinterpret_cast<Value *>(resp->buf);
      _unused(value);
      assert(value->val_frag[0] == c->key_arr[ws_i].key_frag[0]);
      c->stats.num_get_success++;
    }
  }

  if (kAppVerbose) {
    printf("Thread %zu: received %s response. Window slot %zu\n", c->thread_id,
           c->is_set_arr[ws_i] ? "SET" : "GET", ws_i);
  }

  c->rpc->release_response(resp_handle);

  double req_lat_us =
      erpc::to_usec(erpc::rdtsc() - c->start_tsc[ws_i], c->rpc->get_freq_ghz());
  c->latency.update(static_cast<size_t>(req_lat_us * kAppLatFac));
  c->num_resps++;

  send_req(*c, ws_i);  // Clock the used window slot
}

// Connect this client thread to all server threads
void create_sessions(ClientContext &c) {
  std::string server_uri = erpc::get_uri_for_process(0);
  if (FLAGS_sm_verbose == 1) {
    printf("Process %zu: Creating %zu sessions to %s.\n", FLAGS_process_id,
           FLAGS_num_server_threads, server_uri.c_str());
  }

  for (size_t i = 0; i < FLAGS_num_server_threads; i++) {
    int session_num = c.rpc->create_session(server_uri, i);
    erpc::rt_assert(session_num >= 0, "Failed to create session");
    c.session_num_vec.push_back(session_num);
  }

  while (c.num_sm_resps != FLAGS_num_server_threads) {
    c.rpc->run_event_loop(kAppEvLoopMs);
    if (unlikely(ctrl_c_pressed == 1)) return;
  }
}

void client_func(erpc::Nexus *nexus, size_t thread_id) {
  std::vector<size_t> port_vec = flags_get_numa_ports(FLAGS_numa_node);
  uint8_t phy_port = port_vec.at(0);

  ClientContext c;
  erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(&c), thread_id,
                                  basic_sm_handler, phy_port);
  if (FLAGS_workload == "set") c.workload = Workload::kSets;
  if (FLAGS_workload == "get") c.workload = Workload::kGets;
  if (FLAGS_workload == "5050") c.workload = Workload::k5050;
  c.pcg = pcg64_fast(pcg_extras::seed_seq_from<std::random_device>{});

  rpc.retry_connect_on_invalid_rpc_id = true;
  c.rpc = &rpc;
  c.thread_id = thread_id;

  create_sessions(c);

  printf("Process %zu, thread %zu: Connected. Starting work.\n",
         FLAGS_process_id, thread_id);
  if (thread_id == 0) {
    printf("thread_id: median_us 5th_us 99th_us 999th_us Mops. Stats.\n");
  }

  for (size_t i = 0; i < FLAGS_window_size; i++) {
    c.req_msgbuf[i] = rpc.alloc_msg_buffer_or_die(sizeof(Key) + sizeof(Value));
    c.resp_msgbuf[i] = rpc.alloc_msg_buffer_or_die(sizeof(Key) + sizeof(Value));
    send_req(c, i);
  }

  for (size_t i = 0; i < FLAGS_test_ms; i += 1000) {
    struct timespec start;
    clock_gettime(CLOCK_REALTIME, &start);

    rpc.run_event_loop(kAppEvLoopMs);  // 1 second
    if (ctrl_c_pressed == 1) break;

    double seconds = erpc::sec_since(start);
    printf("%zu: %.1f %.1f %.1f %.1f %.2f. %s\n", thread_id,
           c.latency.perc(.5) / kAppLatFac, c.latency.perc(.05) / kAppLatFac,
           c.latency.perc(.99) / kAppLatFac, c.latency.perc(.999) / kAppLatFac,
           c.num_resps / (seconds * Mi(1)), c.get_stats_string().c_str());

    c.num_resps = 0;
    c.latency.reset();
    c.reset_stats();
  }
}

int main(int argc, char **argv) {
  signal(SIGINT, ctrl_c_handler);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  static_assert(sizeof(Key) + sizeof(Value) <= erpc::CTransport::kMTU,
                "KV too large");

  erpc::rt_assert(FLAGS_numa_node <= 1, "Invalid NUMA node");
  erpc::rt_assert(FLAGS_window_size <= kAppMaxWindowSize, "Window too large");

  erpc::Nexus nexus(erpc::get_uri_for_process(FLAGS_process_id),
                    FLAGS_numa_node, 0);
  nexus.register_req_func(kAppReqType, req_handler);

  size_t num_threads = FLAGS_process_id == 0 ? FLAGS_num_server_threads
                                             : FLAGS_num_client_threads;
  std::vector<std::thread> threads(num_threads);

  for (size_t i = 0; i < num_threads; i++) {
    threads[i] = std::thread(FLAGS_process_id == 0 ? server_func : client_func,
                             &nexus, i);
    erpc::bind_to_core(threads[i], FLAGS_numa_node, i);
  }

  for (size_t i = 0; i < num_threads; i++) threads[i].join();
}
