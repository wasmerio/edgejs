#include "ubi_stream_listener.h"

void UbiInitStreamListenerState(UbiStreamListenerState* state,
                                UbiStreamListener* initial) {
  if (state == nullptr) return;
  state->current = nullptr;
  if (initial == nullptr) return;
  initial->previous = nullptr;
  state->current = initial;
}

void UbiPushStreamListener(UbiStreamListenerState* state,
                           UbiStreamListener* listener) {
  if (state == nullptr || listener == nullptr) return;
  if (state->current == listener) return;
  listener->previous = state->current;
  state->current = listener;
}

bool UbiRemoveStreamListener(UbiStreamListenerState* state,
                             UbiStreamListener* listener) {
  if (state == nullptr || listener == nullptr) return false;

  UbiStreamListener* previous = nullptr;
  UbiStreamListener* current = state->current;
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

bool UbiStreamEmitAlloc(UbiStreamListenerState* state,
                        size_t suggested_size,
                        uv_buf_t* out) {
  if (state == nullptr || out == nullptr) return false;

  for (UbiStreamListener* listener = state->current;
       listener != nullptr;
       listener = listener->previous) {
    if (listener->on_alloc == nullptr) continue;
    if (listener->on_alloc(listener, suggested_size, out)) return true;
  }

  return false;
}

bool UbiStreamEmitRead(UbiStreamListenerState* state,
                       ssize_t nread,
                       const uv_buf_t* buf) {
  if (state == nullptr) return false;

  const uv_buf_t empty = uv_buf_init(nullptr, 0);
  const uv_buf_t* current_buf = buf;
  if (current_buf == nullptr) current_buf = &empty;

  for (UbiStreamListener* listener = state->current;
       listener != nullptr;
       listener = listener->previous) {
    if (listener->on_read == nullptr) continue;
    if (listener->on_read(listener, nread, current_buf)) return true;
    if (nread < 0) current_buf = &empty;
  }

  return false;
}

namespace {

bool EmitAfterWriteFrom(UbiStreamListener* listener,
                        napi_value req_obj,
                        int status) {
  for (; listener != nullptr; listener = listener->previous) {
    if (listener->on_after_write == nullptr) continue;
    if (listener->on_after_write(listener, req_obj, status)) return true;
  }
  return false;
}

bool EmitAfterShutdownFrom(UbiStreamListener* listener,
                           napi_value req_obj,
                           int status) {
  for (; listener != nullptr; listener = listener->previous) {
    if (listener->on_after_shutdown == nullptr) continue;
    if (listener->on_after_shutdown(listener, req_obj, status)) return true;
  }
  return false;
}

}  // namespace

bool UbiStreamEmitAfterWrite(UbiStreamListenerState* state,
                             napi_value req_obj,
                             int status) {
  if (state == nullptr) return false;
  return EmitAfterWriteFrom(state->current, req_obj, status);
}

bool UbiStreamEmitAfterShutdown(UbiStreamListenerState* state,
                                napi_value req_obj,
                                int status) {
  if (state == nullptr) return false;
  return EmitAfterShutdownFrom(state->current, req_obj, status);
}

bool UbiStreamPassAfterWrite(UbiStreamListener* listener,
                             napi_value req_obj,
                             int status) {
  return EmitAfterWriteFrom(listener != nullptr ? listener->previous : nullptr, req_obj, status);
}

bool UbiStreamPassAfterShutdown(UbiStreamListener* listener,
                                napi_value req_obj,
                                int status) {
  return EmitAfterShutdownFrom(listener != nullptr ? listener->previous : nullptr, req_obj, status);
}

void UbiStreamNotifyClosed(UbiStreamListenerState* state) {
  if (state == nullptr) return;
  UbiStreamListener* listener = state->current;
  state->current = nullptr;
  while (listener != nullptr) {
    if (listener->on_close != nullptr) listener->on_close(listener);
    listener = listener->previous;
  }
}
