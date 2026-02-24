#include <string>

#include "test_env.h"

extern "C" napi_value Init(napi_env env, napi_value exports);

class Test12Error : public FixtureTestBase {};

TEST_F(Test12Error, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(Init(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__err", exports), napi_ok);

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

  ASSERT_TRUE(run_js("if(__err.checkError(new Error('x'))!==true) throw new Error('iserr1');"));
  ASSERT_TRUE(run_js("if(__err.checkError({})!==false) throw new Error('iserr2');"));
  ASSERT_TRUE(run_js("let ok=false; try { __err.throwExistingError(); } catch(e) { ok=(e instanceof Error && e.message==='existing error'); } if(!ok) throw new Error('throwExistingError');"));
  ASSERT_TRUE(run_js("let ok=false; try { __err.throwError(); } catch(e) { ok=(e instanceof Error && e.message==='error'); } if(!ok) throw new Error('throwError');"));
  ASSERT_TRUE(run_js("let ok=false; try { __err.throwRangeError(); } catch(e) { ok=(e instanceof RangeError && e.message==='range error'); } if(!ok) throw new Error('throwRangeError');"));
  ASSERT_TRUE(run_js("let ok=false; try { __err.throwTypeError(); } catch(e) { ok=(e instanceof TypeError && e.message==='type error'); } if(!ok) throw new Error('throwTypeError');"));
  ASSERT_TRUE(run_js("let ok=false; try { __err.throwSyntaxError(); } catch(e) { ok=(e instanceof SyntaxError && e.message==='syntax error'); } if(!ok) throw new Error('throwSyntaxError');"));
  ASSERT_TRUE(run_js("let ok=false; try { __err.throwErrorCode(); } catch(e) { ok=(e.code==='ERR_TEST_CODE' && e.message==='Error [error]'); } if(!ok) throw new Error('throwErrorCode');"));
  ASSERT_TRUE(run_js("let ok=false; try { __err.throwRangeErrorCode(); } catch(e) { ok=(e.code==='ERR_TEST_CODE' && e.message==='RangeError [range error]'); } if(!ok) throw new Error('throwRangeErrorCode');"));
  ASSERT_TRUE(run_js("let ok=false; try { __err.throwTypeErrorCode(); } catch(e) { ok=(e.code==='ERR_TEST_CODE' && e.message==='TypeError [type error]'); } if(!ok) throw new Error('throwTypeErrorCode');"));
  ASSERT_TRUE(run_js("let ok=false; try { __err.throwSyntaxErrorCode(); } catch(e) { ok=(e.code==='ERR_TEST_CODE' && e.message==='SyntaxError [syntax error]'); } if(!ok) throw new Error('throwSyntaxErrorCode');"));
  ASSERT_TRUE(run_js("let ok=false; try { __err.throwArbitrary(42); } catch(e) { ok=(e===42); } if(!ok) throw new Error('throwArbitrary');"));
  ASSERT_TRUE(run_js("const e=__err.createError(); if(!(e instanceof Error)||e.message!=='error') throw new Error('createError');"));
  ASSERT_TRUE(run_js("const e=__err.createRangeError(); if(!(e instanceof RangeError)||e.message!=='range error') throw new Error('createRangeError');"));
  ASSERT_TRUE(run_js("const e=__err.createTypeError(); if(!(e instanceof TypeError)||e.message!=='type error') throw new Error('createTypeError');"));
  ASSERT_TRUE(run_js("const e=__err.createSyntaxError(); if(!(e instanceof SyntaxError)||e.message!=='syntax error') throw new Error('createSyntaxError');"));
  ASSERT_TRUE(run_js("const e=__err.createErrorCode(); if(!(e instanceof Error)||e.code!=='ERR_TEST_CODE'||e.message!=='Error [error]'||e.name!=='Error') throw new Error('createErrorCode');"));
  ASSERT_TRUE(run_js("const e=__err.createRangeErrorCode(); if(!(e instanceof RangeError)||e.code!=='ERR_TEST_CODE'||e.message!=='RangeError [range error]'||e.name!=='RangeError') throw new Error('createRangeErrorCode');"));
  ASSERT_TRUE(run_js("const e=__err.createTypeErrorCode(); if(!(e instanceof TypeError)||e.code!=='ERR_TEST_CODE'||e.message!=='TypeError [type error]'||e.name!=='TypeError') throw new Error('createTypeErrorCode');"));
  ASSERT_TRUE(run_js("const e=__err.createSyntaxErrorCode(); if(!(e instanceof SyntaxError)||e.code!=='ERR_TEST_CODE'||e.message!=='SyntaxError [syntax error]'||e.name!=='SyntaxError') throw new Error('createSyntaxErrorCode');"));
}
