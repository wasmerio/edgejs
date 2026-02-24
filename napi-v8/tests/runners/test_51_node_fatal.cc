#include <string>

#include <gtest/gtest.h>

#include "test_env.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test51NodeFatal : public FixtureTestBase {};

TEST_F(Test51NodeFatal, FatalMessageDeath) {
  ASSERT_DEATH(
      {
        EnvScope s(runtime_.get());
        napi_value exports = nullptr;
        ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
        ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);
        napi_value global = nullptr;
        ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
        ASSERT_EQ(napi_set_named_property(s.env, global, "__tf", exports), napi_ok);

        v8::Local<v8::String> source = v8::String::NewFromUtf8(
            s.isolate, "__tf.Test()", v8::NewStringType::kNormal).ToLocalChecked();
        v8::Local<v8::Script> script =
            v8::Script::Compile(s.context, source).ToLocalChecked();
        v8::MaybeLocal<v8::Value> ignored = script->Run(s.context);
        (void)ignored;
      },
      "FATAL ERROR: test_fatal::Test fatal message");
}

TEST_F(Test51NodeFatal, FatalStringLengthDeath) {
  ASSERT_DEATH(
      {
        EnvScope s(runtime_.get());
        napi_value exports = nullptr;
        ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
        ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);
        napi_value global = nullptr;
        ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
        ASSERT_EQ(napi_set_named_property(s.env, global, "__tf", exports), napi_ok);

        v8::Local<v8::String> source = v8::String::NewFromUtf8(
            s.isolate, "__tf.TestStringLength()", v8::NewStringType::kNormal).ToLocalChecked();
        v8::Local<v8::Script> script =
            v8::Script::Compile(s.context, source).ToLocalChecked();
        v8::MaybeLocal<v8::Value> ignored = script->Run(s.context);
        (void)ignored;
      },
      "FATAL ERROR: test_fatal::Test fatal message");
}
