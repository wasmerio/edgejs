#ifndef EDGE_CLUSTER_WASIX_H_
#define EDGE_CLUSTER_WASIX_H_

#include "unofficial_napi.h"

// Installs the WASIX cluster reuseport scheduling strategy in cluster worker
// processes. No-op on native targets and outside cluster workers.
//
// WASIX cannot pass listen handles between processes (no SCM_RIGHTS over the
// IPC channel), which breaks both of Node cluster's scheduling strategies:
// round robin passes every accepted connection to a worker, and shared-handle
// mode passes the listen handle itself. SO_REUSEPORT works end to end, so TCP
// and UDP port listens in cluster workers bind their own socket instead and
// the host kernel distributes traffic between the workers.
//
// The strategy is implemented by replacing the worker-side cluster._getServer
// (an exported, documented-as-replaceable property) from an embedded script.
// It lives here rather than in lib/ because the Node lib/ tree is kept
// byte-identical to upstream.
void EdgeMaybeInstallWasixClusterReusePort(napi_env env);

// Installs the primary-side half of the strategy: a broker that arbitrates the
// ephemeral port behind a port-0 listen. No-op on native targets, outside the
// primary, and after the first call.
//
// A port-0 listen means "any port", but a cluster only stays one server if
// every worker lands on the *same* one. Upstream gets that for free by binding
// once in the primary and sharing the descriptor; without SCM_RIGHTS the
// workers must instead be told which number to bind, which is what the broker
// answers. Only a port number crosses the channel, never a descriptor.
//
// Call this when a cluster.fork() spawn is observed rather than at startup:
// that is the first point where the process is known to be a cluster primary,
// and `cluster` is already loaded by then, so it costs nothing. Requiring
// `cluster` eagerly would add ~21 ms to the startup of every process.
// createWorkerProcess() runs before cluster.emit('fork'), so a broker
// installed during the spawn still observes the worker that triggered it.
void EdgeMaybeInstallWasixClusterPrimary(napi_env env);

#endif  // EDGE_CLUSTER_WASIX_H_
