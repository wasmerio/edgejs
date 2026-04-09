'use strict';

const assert = require('node:assert/strict');

(async () => {
  const { Command, Option, Argument } = require('commander');

  // Basic option parsing
  const program = new Command();
  program
    .option('-p, --port <number>', 'server port')
    .option('-v, --verbose', 'enable verbose logging')
    .option('--no-color', 'disable color output');

  program.parse(['--port', '8080', '--verbose'], { from: 'user' });

  const opts = program.opts();
  assert.equal(opts.port, '8080');
  assert.equal(opts.verbose, true);
  assert.equal(opts.color, true, 'negatable boolean defaults to true');

  // Parsing with --no- prefix
  const prog2 = new Command();
  prog2.option('--no-color', 'disable color');
  prog2.parse(['--no-color'], { from: 'user' });
  assert.equal(prog2.opts().color, false);

  // Required option with default value
  const prog3 = new Command();
  prog3.option('-t, --timeout <ms>', 'timeout in ms', '3000');
  prog3.parse([], { from: 'user' });
  assert.equal(prog3.opts().timeout, '3000');

  // Arguments (positional)
  const prog4 = new Command();
  let parsedSource = '';
  prog4
    .argument('<source>', 'source file')
    .argument('[destination]', 'destination file', 'out.txt')
    .action((source, destination) => {
      parsedSource = source;
    });
  prog4.parse(['input.txt'], { from: 'user' });
  assert.equal(parsedSource, 'input.txt');

  // Sub-commands
  const prog5 = new Command();
  let subCmdRan = false;
  let subCmdName = '';

  prog5
    .command('serve')
    .option('--host <addr>', 'bind address', 'localhost')
    .action((options) => {
      subCmdRan = true;
      subCmdName = 'serve';
      assert.equal(options.host, '0.0.0.0');
    });

  prog5
    .command('build')
    .option('--minify', 'minify output')
    .action(() => {
      subCmdRan = true;
      subCmdName = 'build';
    });

  prog5.parse(['serve', '--host', '0.0.0.0'], { from: 'user' });
  assert.equal(subCmdRan, true, 'sub-command action should run');
  assert.equal(subCmdName, 'serve');

  // Option with custom processing function
  const prog6 = new Command();
  prog6.option(
    '-n, --number <value>',
    'a number',
    (val) => parseInt(val, 10),
    0
  );
  prog6.parse(['-n', '42'], { from: 'user' });
  assert.equal(prog6.opts().number, 42);
  assert.equal(typeof prog6.opts().number, 'number');

  // Option class can be used directly
  const opt = new Option('-d, --debug', 'enable debug mode');
  assert.equal(opt.long, '--debug');
  assert.equal(opt.short, '-d');

  // Argument class
  if (Argument) {
    const arg = new Argument('<file>', 'input file');
    assert.equal(arg.required, true);
    assert.equal(arg.description, 'input file');
  }

  console.log('commander-test:ok');
})().catch((err) => {
  console.error('commander-test:fail');
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
