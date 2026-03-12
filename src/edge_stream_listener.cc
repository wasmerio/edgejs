#include "edge_stream_listener.h"

void EdgeInitStreamListenerState(EdgeStreamListenerState* state,
                                EdgeStreamListener* initial) {
  if (state == nullptr) return;
  state->current = nullptr;
  if (initial == nullptr) return;
  initial->previous = nullptr;
  state->current = initial;
}

void EdgePushStreamListener(EdgeStreamListenerState* state,
                           EdgeStreamListener* listener) {
  if (state == nullptr || listener == nullptr) return;
  if (state->current == listener) return;
  listener->previous = state->current;
  state->current = listener;
}

bool EdgeRemoveStreamListener(EdgeStreamListenerState* state,
                             EdgeStreamListener* listener) {
  if (state == nullptr || listener == nullptr) return false;

  EdgeStreamListener* previous = nullptr;
  EdgeStreamListener* current = state->current;
  while (current != nullptr) {
    if (current == listener) {
      if (previous == nullptr) {
        state->current = current->previous;
      } else {
        previous->previous = current->previous;
      }
      current->previous = nullptr;
      return true;
    }
    previous = current;
    current = current->previous;
  }

  return false;
}

bool EdgeStreamEmitAlloc(EdgeStreamListenerState* state,
                        size_t suggested_size,
                        uv_buf_t* out) {
  if (state == nullptr || out == nullptr) return false;

  for (EdgeStreamListener* listener = state->current;
       listener != nullptr;
       listener = listener->previous) {
    if (listener->on_alloc == nullptr) continue;
    if (listener->on_alloc(listener, suggested_size, out)) return true;
  }

  return false;
}

bool EdgeStreamEmitRead(EdgeStreamListenerState* state,
                       ssize_t nread,
                       const uv_buf_t* buf) {
  if (state == nullptr) return false;

  const uv_buf_t empty = uv_buf_init(nullptr, 0);
  const uv_buf_t* current_buf = buf;
  if (current_buf == nullptr) current_buf = &empty;

  for (EdgeStreamListener* listener = state->current;
       listener != nullptr;
       listener = listener->previous) {
    if (listener->on_read == nullptr) continue;
    if (listener->on_read(listener, nread, current_buf)) return true;
    if (nread < 0) current_buf = &empty;
  }

  return false;
}

namespace {

bool EmitAfterWriteFrom(EdgeStreamListener* listener,
                        napi_value req_obj,
                        int status) {
  for (; listener != nullptr; listener = listener->previous) {
    if (listener->on_after_write == nullptr) continue;
    if (listener->on_after_write(listener, req_obj, status)) return true;
  }
  return false;
}

bool EmitAfterShutdownFrom(EdgeStreamListener* listener,
                           napi_value req_obj,
                           int status) {
  for (; listener != nullptr; listener = listener->previous) {
    if (listener->on_after_shutdown == nullptr) continue;
    if (listener->on_after_shutdown(listener, req_obj, status)) return true;
  }
  return false;
}

}  // namespace

bool EdgeStreamEmitAfterWrite(EdgeStreamListenerState* state,
                             napi_value req_obj,
                             int status) {
  if (state == nullptr) return false;
  return EmitAfterWriteFrom(state->current, req_obj, status);
}

bool EdgeStreamEmitAfterShutdown(EdgeStreamListenerState* state,
                                napi_value req_obj,
                                int status) {
  if (state == nullptr) return false;
  return EmitAfterShutdownFrom(state->current, req_obj, status);
}

bool EdgeStreamPassAfterWrite(EdgeStreamListener* listener,
                             napi_value req_obj,
                             int status) {
  return EmitAfterWriteFrom(listener != nullptr ? listener->previous : nullptr, req_obj, status);
}

bool EdgeStreamPassAfterShutdown(EdgeStreamListener* listener,
                                napi_value req_obj,
                                int status) {
  return EmitAfterShutdownFrom(listener != nullptr ? listener->previous : nullptr, req_obj, status);
}

void EdgeStreamNotifyClosed(EdgeStreamListenerState* state) {
  if (state == nullptr) return;
  EdgeStreamListener* listener = state->current;
  state->current = nullptr;
  while (listener != nullptr) {
    if (listener->on_close != nullptr) listener->on_close(listener);
    listener = listener->previous;
  }
}
