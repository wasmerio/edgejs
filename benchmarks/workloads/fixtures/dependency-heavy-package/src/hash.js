export function rollingHash(text, seed) {
  let hash = (2166136261 ^ seed) >>> 0;

  for (let i = 0; i < text.length; i += 1) {
    hash ^= text.charCodeAt(i);
    hash = Math.imul(hash, 16777619) >>> 0;
  }

  return hash;
}
