const express = require('express');

const app = express();
app.get('/api/bench', (req, res) => res.type('text').send('hello edge\n'));
app.listen(Number(process.env.PORT || 3000), process.env.HOST || '127.0.0.1');
