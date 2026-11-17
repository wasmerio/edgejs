#pragma once

#include "node_api.h"

napi_value EdgeInstallBufferBinding(napi_env env);

// The env's Buffer.prototype, which bootstrap hands to the buffer binding's
// setBufferPrototype, or nullptr before that and after the env's cleanup.
// Reading it runs no JS.
napi_value EdgeGetBufferPrototype(napi_env env);

// Makes `bytes`, a Uint8Array, a Buffer in place by giving it the env's
// Buffer.prototype, as Node's Buffer::New does; returns `bytes`. Before
// bootstrap sets the prototype it stays a plain Uint8Array. No JS runs and
// nothing is copied (unlike Buffer.from, which would copy small byte counts
// into Buffer's shared pool).
napi_value EdgeMakeBuffer(napi_env env, napi_value bytes);
