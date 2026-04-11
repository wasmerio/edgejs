export function normalizeToken(input) {
  return input
      .trim()
      .toLowerCase()
      .replace(/[^a-z]/g, "");
}
