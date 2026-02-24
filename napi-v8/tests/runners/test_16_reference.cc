#include "test_env.h"

extern "C" napi_value Init(napi_env env, napi_value exports);

class Test16Reference : public FixtureTestBase {};

TEST_F(Test16Reference, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(Init(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__ref", exports), napi_ok);

  auto run_js = [&](const char* source) {
    v8::TryCatch tc(s.isolate);
    std::string wrapped = std::string("(() => { ") + source + " })();";
    v8::Local<v8::String> source_code =
        v8::String::NewFromUtf8(s.isolate, wrapped.c_str(), v8::NewStringType::kNormal)
            .ToLocalChecked();
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(s.context, source_code).ToLocal(&script)) return false;
    v8::Local<v8::Value> out;
    if (!script->Run(s.context).ToLocal(&out)) {
      if (tc.HasCaught()) {
        v8::String::Utf8Value msg(s.isolate, tc.Exception());
        ADD_FAILURE() << "JS exception: " << (*msg ? *msg : "<empty>")
                      << " while running: " << source;
      }
      return false;
    }
    return true;
  };

  ASSERT_TRUE(run_js(R"JS(
(() => {
  const s1 = __ref.createSymbol('x');
  __ref.createReference(s1, 0);
  if (__ref.referenceValue !== s1) throw new Error('symbolRef');
  __ref.deleteReference();
})();
)JS"));

  ASSERT_TRUE(run_js(R"JS(
(() => {
  const s1 = __ref.createSymbolFor('k');
  const s2 = __ref.createSymbolFor('k');
  if (s1 !== s2) throw new Error('symbolForStable');
  __ref.createReference(s1, 1);
  if (__ref.referenceValue !== Symbol.for('k')) throw new Error('symbolForValue');
  if (__ref.decrementRefcount() !== 0) throw new Error('unrefToWeak');
  __ref.deleteReference();
})();
)JS"));

  ASSERT_TRUE(run_js(R"JS(
(() => {
  const s = __ref.createSymbolForEmptyString();
  if (s !== Symbol.for('')) throw new Error('symbolForEmpty');
})();
)JS"));

  ASSERT_TRUE(run_js(R"JS(
(() => {
  let threw = false;
  try {
    __ref.createSymbolForIncorrectLength();
  } catch (e) {
    threw = /Invalid argument/.test(String(e && e.message));
  }
  if (!threw) throw new Error('invalidLengthExpected');
})();
)JS"));

  ASSERT_TRUE(run_js(R"JS(
(() => {
  const ext = __ref.createExternal();
  __ref.checkExternal(ext);
  __ref.createReference(ext, 1);
  if (__ref.incrementRefcount() !== 2) throw new Error('refUp');
  if (__ref.decrementRefcount() !== 1) throw new Error('refDown1');
  if (__ref.decrementRefcount() !== 0) throw new Error('refDown0');
  __ref.deleteReference();
})();
)JS"));
}
