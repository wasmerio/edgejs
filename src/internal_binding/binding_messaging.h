#ifndef EDGE_INTERNAL_BINDING_BINDING_MESSAGING_H_
#define EDGE_INTERNAL_BINDING_BINDING_MESSAGING_H_

#include <memory>

#include "node_api.h"

namespace internal_binding {

struct EdgeMessagePortData;
using EdgeMessagePortDataPtr = std::shared_ptr<EdgeMessagePortData>;

EdgeMessagePortDataPtr EdgeCreateMessagePortData();
void EdgeEntangleMessagePortData(const EdgeMessagePortDataPtr& first,
                                const EdgeMessagePortDataPtr& second);
EdgeMessagePortDataPtr EdgeGetMessagePortData(napi_env env, napi_value value);
napi_value EdgeCreateMessagePortForData(napi_env env, const EdgeMessagePortDataPtr& data);

}  // namespace internal_binding

#endif  // EDGE_INTERNAL_BINDING_BINDING_MESSAGING_H_
