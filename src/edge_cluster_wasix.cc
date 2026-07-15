#include "edge_cluster_wasix.h"

#if defined(__wasi__)

#include <cstdlib>

#include <uv.h>

#include "edge_module_loader.h"

namespace {

// Invoked with (cluster, net, sendHelper, UV_TCP_REUSEPORT). Kept as an
// embedded script instead of a lib/ module because the Node lib/ tree stays
// byte-identical to upstream; the behavior is edge/WASIX-specific.
//
// TCP port listens bypass the queryServer round trip entirely: the worker
// binds its own SO_REUSEPORT listener and reports the 'listening' act for
// the primary's bookkeeping (worker state, cluster 'listening' event). UDP,
// fd, and pipe listens keep the upstream path. Known limitation vs the
// upstream flow: obj._getServerData/_setServerData is not round-tripped
// through the primary, so e.g. TLS session ticket keys are per-worker.
constexpr const char kInstallScript[] = R"JS(
(function installWasixClusterReusePort(cluster, net, sendHelper, UV_TCP_REUSEPORT) {
  'use strict';

  if (!cluster.isWorker || typeof cluster._getServer !== 'function')
    return;

  const originalGetServer = cluster._getServer;
  const ownHandles = new Set();
  let disconnectHookInstalled = false;

  cluster._getServer = function(obj, options, cb) {
    const isTcpPortListen =
      (options.addressType === 4 || options.addressType === 6) &&
      typeof options.port === 'number' && options.port >= 0 &&
      (options.fd == null || options.fd < 0);

    if (!isTcpPortListen)
      return originalGetServer.call(this, obj, options, cb);

    const flags = (options.flags | UV_TCP_REUSEPORT) >>> 0;
    const rval = net._createServerHandle(options.address, options.port,
                                         options.addressType, options.fd,
                                         flags);
    if (typeof rval === 'number')
      return cb(rval, null);

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

    cb(0, rval);
  };
})
)JS";

void ClearPendingException(napi_env env) {
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
    napi_value ignored = nullptr;
    (void)napi_get_and_clear_last_exception(env, &ignored);
  }
}

}  // namespace

void EdgeMaybeInstallWasixClusterReusePort(napi_env env) {
  // pre_execution deletes NODE_UNIQUE_ID from process.env, but this runs
  // before the main builtin, while the variable is still present.
  if (std::getenv("NODE_UNIQUE_ID") == nullptr) return;

  napi_value cluster = nullptr;
  napi_value net = nullptr;
  napi_value utils = nullptr;
  if (!EdgeRequireBuiltin(env, "cluster", &cluster) || cluster == nullptr ||
      !EdgeRequireBuiltin(env, "net", &net) || net == nullptr ||
      !EdgeRequireBuiltin(env, "internal/cluster/utils", &utils) || utils == nullptr) {
    ClearPendingException(env);
    return;
  }

  napi_value send_helper = nullptr;
  if (napi_get_named_property(env, utils, "sendHelper", &send_helper) != napi_ok ||
      send_helper == nullptr) {
    ClearPendingException(env);
    return;
  }

  napi_value script = nullptr;
  napi_value install_fn = nullptr;
  if (napi_create_string_utf8(env, kInstallScript, NAPI_AUTO_LENGTH, &script) != napi_ok ||
      napi_run_script(env, script, &install_fn) != napi_ok || install_fn == nullptr) {
    ClearPendingException(env);
    return;
  }

  napi_value reuseport_flag = nullptr;
  napi_value global = nullptr;
  if (napi_create_uint32(env, static_cast<uint32_t>(UV_TCP_REUSEPORT), &reuseport_flag) != napi_ok ||
      napi_get_global(env, &global) != napi_ok) {
    ClearPendingException(env);
    return;
  }

  napi_value argv[] = {cluster, net, send_helper, reuseport_flag};
  napi_value result = nullptr;
  if (napi_call_function(env, global, install_fn, 4, argv, &result) != napi_ok) {
    ClearPendingException(env);
  }
}

#else  // !defined(__wasi__)

void EdgeMaybeInstallWasixClusterReusePort(napi_env /*env*/) {}

#endif  // defined(__wasi__)
