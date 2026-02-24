#include <string>

#include "test_env.h"

extern "C" napi_value Init(napi_env env, napi_value exports);

class Test29BigInt : public FixtureTestBase {};

TEST_F(Test29BigInt, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(Init(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__tb", exports), napi_ok);

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

  ASSERT_TRUE(run_js(R"JS(
const nums = [
  0n, -0n, 1n, -1n, 100n, 2121n, -1233n, 986583n, -976675n,
  98765432213456789876546896323445679887645323232436587988766545658n,
  -4350987086545760976737453646576078997096876957864353245245769809n,
];
for (const num of nums) {
  if (num > -(2n ** 63n) && num < 2n ** 63n) {
    if (__tb.TestInt64(num) !== num) throw new Error('int64');
    if (__tb.IsLossless(num, true) !== true) throw new Error('losslessSigned');
  } else if (__tb.IsLossless(num, true) !== false) throw new Error('lossySigned');

  if (num >= 0 && num < 2n ** 64n) {
    if (__tb.TestUint64(num) !== num) throw new Error('uint64');
    if (__tb.IsLossless(num, false) !== true) throw new Error('losslessUnsigned');
  } else if (__tb.IsLossless(num, false) !== false) throw new Error('lossyUnsigned');

  if (__tb.TestWords(num) !== num) throw new Error('words');
}
let t = false;
try { __tb.CreateTooBigBigInt(); } catch (e) { t = (e && e.message === 'Invalid argument'); }
if (!t) throw new Error('tooBig');
t = false;
try { __tb.MakeBigIntWordsThrow(); } catch (e) { t = (e && e.name === 'RangeError' && e.message === 'Maximum BigInt size exceeded'); }
if (!t) throw new Error('wordsThrow');
)JS"));
}
