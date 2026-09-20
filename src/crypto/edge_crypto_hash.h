#ifndef EDGE_CRYPTO_HASH_H_
#define EDGE_CRYPTO_HASH_H_

#include <openssl/evp.h>

#include "edge_buffer_lease.h"

namespace edge::crypto {

inline bool UpdateHashFromBuffer(napi_env env,
                                 EVP_MD_CTX* context,
                                 napi_value value,
                                 size_t byte_length) {
  // Imported N-API leases copy host-owned bytes into guest memory. Bound the
  // lease size so even a complete archive never needs an archive-sized guest
  // allocation. Each read-only lease is released before the next is acquired.
  constexpr size_t kHashChunkSize = 256 * 1024;
  for (size_t offset = 0; offset < byte_length;) {
    const size_t remaining = byte_length - offset;
    const size_t length = remaining < kHashChunkSize ? remaining : kHashChunkSize;
    EdgeBufferLease input;
    if (!input.Acquire(env, value, offset, length, unofficial_napi_buffer_access_read)) return false;
    const bool ok = EVP_DigestUpdate(context, input.data(), input.size()) == 1;
    if (!input.Release(false) || !ok) return false;
    offset += length;
  }
  return true;
}

}  // namespace edge::crypto

#endif  // EDGE_CRYPTO_HASH_H_
