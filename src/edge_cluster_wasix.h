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

#endif  // EDGE_CLUSTER_WASIX_H_
