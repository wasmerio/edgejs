const { Hono } = require('hono');
const { serve } = require('@hono/node-server');

const app = new Hono();
app.get('/api/bench', (c) => c.text('hello edge\n'));
serve({ fetch: app.fetch, port: Number(process.env.PORT || 3000), hostname: process.env.HOST || '127.0.0.1' });
