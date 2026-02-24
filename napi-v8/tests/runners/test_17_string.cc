#include <string>

#include "test_env.h"

extern "C" napi_value Init(napi_env env, napi_value exports);

class Test17String : public FixtureTestBase {};

TEST_F(Test17String, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(Init(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__str", exports), napi_ok);

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

  ASSERT_TRUE(run_js(R"JS(
const kInsufficientIdx = 3;
const asciiCases = ['', 'hello world', 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789', '?!@#$%^&*()_+-=[]{}/.,<>\'"\\'];
const latin1Cases = [
  { str: '¡¢£¤¥¦§¨©ª«¬­®¯°±²³´µ¶·¸¹º»¼½¾¿', utf8Length: 62, utf8InsufficientIdx: 1 },
  { str: 'ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖ×ØÙÚÛÜÝÞßàáâãäåæçèéêëìíîïðñòóôõö÷øùúûüýþ', utf8Length: 126, utf8InsufficientIdx: 1 },
];
const unicodeCases = [{ str: '\u{2003}\u{2101}\u{2001}\u{202}\u{2011}', utf8Length: 14, utf8InsufficientIdx: 1 }];
function assert(cond, tag) { if (!cond) throw new Error(tag); }
function testLatin1Cases(str) {
  assert(__str.TestLatin1(str) === str, 'l1');
  assert(__str.TestLatin1AutoLength(str) === str, 'l2');
  assert(__str.TestLatin1External(str) === str, 'l3');
  assert(__str.TestLatin1ExternalAutoLength(str) === str, 'l4');
  assert(__str.TestPropertyKeyLatin1(str) === str, 'l5');
  assert(__str.TestPropertyKeyLatin1AutoLength(str) === str, 'l6');
  assert(__str.Latin1Length(str) === str.length, 'l7');
  if (str !== '') assert(__str.TestLatin1Insufficient(str) === str.slice(0, kInsufficientIdx), 'l8');
}
function testUnicodeCases(str, utf8Length, utf8InsufficientIdx) {
  assert(__str.TestUtf8(str) === str, 'u1');
  assert(__str.TestUtf16(str) === str, 'u2');
  assert(__str.TestUtf8AutoLength(str) === str, 'u3');
  assert(__str.TestUtf16AutoLength(str) === str, 'u4');
  assert(__str.TestUtf16External(str) === str, 'u5');
  assert(__str.TestUtf16ExternalAutoLength(str) === str, 'u6');
  assert(__str.TestPropertyKeyUtf8(str) === str, 'u7');
  assert(__str.TestPropertyKeyUtf8AutoLength(str) === str, 'u8');
  assert(__str.TestPropertyKeyUtf16(str) === str, 'u9');
  assert(__str.TestPropertyKeyUtf16AutoLength(str) === str, 'u10');
  assert(__str.Utf8Length(str) === utf8Length, 'u11');
  assert(__str.Utf16Length(str) === str.length, 'u12');
  if (str !== '') {
    assert(__str.TestUtf8Insufficient(str) === str.slice(0, utf8InsufficientIdx), 'u13');
    assert(__str.TestUtf16Insufficient(str) === str.slice(0, kInsufficientIdx), 'u14');
  }
}
asciiCases.forEach(testLatin1Cases);
asciiCases.forEach((str) => testUnicodeCases(str, str.length, kInsufficientIdx));
latin1Cases.forEach((it) => testLatin1Cases(it.str));
latin1Cases.forEach((it) => testUnicodeCases(it.str, it.utf8Length, it.utf8InsufficientIdx));
unicodeCases.forEach((it) => testUnicodeCases(it.str, it.utf8Length, it.utf8InsufficientIdx));
let threw = false;
try { __str.TestLargeUtf8(); } catch (e) { threw = String(e && e.message) === 'Invalid argument'; }
assert(threw, 'largeUtf8');
threw = false;
try { __str.TestLargeLatin1(); } catch (e) { threw = String(e && e.message) === 'Invalid argument'; }
assert(threw, 'largeLatin1');
threw = false;
try { __str.TestLargeUtf16(); } catch (e) { threw = String(e && e.message) === 'Invalid argument'; }
assert(threw, 'largeUtf16');
__str.TestMemoryCorruption(' '.repeat(64 * 1024));
)JS"));

  ASSERT_TRUE(run_js(R"JS(
const expected = {
  envIsNull: 'Invalid argument',
  stringIsNullNonZeroLength: 'Invalid argument',
  stringIsNullZeroLength: 'napi_ok',
  resultIsNull: 'Invalid argument',
};
const a = __str.testNull.test_create_latin1();
const b = __str.testNull.test_create_utf8();
const c = __str.testNull.test_create_utf16();
if (JSON.stringify(a) !== JSON.stringify(expected)) throw new Error('nullLatin1');
if (JSON.stringify(b) !== JSON.stringify(expected)) throw new Error('nullUtf8');
if (JSON.stringify(c) !== JSON.stringify(expected)) throw new Error('nullUtf16');
)JS"));
}
