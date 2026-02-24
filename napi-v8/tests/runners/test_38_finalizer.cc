#include <string>

#include "test_env.h"

extern "C" napi_value Init(napi_env env, napi_value exports);

class Test38Finalizer : public FixtureTestBase {};

TEST_F(Test38Finalizer, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(Init(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__tf", exports), napi_ok);

  auto run_js = [&](const char* source_text) {
    v8::TryCatch tc(s.isolate);
    std::string wrapped = std::string("(() => { 'use strict'; ") + source_text + " })();";
    v8::Local<v8::String> source =
        v8::String::NewFromUtf8(s.isolate, wrapped.c_str(), v8::NewStringType::kNormal)
            .ToLocalChecked();
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(s.context, source).ToLocal(&script)) return false;
    v8::Local<v8::Value> out;
    if (!script->Run(s.context).ToLocal(&out)) {
      if (tc.HasCaught()) {
        v8::String::Utf8Value msg(s.isolate, tc.Exception());
        ADD_FAILURE() << "JS exception: " << (*msg ? *msg : "<empty>")
                      << " while running: " << source_text;
      }
      return false;
    }
    return true;
  };

  auto get_count = [&]() -> int32_t {
    napi_value fn = nullptr;
    napi_value out = nullptr;
    int32_t count = -1;
    if (napi_get_named_property(s.env, exports, "getFinalizerCallCount", &fn) != napi_ok) return -1;
    if (napi_call_function(s.env, exports, fn, 0, nullptr, &out) != napi_ok) return -1;
    if (napi_get_value_int32(s.env, out, &count) != napi_ok) return -1;
    return count;
  };

  auto pump_gc = [&]() {
    s.isolate->LowMemoryNotification();
    s.isolate->PerformMicrotaskCheckpoint();
  };

  ASSERT_TRUE(run_js(R"JS(
globalThis.__js_finalizer_called = false;
(() => {
  const obj = {};
  __tf.addFinalizer(obj);
})();
)JS"));

  for (int i = 0; i < 100 && get_count() < 1; ++i) {
    pump_gc();
  }
  ASSERT_EQ(get_count(), 1);

  ASSERT_TRUE(run_js(R"JS(
(() => {
  const obj = {};
  __tf.addFinalizerWithJS(obj, () => { globalThis.__js_finalizer_called = true; });
})();
)JS"));

  for (int i = 0; i < 100 && get_count() < 2; ++i) {
    pump_gc();
  }
  ASSERT_EQ(get_count(), 2);
  ASSERT_TRUE(run_js("if (!globalThis.__js_finalizer_called) throw new Error('jsFinalizerCalled');"));
}
