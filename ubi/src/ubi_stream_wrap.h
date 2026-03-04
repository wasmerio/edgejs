#ifndef UBI_STREAM_WRAP_H_
#define UBI_STREAM_WRAP_H_

#include "node_api.h"

enum UbiStreamStateIndex : int {
  kUbiReadBytesOrError = 0,
  kUbiArrayBufferOffset = 1,
  kUbiBytesWritten = 2,
  kUbiLastWriteWasAsync = 3,
  kUbiStreamStateLength = 4,
};

napi_value UbiInstallStreamWrapBinding(napi_env env);
int32_t* UbiGetStreamBaseState();

#endif  // UBI_STREAM_WRAP_H_
