export const dynamic = 'force-dynamic';

export function GET() {
  return new Response('hello edge\n', {
    headers: { 'Content-Type': 'text/plain; charset=utf-8', 'Content-Length': '11' },
  });
}
