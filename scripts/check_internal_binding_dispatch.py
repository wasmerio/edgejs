#!/usr/bin/env python3

import re
import sys
from pathlib import Path


def extract_function_body(source: str, signature: str) -> str:
  start = source.find(signature)
  if start < 0:
    raise ValueError(f"signature not found: {signature}")

  brace = source.find("{", start)
  if brace < 0:
    raise ValueError(f"opening brace not found for: {signature}")

  depth = 0
  for i in range(brace, len(source)):
    ch = source[i]
    if ch == "{":
      depth += 1
    elif ch == "}":
      depth -= 1
      if depth == 0:
        return source[brace : i + 1]

  raise ValueError(f"unbalanced braces for: {signature}")


def main() -> int:
  if len(sys.argv) != 2:
    print("usage: check_internal_binding_dispatch.py <edge_module_loader.cc>", file=sys.stderr)
    return 2

  path = Path(sys.argv[1])
  text = path.read_text(encoding="utf-8")
  body = extract_function_body(text, "static napi_value NativeGetInternalBindingCallback")

  if re.search(r"\bif\s*\(\s*name\s*==\s*\"[^\"]+\"", body):
    print("error: monolithic internalBinding name checks found in NativeGetInternalBindingCallback",
          file=sys.stderr)
    return 1

  if "internal_binding::Resolve(env, name, options)" not in body:
    print("error: dispatch call missing in NativeGetInternalBindingCallback", file=sys.stderr)
    return 1

  return 0


if __name__ == "__main__":
  raise SystemExit(main())
