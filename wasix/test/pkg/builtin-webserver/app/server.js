const http = require("http");
const host = "0.0.0.0";
const port = 8080;

const server = http.createServer((req, res) => {
  res.setHeader("Content-Type", "text/plain");

  if (req.url === "/") {
    res.end("hello");
    return;
  }

  res.statusCode = 404;
  res.end("not found");
});

server.listen(port, host, () => {
  console.log(`listening on http://${host}:${port}`);
});
