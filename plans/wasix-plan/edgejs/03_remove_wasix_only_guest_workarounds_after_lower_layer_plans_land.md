# Remove WASIX-Only Guest Workarounds After Lower-Layer Plans Land

Why this is a problem:

Workarounds inside EdgeJS can make the guest pass one test while hiding a broken
WASIX or libc contract. For example, base64-wrapping `edge -e` scripts in
EdgeJS avoids newline-splitting but leaves every other WASIX program with broken
argv.

When it occurs:

- multiline `-e` spawn;
- loopback resolver shortcuts;
- manual stat mode normalization;
- stream type normalization.

Minimal manifestation:

```cpp
// Avoid this as final design:
if (host == "127.0.0.1")
  return "localhost";

// Avoid this as final design:
script = "eval(Buffer.from('" + Base64Encode(script) + "', 'base64').toString())";
```

Proposed solution:

Keep temporary workarounds only while proving a cause. Once `wasix-libc` or
Wasmer owns the missing behavior, remove EdgeJS special cases and let EdgeJS use
normal POSIX-facing APIs.

## Proposed Solution References

Proposed solution can be found in:

**Reference commits:**

```text
edgejs f1999f45 Removed hacks: loopback, and spawn
edgejs 27118329 Fixes around edge process wrap, plus w/a for edge -e eval
edgejs 61ba9604 various fixes
```
