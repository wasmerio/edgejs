#include "test_env.h"

#include "unofficial_napi.h"

class Test65UnofficialContextify : public FixtureTestBase {};

namespace {

napi_value Str(napi_env env, const char* value) {
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &out) != napi_ok) return nullptr;
  return out;
}

napi_value Sym(napi_env env, const char* value) {
  napi_value desc = Str(env, value);
  if (desc == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_create_symbol(env, desc, &out) != napi_ok) return nullptr;
  return out;
}

}  // namespace

TEST_F(Test65UnofficialContextify, MakeRunDisposeRoundTrip) {
  EnvScope s(runtime_.get());

  napi_value sandbox = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);

  napi_value result = nullptr;
  ASSERT_EQ(unofficial_napi_contextify_make_context(s.env,
                                                    sandbox,
                                                    Str(s.env, "ctx"),
                                                    Str(s.env, "test://origin"),
                                                    true,
                                                    true,
                                                    true,
                                                    Sym(s.env, "hdo"),
                                                    &result),
            napi_ok);
  ASSERT_NE(result, nullptr);

  napi_value eval_result = nullptr;
  ASSERT_EQ(unofficial_napi_contextify_run_script(s.env,
                                                  sandbox,
                                                  Str(s.env, "globalThis.answer = 42; answer"),
                                                  Str(s.env, "ctx.js"),
                                                  0,
                                                  0,
                                                  -1,
                                                  true,
                                                  false,
                                                  false,
                                                  Sym(s.env, "hdo"),
                                                  &eval_result),
            napi_ok);
  ASSERT_NE(eval_result, nullptr);

  int32_t answer = 0;
  ASSERT_EQ(napi_get_value_int32(s.env, eval_result, &answer), napi_ok);
  EXPECT_EQ(answer, 42);

  napi_value answer_value = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, sandbox, "answer", &answer_value), napi_ok);
  ASSERT_EQ(napi_get_value_int32(s.env, answer_value, &answer), napi_ok);
  EXPECT_EQ(answer, 42);

  ASSERT_EQ(unofficial_napi_contextify_dispose_context(s.env, sandbox), napi_ok);
  EXPECT_EQ(unofficial_napi_contextify_run_script(s.env,
                                                  sandbox,
                                                  Str(s.env, "1"),
                                                  Str(s.env, "after_dispose.js"),
                                                  0,
                                                  0,
                                                  -1,
                                                  true,
                                                  false,
                                                  false,
                                                  Sym(s.env, "hdo"),
                                                  &eval_result),
            napi_invalid_arg);
}

TEST_F(Test65UnofficialContextify, CompileFunctionAndCachedData) {
  EnvScope s(runtime_.get());

  napi_value params = nullptr;
  ASSERT_EQ(napi_create_array_with_length(s.env, 2, &params), napi_ok);
  ASSERT_EQ(napi_set_element(s.env, params, 0, Str(s.env, "a")), napi_ok);
  ASSERT_EQ(napi_set_element(s.env, params, 1, Str(s.env, "b")), napi_ok);

  napi_value context_extensions = nullptr;
  ASSERT_EQ(napi_create_array_with_length(s.env, 0, &context_extensions), napi_ok);

  napi_value undef = nullptr;
  ASSERT_EQ(napi_get_undefined(s.env, &undef), napi_ok);

  napi_value out = nullptr;
  ASSERT_EQ(unofficial_napi_contextify_compile_function(s.env,
                                                        Str(s.env, "return a + b;"),
                                                        Str(s.env, "fn.js"),
                                                        0,
                                                        0,
                                                        undef,
                                                        true,
                                                        undef,
                                                        context_extensions,
                                                        params,
                                                        Sym(s.env, "hdo"),
                                                        &out),
            napi_ok);
  ASSERT_NE(out, nullptr);

  napi_value fn = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, out, "function", &fn), napi_ok);
  ASSERT_NE(fn, nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  napi_value argv[2] = {nullptr, nullptr};
  ASSERT_EQ(napi_create_int32(s.env, 2, &argv[0]), napi_ok);
  ASSERT_EQ(napi_create_int32(s.env, 3, &argv[1]), napi_ok);

  napi_value fn_result = nullptr;
  ASSERT_EQ(napi_call_function(s.env, global, fn, 2, argv, &fn_result), napi_ok);
  int32_t sum = 0;
  ASSERT_EQ(napi_get_value_int32(s.env, fn_result, &sum), napi_ok);
  EXPECT_EQ(sum, 5);

  napi_value cached_data = nullptr;
  ASSERT_EQ(unofficial_napi_contextify_create_cached_data(s.env,
                                                          Str(s.env, "1 + 1"),
                                                          Str(s.env, "script.js"),
                                                          0,
                                                          0,
                                                          Sym(s.env, "hdo"),
                                                          &cached_data),
            napi_ok);
  ASSERT_NE(cached_data, nullptr);
  bool is_typedarray = false;
  ASSERT_EQ(napi_is_typedarray(s.env, cached_data, &is_typedarray), napi_ok);
  EXPECT_TRUE(is_typedarray);
}

TEST_F(Test65UnofficialContextify, CjsLoaderAndSyntaxDetection) {
  EnvScope s(runtime_.get());

  napi_value out = nullptr;
  ASSERT_EQ(unofficial_napi_contextify_compile_function_for_cjs_loader(
                s.env,
                Str(s.env, "module.exports = 1;"),
                Str(s.env, "cjs.js"),
                false,
                true,
                &out),
            napi_ok);
  ASSERT_NE(out, nullptr);
  napi_value can_parse = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, out, "canParseAsESM", &can_parse), napi_ok);
  bool can_parse_bool = true;
  ASSERT_EQ(napi_get_value_bool(s.env, can_parse, &can_parse_bool), napi_ok);
  EXPECT_FALSE(can_parse_bool);

  ASSERT_EQ(unofficial_napi_contextify_compile_function_for_cjs_loader(
                s.env,
                Str(s.env, "export const x = 1;"),
                Str(s.env, "esmish.js"),
                false,
                true,
                &out),
            napi_ok);
  ASSERT_EQ(napi_get_named_property(s.env, out, "canParseAsESM", &can_parse), napi_ok);
  ASSERT_EQ(napi_get_value_bool(s.env, can_parse, &can_parse_bool), napi_ok);
  EXPECT_TRUE(can_parse_bool);

  bool contains = false;
  ASSERT_EQ(unofficial_napi_contextify_contains_module_syntax(s.env,
                                                              Str(s.env, "export const x = 1;"),
                                                              Str(s.env, "esmish.js"),
                                                              Str(s.env, "file:///esmish.js"),
                                                              true,
                                                              &contains),
            napi_ok);
  EXPECT_TRUE(contains);

  ASSERT_EQ(unofficial_napi_contextify_contains_module_syntax(s.env,
                                                              Str(s.env, "module.exports = 1;"),
                                                              Str(s.env, "cjs.js"),
                                                              Str(s.env, "file:///cjs.js"),
                                                              true,
                                                              &contains),
            napi_ok);
  EXPECT_FALSE(contains);
}
