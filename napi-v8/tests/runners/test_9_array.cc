#include "test_env.h"

extern "C" napi_value Init(napi_env env, napi_value exports);

class Test9Array : public FixtureTestBase {};

TEST_F(Test9Array, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(Init(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__addon", exports), napi_ok);

  auto run_js = [&](const char* source_text) {
    v8::TryCatch tc(s.isolate);
    v8::Local<v8::String> source =
        v8::String::NewFromUtf8(s.isolate, source_text, v8::NewStringType::kNormal)
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

  ASSERT_TRUE(run_js(
      "globalThis.__arr=[1,9,48,13493,9459324,{name:'hello'},['world','node','abi']];"));
  ASSERT_TRUE(run_js(
      "let oob=false; try { __addon.TestGetElement(__arr, __arr.length+1); } catch(e){ oob=true; } if(!oob) throw new Error('oob');"));
  ASSERT_TRUE(run_js(
      "let neg=false; try { __addon.TestGetElement(__arr, -2); } catch(e){ neg=true; } if(!neg) throw new Error('neg');"));
  ASSERT_TRUE(run_js(
      "for(let i=0;i<__arr.length;i++){ if(__addon.TestGetElement(__arr,i)!==__arr[i]) throw new Error('get '+i); }"));
  ASSERT_TRUE(run_js(
      "const copied=__addon.New(__arr); if(JSON.stringify(copied)!==JSON.stringify(__arr)) throw new Error('copy');"));
  ASSERT_TRUE(run_js("if(!__addon.TestHasElement(__arr,0)) throw new Error('has0');"));
  ASSERT_TRUE(run_js("if(__addon.TestHasElement(__arr,__arr.length+1)!==false) throw new Error('hasoob');"));
  ASSERT_TRUE(run_js("if(!(__addon.NewWithLength(0) instanceof Array)) throw new Error('len0');"));
  ASSERT_TRUE(run_js("if(!(__addon.NewWithLength(1) instanceof Array)) throw new Error('len1');"));
  ASSERT_TRUE(run_js("if(!(__addon.NewWithLength(4294967295) instanceof Array)) throw new Error('lenmax');"));
  ASSERT_TRUE(run_js(
      "const arr=['a','b','c','d']; if(arr.length!==4||!(2 in arr)) throw new Error('pre'); if(__addon.TestDeleteElement(arr,2)!==true) throw new Error('delret'); if(arr.length!==4||(2 in arr)!==false) throw new Error('post');"));
}
