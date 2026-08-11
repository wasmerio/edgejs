#include "edge_cluster_wasix.h"

#if defined(__wasi__)

#include <cstdlib>
#include <string>

#include <uv.h>

#include "edge_module_loader.h"

namespace {

// Message acts exchanged between the worker strategy and the primary broker
// below. Defined once and passed into both scripts so they cannot drift.
constexpr const char kReserveAct[] = "edge:wasix-reserve-port";
constexpr const char kReportAct[] = "edge:wasix-report-port";

// Invoked with (cluster, net, dgram, sendHelper, UV_TCP_REUSEPORT,
// UV_UDP_REUSEPORT, RESERVE_ACT, REPORT_ACT). Kept as an embedded script
// instead of a lib/ module because the Node lib/ tree stays byte-identical to
// upstream; the behavior is edge/WASIX-specific.
//
// TCP and UDP port listens replace queryServer with a round trip to the broker
// in the primary (see the broker script below); the worker then binds its own
// SO_REUSEPORT socket rather than receiving a descriptor, and reports the
// 'listening' act for the primary's bookkeeping (worker state, cluster
// 'listening' event). fd and pipe listens keep the upstream path.
//
// The round trip settles the two things a worker cannot decide alone:
//
//   * the port, when the caller asked for 0. "Any port" only holds a cluster
//     together if every worker lands on the same one, which upstream gets for
//     free by binding once in the primary and sharing the descriptor. WASI has
//     no SCM_RIGHTS, so instead the broker arbitrates the *number*: one worker
//     is elected to bind ephemerally and report what it got, and the rest are
//     told to bind that.
//   * the server data (obj._getServerData/_setServerData, i.e. TLS session
//     ticket keys), which has to come from one place for all workers or a
//     session only resumes on the worker that happened to issue it.
//
// Keeping the round trip even for a fixed port costs nothing relative to
// upstream, which always makes one; only the descriptor passing is dropped.
//
// Remaining deviation vs upstream: UDP datagrams are distributed by the
// kernel's reuseport source hash (flows pin to a worker) instead of
// shared-socket delivery.
constexpr const char kWorkerInstallScript[] = R"JS(
(function installWasixClusterReusePort(cluster, net, dgram, sendHelper,
                                       UV_TCP_REUSEPORT, UV_UDP_REUSEPORT,
                                       RESERVE_ACT, REPORT_ACT) {
  'use strict';

  if (!cluster.isWorker || typeof cluster._getServer !== 'function')
    return;

  const UV_EINVAL = -22;
  const originalGetServer = cluster._getServer;
  const ownHandles = new Set();
  // Per-worker sequence per listen target, mirroring the `index` that
  // lib/internal/cluster/child.js attaches to queryServer.
  const indexes = new Map();
  let disconnectHookInstalled = false;

  cluster._getServer = function(obj, options, cb) {
    const isTcp = options.addressType === 4 || options.addressType === 6;
    const isUdp = options.addressType === 'udp4' ||
                  options.addressType === 'udp6';
    // dgram passes the raw bind() arguments through: port can be null,
    // undefined, or even the bind callback function (socket.bind(cb));
    // per the bind([port][, address][, callback]) signature all of those
    // mean an ephemeral-port listen, not an fd/pipe listen.
    const port = (isUdp && typeof options.port !== 'number') ? 0 : options.port;
    const isPortListen =
      typeof port === 'number' && port >= 0 &&
      (options.fd == null || options.fd < 0);

    if ((!isTcp && !isUdp) || !isPortListen)
      return originalGetServer.call(this, obj, options, cb);

    function createHandle(boundPort) {
      return isTcp ?
        net._createServerHandle(options.address, boundPort,
                                options.addressType, options.fd,
                                (options.flags | UV_TCP_REUSEPORT) >>> 0) :
        dgram._createSocketHandle(options.address, boundPort,
                                  options.addressType, options.fd,
                                  (options.flags | UV_UDP_REUSEPORT) >>> 0);
    }

    function adopt(rval) {
      ownHandles.add(rval);
      const originalClose = rval.close;
      rval.close = function() {
        ownHandles.delete(rval);
        return originalClose.apply(rval, arguments);
      };

      if (!disconnectHookInstalled && cluster.worker) {
        disconnectHookInstalled = true;
        // Mirror lib/internal/cluster/child.js: close listeners when the
        // worker disconnects so the process can drain and exit.
        cluster.worker.once('disconnect', () => {
          for (const handle of ownHandles)
            handle.close();
          ownHandles.clear();
        });
      }

      obj.once('listening', () => {
        if (cluster.worker)
          cluster.worker.state = 'listening';
        const address = obj.address();
        sendHelper(process, {
          act: 'listening',
          address: options.address,
          port: (address && address.port) || options.port,
          addressType: options.addressType,
          fd: options.fd,
        }, null);
      });
    }

    // Keyed exactly like queryServer() in lib/internal/cluster/primary.js so
    // that two distinct ephemeral servers in one worker stay distinct, and the
    // Nth ephemeral server of every worker agrees with the Nth of the others.
    const indexesKey =
      `${options.address}:${port}:${options.addressType}:${options.fd}`;
    const index = indexes.get(indexesKey) || 0;
    indexes.set(indexesKey, index + 1);
    const key = `${indexesKey}:${index}`;

    // Every listen takes the round trip, exactly as upstream does, even when
    // the port is fixed and needs no arbitration. The reply also carries the
    // server data (TLS session ticket keys), which has to come from one place
    // for all workers or a session only resumes on the worker that issued it.
    // Binding locally with SO_REUSEPORT is the only part that diverges from
    // upstream; the round trip itself is not extra cost.
    sendHelper(process, {
      act: RESERVE_ACT,
      key,
      port,
      data: typeof obj._getServerData === 'function' ? obj._getServerData() : null,
    }, null, (reply) => {
      if (!reply || (!reply.assign && !reply.port)) {
        cb(UV_EINVAL, null);
        return;
      }

      if (typeof obj._setServerData === 'function')
        obj._setServerData(reply.data);

      const rval = createHandle(reply.assign ? 0 : reply.port);
      if (typeof rval === 'number') {
        // Report the failed election so a waiter can take over; otherwise
        // every other worker blocks forever on a port nobody owns.
        if (reply.assign) {
          sendHelper(process,
                     { act: REPORT_ACT, key, port: 0, errno: rval }, null);
        }
        cb(rval, null);
        return;
      }

      if (reply.assign) {
        const sockname = {};
        const err = rval.getsockname(sockname);
        const chosen = err === 0 ? sockname.port : 0;
        sendHelper(process,
                   { act: REPORT_ACT, key, port: chosen, errno: err || 0 },
                   null);
        if (!chosen) {
          // Bound, but to a port we cannot name, so no peer can join it.
          // Fail this listen rather than run a one-worker "cluster".
          rval.close();
          cb(err || UV_EINVAL, null);
          return;
        }
      }

      adopt(rval);
      cb(0, rval);
    });
  };
})
)JS";

// Invoked with (cluster, sendHelper, RESERVE_ACT, REPORT_ACT) in the primary.
//
// Arbitrates the ephemeral port for a listen key. The first worker to ask is
// elected to pick one and report it back; anyone who asks in the meantime is
// parked until it does. Only a number crosses the channel, never a descriptor,
// so this works on WASIX where the upstream shared-handle path cannot.
//
// lib/internal/cluster/primary.js ignores acts it does not know
// (methodMessageMapping lookup), so these ride alongside the upstream protocol
// on a second 'internalMessage' listener without disturbing it.
constexpr const char kPrimaryInstallScript[] = R"JS(
(function installWasixClusterPortBroker(cluster, sendHelper,
                                        RESERVE_ACT, REPORT_ACT) {
  'use strict';

  if (!cluster.isPrimary) return;
  // Spawn calls this on every cluster.fork(); only the first should install.
  if (cluster.__edgeWasixPortBroker) return;
  Object.defineProperty(cluster, '__edgeWasixPortBroker', { value: true });

  // key -> { port, elector, waiters: [{ worker, seq }] }
  const reservations = new Map();

  function reply(worker, seq, payload) {
    if (!worker || !worker.process || !worker.process.connected) return;
    payload.ack = seq;
    sendHelper(worker.process, payload, null);
  }

  function elect(entry, worker, seq) {
    entry.elector = worker;
    reply(worker, seq, { assign: true, data: entry.data });
  }

  function promoteNextWaiter(key, entry) {
    const next = entry.waiters.shift();
    if (next) elect(entry, next.worker, next.seq);
    else reservations.delete(key);
  }

  function onMessage(worker, message) {
    if (!message || message.cmd !== 'NODE_CLUSTER') return;

    if (message.act === RESERVE_ACT) {
      let entry = reservations.get(message.key);
      if (entry === undefined) {
        // A fixed port is already agreed by definition; only port 0 needs an
        // election. Either way the first caller's data becomes the shared
        // copy, matching `handle.data ||= message.data` in primary.js.
        entry = {
          port: message.port > 0 ? message.port : 0,
          data: message.data,
          elector: null,
          waiters: [],
        };
        reservations.set(message.key, entry);
      } else if (entry.data == null) {
        entry.data = message.data;
      }

      if (entry.port)
        reply(worker, message.seq, { port: entry.port, data: entry.data });
      else if (entry.elector === null)
        elect(entry, worker, message.seq);
      else
        entry.waiters.push({ worker, seq: message.seq });
      return;
    }

    if (message.act === REPORT_ACT) {
      const entry = reservations.get(message.key);
      if (entry === undefined || entry.elector !== worker) return;
      entry.elector = null;
      if (message.port > 0) {
        entry.port = message.port;
        for (const waiter of entry.waiters)
          reply(waiter.worker, waiter.seq, { port: entry.port, data: entry.data });
        entry.waiters.length = 0;
      } else {
        promoteNextWaiter(message.key, entry);
      }
    }
  }

  cluster.on('fork', (worker) => {
    worker.process.on('internalMessage',
                      (message) => onMessage(worker, message));
    worker.once('exit', () => {
      // A worker that dies mid-election would otherwise strand every waiter.
      for (const { 0: key, 1: entry } of reservations) {
        entry.waiters = entry.waiters.filter((w) => w.worker !== worker);
        if (entry.elector === worker && !entry.port) {
          entry.elector = null;
          promoteNextWaiter(key, entry);
        }
      }
    });
  });
})
)JS";

void ClearPendingException(napi_env env) {
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
    napi_value ignored = nullptr;
    (void)napi_get_and_clear_last_exception(env, &ignored);
  }
}

// Compiles `script` and calls it with `argv`. Returns false and swallows the
// exception if anything along the way fails; a missing strategy degrades to
// upstream behavior rather than taking the process down.
bool RunInstallScript(napi_env env,
                      const char* source,
                      napi_value* argv,
                      size_t argc) {
  napi_value script = nullptr;
  napi_value install_fn = nullptr;
  if (napi_create_string_utf8(env, source, NAPI_AUTO_LENGTH, &script) != napi_ok ||
      napi_run_script(env, script, &install_fn) != napi_ok || install_fn == nullptr) {
    ClearPendingException(env);
    return false;
  }

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok) {
    ClearPendingException(env);
    return false;
  }

  napi_value result = nullptr;
  if (napi_call_function(env, global, install_fn, argc, argv, &result) != napi_ok) {
    ClearPendingException(env);
    return false;
  }
  return true;
}

// Resolves internal/cluster/utils#sendHelper, the ack-matching send both
// halves of the protocol ride on.
bool GetSendHelper(napi_env env, napi_value* out) {
  napi_value utils = nullptr;
  if (!EdgeRequireBuiltin(env, "internal/cluster/utils", &utils) || utils == nullptr) {
    ClearPendingException(env);
    return false;
  }
  if (napi_get_named_property(env, utils, "sendHelper", out) != napi_ok || *out == nullptr) {
    ClearPendingException(env);
    return false;
  }
  return true;
}

bool GetActNames(napi_env env, napi_value* reserve, napi_value* report) {
  if (napi_create_string_utf8(env, kReserveAct, NAPI_AUTO_LENGTH, reserve) != napi_ok ||
      napi_create_string_utf8(env, kReportAct, NAPI_AUTO_LENGTH, report) != napi_ok) {
    ClearPendingException(env);
    return false;
  }
  return true;
}

}  // namespace

void EdgeMaybeInstallWasixClusterReusePort(napi_env env) {
  // pre_execution deletes NODE_UNIQUE_ID from process.env, but this runs
  // before the main builtin, while the variable is still present.
  if (std::getenv("NODE_UNIQUE_ID") == nullptr) return;

  napi_value cluster = nullptr;
  napi_value net = nullptr;
  napi_value dgram = nullptr;
  if (!EdgeRequireBuiltin(env, "cluster", &cluster) || cluster == nullptr ||
      !EdgeRequireBuiltin(env, "net", &net) || net == nullptr ||
      !EdgeRequireBuiltin(env, "internal/dgram", &dgram) || dgram == nullptr) {
    ClearPendingException(env);
    return;
  }

  napi_value send_helper = nullptr;
  if (!GetSendHelper(env, &send_helper)) return;

  napi_value tcp_reuseport_flag = nullptr;
  napi_value udp_reuseport_flag = nullptr;
  napi_value reserve_act = nullptr;
  napi_value report_act = nullptr;
  if (napi_create_uint32(env, static_cast<uint32_t>(UV_TCP_REUSEPORT), &tcp_reuseport_flag) != napi_ok ||
      napi_create_uint32(env, static_cast<uint32_t>(UV_UDP_REUSEPORT), &udp_reuseport_flag) != napi_ok) {
    ClearPendingException(env);
    return;
  }
  if (!GetActNames(env, &reserve_act, &report_act)) return;

  napi_value argv[] = {cluster, net, dgram, send_helper,
                       tcp_reuseport_flag, udp_reuseport_flag,
                       reserve_act, report_act};
  (void)RunInstallScript(env, kWorkerInstallScript, argv, 8);
}

void EdgeMaybeInstallWasixClusterPrimary(napi_env env) {
  // Only reached from a cluster.fork() spawn, so `cluster` is already resolved
  // and this require is a cache hit. Requiring it eagerly at startup instead
  // would cost every process ~21 ms for a module most never touch.
  napi_value cluster = nullptr;
  if (!EdgeRequireBuiltin(env, "cluster", &cluster) || cluster == nullptr) {
    ClearPendingException(env);
    return;
  }

  napi_value send_helper = nullptr;
  if (!GetSendHelper(env, &send_helper)) return;

  napi_value reserve_act = nullptr;
  napi_value report_act = nullptr;
  if (!GetActNames(env, &reserve_act, &report_act)) return;

  napi_value argv[] = {cluster, send_helper, reserve_act, report_act};
  (void)RunInstallScript(env, kPrimaryInstallScript, argv, 4);
}

#else  // !defined(__wasi__)

void EdgeMaybeInstallWasixClusterReusePort(napi_env /*env*/) {}

void EdgeMaybeInstallWasixClusterPrimary(napi_env /*env*/) {}

#endif  // defined(__wasi__)
