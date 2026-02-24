#include "test_env.h"

extern "C" napi_value Init(napi_env env, napi_value exports);

class Test15Function : public FixtureTestBase {};

TEST_F(Test15Function, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(Init(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__fn", exports), napi_ok);

  auto run_js = [&](const char* source_text) {
    v8::TryCatch tc(s.isolate);
    std::string wrapped = std::string("(() => { ") + source_text + " })();";
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

  ASSERT_TRUE(run_js("function func1(){ return 1; } if(__fn.TestCall(func1)!==1) throw new Error('call1');"));
  ASSERT_TRUE(run_js("function func2(){ return null; } if(__fn.TestCall(func2)!==null) throw new Error('call2');"));
  ASSERT_TRUE(run_js("function func3(i){ return i+1; } if(__fn.TestCall(func3,1)!==2) throw new Error('call3');"));
  ASSERT_TRUE(run_js("function func4(i){ return i+1; } if(__fn.TestCall(func4,1)!==2) throw new Error('call4');"));
  ASSERT_TRUE(run_js("if(__fn.TestName.name!=='Name') throw new Error('name1');"));
  ASSERT_TRUE(run_js("if(__fn.TestNameShort.name!=='Name_') throw new Error('name2');"));
  ASSERT_TRUE(run_js("const r=__fn.TestCreateFunctionParameters(); if(!r||r.envIsNull!=='Invalid argument'||r.nameIsNull!=='napi_ok'||r.cbIsNull!=='Invalid argument'||r.resultIsNull!=='Invalid argument') throw new Error('params');"));
  ASSERT_TRUE(run_js("let ok=false; try { __fn.TestBadReturnExceptionPending(); } catch(e) { ok=(e && e.code==='throwing exception' && e.name==='Error'); } if(!ok) throw new Error('badReturnPending');"));
}
