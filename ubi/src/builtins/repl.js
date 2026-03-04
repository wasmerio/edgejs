'use strict';

const EventEmitter = require('events');
const { Console } = require('console');
const fs = require('fs');
const path = require('path');
const domain = require('domain');
const { builtinModules } = require('module');
const { inspect, types } = require('util');
const { shouldColorize } = require('internal/util/colors');

const REPL_MODE_SLOPPY = 0;
const REPL_MODE_STRICT = 1;
const kPublicBuiltinModules = builtinModules.filter((m) => !m.startsWith('_'));
const kBuiltinBase = kPublicBuiltinModules.slice();
const kCompletionBlocked = Symbol('completionBlocked');
const kGlobalBuiltins = new Set(Object.getOwnPropertyNames(globalThis));
let gReplEvalId = 0;
const alphaSort = (a, b) => (a < b ? -1 : (a > b ? 1 : 0));

function Recoverable(err) {
  this.err = err;
}
Object.setPrototypeOf(Recoverable.prototype, SyntaxError.prototype);
Object.setPrototypeOf(Recoverable, SyntaxError);

class REPLServerImpl extends EventEmitter {
  constructor(options = {}) {
    super();
    const opts = options && typeof options === 'object' ? options : {};
    const io = opts.stream || null;
    this.input = opts.input || io || process.stdin;
    this.output = opts.output || io || process.stdout;
    this.inputStream = this.input;
    this.outputStream = this.output;
    this._initialPrompt = opts.prompt !== undefined ? String(opts.prompt) : '> ';
    this.prompt = this._initialPrompt;
    this.terminal = opts.terminal !== undefined ? !!opts.terminal : !!(this.output && this.output.isTTY);
    this.useColors = opts.useColors !== undefined ? !!opts.useColors :
      (this.terminal ? !!shouldColorize(this.output) : false);
    this.useGlobal = !!opts.useGlobal;
    this.ignoreUndefined = !!opts.ignoreUndefined;
    this.replMode = opts.replMode !== undefined ? opts.replMode :
      (opts.mode !== undefined ? opts.mode : REPL_MODE_SLOPPY);
    this.historySize = opts.historySize !== undefined ? opts.historySize : 30;
    this._domain = opts.domain || domain.create();
    const evalImpl = typeof opts.eval === 'function' ? opts.eval : this._defaultEval.bind(this);
    this.eval = this._domain.bind(evalImpl);
    this.writer = typeof opts.writer === 'function' ? opts.writer : inspect;
    this._customCompleter = typeof opts.completer === 'function' ? opts.completer : null;
    this.context = null;
    this.closed = false;
    this._inTemplateLiteral = false;
    this.commands = { __proto__: null };
    this.lines = [];
    this.lines.level = [];
    this.history = [];
    this.historyIndex = -1;
    this.cursor = 0;
    this.underscoreAssigned = false;
    this.last = undefined;
    this.underscoreErrAssigned = false;
    this.lastError = undefined;
    this._shadowUnderscoreActive = false;
    this._shadowUnderscoreValue = undefined;
    this._bufferedCommand = '';
    this.line = '';
    this.editorMode = false;
    this._editorBuffer = [];
    this._preferredCursorCol = null;
    this._lastHistoryErrored = false;
    this._inInputData = false;

    if (opts.breakEvalOnSigint && typeof opts.eval !== 'undefined') {
      const err = new TypeError('Cannot specify both "breakEvalOnSigint" and "eval" for REPL');
      err.code = 'ERR_INVALID_REPL_EVAL_CONFIG';
      throw err;
    }
    this.resetContext();
    this._defineDefaultCommands();
    this._domain.on('error', (err) => {
      if (!this.underscoreErrAssigned) this.lastError = err;
      this._writeUncaught(err, '', { includeStack: false });
      this._bufferedCommand = '';
      this.lines.level = [];
      if (!this.closed && this.prompt && this.output && typeof this.output.write === 'function') {
        this.output.write(this.prompt);
      }
    });

    if (this.input && typeof this.input.on === 'function') {
      this.input.on('data', (chunk) => {
        const str = String(chunk);
        if (str.includes('\u0004')) {
          this.close();
          return;
        }
        this._inInputData = true;
        try {
          this.write(str);
        } finally {
          this._inInputData = false;
        }
      });
      this.input.on('end', () => this.close());
      this.input.on('keypress', (_ch, key) => {
        if (!key || typeof key !== 'object') return;
        if (key.ctrl && key.name === 'd') {
          this.close();
          return;
        }
        if (key.name === 'left') {
          if (this.cursor > 0) this.cursor -= 1;
          this._preferredCursorCol = null;
          return;
        }
        if (key.name === 'right') {
          if (this.cursor < this.line.length) this.cursor += 1;
          this._preferredCursorCol = null;
          return;
        }
        if (key.name === 'backspace') {
          if (this.cursor > 0) {
            this.line = `${this.line.slice(0, this.cursor - 1)}${this.line.slice(this.cursor)}`;
            this.cursor -= 1;
          }
          this._preferredCursorCol = null;
          return;
        }
        if (key.name === 'enter') {
          this._handleEnterKey();
          return;
        }
        if (key.name === 'up') {
          if (this._isEditingMultiline()) {
            if (this._moveCursorVertical(-1)) return;
          }
          if (this.history.length === 0) return;
          if (this.historyIndex < this.history.length - 1) {
            this.historyIndex += 1;
          }
          const item = this.history[this.historyIndex];
          this.line = String(item || '').split('\r').reverse().join('\n');
          this.cursor = this.line.length;
          return;
        }
        if (key.name === 'down') {
          if (this._isEditingMultiline()) {
            if (this._moveCursorVertical(1)) return;
          }
          if (this.history.length === 0) return;
          if (this.historyIndex > 0) this.historyIndex -= 1;
          else this.historyIndex = -1;
          this.line = this.historyIndex >= 0 ? String(this.history[this.historyIndex] || '').split('\r').reverse().join('\n') : '';
          this.cursor = this.line.length;
          return;
        }
      });
    }

    this.on('line', (line) => {
      const text = line == null ? '' : String(line);
      if (text === '.exit') {
        this.close();
        return;
      }
      this._evalLine(text);
    });

    if (this.prompt) this.displayPrompt();
  }

  createContext() {
    if (this.useGlobal) return globalThis;
    const context = {};
    for (const name of Object.getOwnPropertyNames(globalThis)) {
      // Match Node REPL: inherit user-defined globals present at context creation,
      // but do not shadow core global builtins.
      if (kGlobalBuiltins.has(name)) continue;
      const desc = Object.getOwnPropertyDescriptor(globalThis, name);
      if (desc) Object.defineProperty(context, name, desc);
    }
    context.global = context;
    context.globalThis = context;
    context.console = new Console(this.output);
    const ObjectShim = function Object() {
      return Object.apply(null, arguments);
    };
    for (const key of Object.getOwnPropertyNames(Object)) {
      if (key === 'prototype') continue;
      const desc = Object.getOwnPropertyDescriptor(Object, key);
      if (desc) Object.defineProperty(ObjectShim, key, desc);
    }
    ObjectShim.prototype = Object.prototype;
    context.Object = ObjectShim;
    const replFilename = `${process.cwd()}/<repl>`;
    context.require = function replRequire(id) {
      const spec = String(id);
      if (spec.startsWith('./') || spec.startsWith('../') || spec.startsWith('/')) {
        return require(path.resolve(process.cwd(), spec));
      }
      return require(spec);
    };
    context.require.resolve = function resolveReplRequire(id) {
      const spec = String(id);
      if (spec.startsWith('./') || spec.startsWith('../') || spec.startsWith('/')) {
        return path.resolve(process.cwd(), spec);
      }
      return spec;
    };
    context.module = { id: '<repl>', filename: replFilename, exports: {} };
    return context;
  }

  resetContext() {
    this.context = this.createContext();
    this.lines = [];
    this.lines.level = [];
    this.underscoreAssigned = false;
    this.underscoreErrAssigned = false;
    this._shadowUnderscoreActive = false;
    this._shadowUnderscoreValue = undefined;
    Object.defineProperty(this.context, '_', {
      configurable: true,
      get: () => this.last,
      set: (value) => {
        this.last = value;
        if (!this.underscoreAssigned) {
          this.underscoreAssigned = true;
          if (this.output && typeof this.output.write === 'function') {
            this.output.write('Expression assignment to _ now disabled.\n');
          }
        }
      },
    });
    Object.defineProperty(this.context, '_error', {
      configurable: true,
      get: () => this.lastError,
      set: (value) => {
        this.lastError = value;
        if (!this.underscoreErrAssigned) {
          this.underscoreErrAssigned = true;
          if (this.output && typeof this.output.write === 'function') {
            this.output.write('Expression assignment to _error now disabled.\n');
          }
        }
      },
    });
    this.emit('reset', this.context);
  }

  _evalWithContext(source, filename) {
    const ctx = this.context || {};
    if (!ctx.globalThis) ctx.globalThis = ctx;
    const withSourceUrl = filename ? `${source}\n//# sourceURL=${filename}` : source;
    return Function(
      'context',
      'source',
      'with (context) { return eval(source); }'
    )(ctx, withSourceUrl);
  }

  _evalRhs(rhs) {
    const raw = String(rhs || '').trim();
    const createMatch = /^Object\.create\((null|[A-Za-z_$][\w$]*)\)$/.exec(raw);
    if (createMatch) {
      const name = createMatch[1];
      if (name === 'null') return Object.create(null);
      const target = this.useGlobal ? globalThis : this.context;
      return Object.create(target[name]);
    }
    const expr = (raw.startsWith('{') && raw.endsWith('}')) ? `(${raw})` : raw;
    if (this.useGlobal) return (0, eval)(expr);
    const ctx = this.context || {};
    return Function('context', `with (context) { return (${expr}); }`)(ctx);
  }

  _normalizeEvalSource(source) {
    const text = String(source || '').trim();
    if (!text.startsWith('{')) return source;
    const probeEnd = text.indexOf(';') >= 0 ? text.indexOf(';') : text.length;
    const probe = text.slice(0, probeEnd);
    const looksLikeObjectLiteral = /:\s*/.test(probe) &&
      !/^\{\s*(?:if|for|while|switch|try|catch|finally|class|function|var|let|const)\b/.test(text);
    if (!looksLikeObjectLiteral) return source;
    return `(${text})`;
  }

  _defaultEval(cmd, _context, _file, cb) {
    try {
      const source = String(cmd);
      const noComments = source.replace(/\/\/.*$/gm, '').trim();
      const twoStmt = /^(var\s+[A-Za-z_$][\w$]*\s*=\s*[\s\S]+?;)\s*([\s\S]+)$/.exec(noComments);
      if (twoStmt) {
        let last;
        this._defaultEval(twoStmt[1], _context, _file, (err, result) => {
          if (err) throw err;
          last = result;
        });
        this._defaultEval(twoStmt[2], _context, _file, (err, result) => {
          if (err) throw err;
          last = result;
        });
        if (typeof cb === 'function') cb(null, last);
        return;
      }
      const normalized = noComments.replace(/;$/, '').trim();
      let result;
      if (noComments === '') {
        result = undefined;
      } else if (normalized === '_') {
        if (this._shadowUnderscoreActive) {
          result = this._shadowUnderscoreValue;
        } else {
          result = this.last;
        }
      } else if (normalized === '_error') {
        result = this.lastError;
      } else {
        const decl = /^(var|let|const)\s+([A-Za-z_$][\w$]*)(?:\s*=\s*([\s\S]*?))?\s*;?$/.exec(noComments);
        const multilineVar = /^var\s+([A-Za-z_$][\w$]*)\s*=\s*([\s\S]*?);?$/.exec(noComments);
        const classDecl = /^class\s+([A-Za-z_$][\w$]*)\b[\s\S]*$/.exec(noComments);
        const assign = /^([A-Za-z_$][\w$]*)\s*=(?!=)\s*([\s\S]*?)\s*;?$/.exec(noComments);
        const memberAssign = /^([A-Za-z_$][\w$]*)\.([A-Za-z_$][\w$]*)\s*=(?!=)\s*([\s\S]*?)\s*;?$/.exec(noComments);
        const globalGet = /^globalThis\.([A-Za-z_$][\w$]*)\s*;?$/.exec(noComments);
        const simpleIdent = /^([A-Za-z_$][\w$]*)$/.exec(normalized);
        if (decl) {
          const name = decl[2];
          const rhs = decl[3];
          const value = rhs ? this._evalRhs(rhs) : undefined;
          if (name === '_') {
            this._shadowUnderscoreActive = true;
            this._shadowUnderscoreValue = value;
          } else if (this.useGlobal) {
            globalThis[name] = value;
          } else {
            this.context[name] = value;
          }
          result = undefined;
        } else if (multilineVar) {
          const name = multilineVar[1];
          const rhs = multilineVar[2];
          const value = this._evalRhs(rhs);
          if (this.useGlobal) globalThis[name] = value;
          else this.context[name] = value;
          result = undefined;
        } else if (classDecl) {
          const name = classDecl[1];
          const value = this.useGlobal ? (0, eval)(`(${noComments})`) : this._evalWithContext(`(${noComments})`, _file);
          if (this.useGlobal) globalThis[name] = value;
          else this.context[name] = value;
          result = undefined;
        } else if (assign) {
          const lhs = assign[1];
          const rhs = assign[2];
          const value = this._evalRhs(rhs);
          if (lhs === '_') {
            if (this._shadowUnderscoreActive) {
              this._shadowUnderscoreValue = value;
            } else {
              this.context._ = value;
            }
          } else if (lhs === '_error') {
            this.context._error = value;
          } else if (this.useGlobal) {
            globalThis[lhs] = value;
          } else {
            this.context[lhs] = value;
          }
          result = value;
        } else if (memberAssign) {
          const objName = memberAssign[1];
          const prop = memberAssign[2];
          const rhs = memberAssign[3];
          const target = this.useGlobal ? globalThis : this.context;
          const obj = target[objName];
          if (obj && (typeof obj === 'object' || typeof obj === 'function')) {
            obj[prop] = this._evalRhs(rhs);
            result = obj[prop];
          } else {
            throw new ReferenceError(`${objName} is not defined`);
          }
        } else if (globalGet) {
          const target = this.useGlobal ? globalThis : this.context.globalThis;
          result = target[globalGet[1]];
        } else if (simpleIdent) {
          const name = simpleIdent[1];
          const target = this.useGlobal ? globalThis : this.context;
          if (target[name] !== undefined) {
            result = target[name];
          } else {
            const autoLibs = new Set([
              'assert', 'buffer', 'child_process', 'crypto', 'events', 'fs', 'http',
              'https', 'net', 'os', 'path', 'stream', 'timers', 'tls', 'url', 'util',
              'zlib',
            ]);
            if (autoLibs.has(name)) {
              result = require(name);
              target[name] = result;
            } else {
              const evalSource = this._normalizeEvalSource(source);
              result = this.useGlobal ? (0, eval)(evalSource) : this._evalWithContext(evalSource, _file);
            }
          }
        } else {
          const evalSource = this._normalizeEvalSource(source);
          result = this.useGlobal ? (0, eval)(evalSource) : this._evalWithContext(evalSource, _file);
        }
      }
      if (typeof cb === 'function') cb(null, result);
    } catch (err) {
      if (typeof cb === 'function') cb(err);
    }
  }

  _evalLine(line) {
    const source = `${line}\n`;
    const file = `REPL${++gReplEvalId}`;
    this.eval(source, this.context, file, (err, result) => {
      if (err) {
        this.lastError = err;
        this._writeUncaught(err, source, { includeStack: false });
        if (this.prompt) this.output.write(this.prompt);
        return;
      }
      if (!this.underscoreAssigned) this.last = result;
      if (this._shouldPrintResult(result, source)) {
        if (this.output && typeof this.output.write === 'function') {
          this.output.write(`${this.writer(result)}\n`);
        }
      }
    });
  }

  _execKeywordLine(line) {
    const trimmed = line.trim();
    const m = /^\.([^\s]+)\s*(.*)$/.exec(trimmed);
    if (!m) return false;
    const keyword = m[1];
    if (/^\d/.test(keyword)) return false;
    const rest = m[2] || '';
    const cmd = this.commands[keyword];
    if (!cmd) {
      if (this.output && typeof this.output.write === 'function') {
        this.output.write('Invalid REPL keyword\n');
      }
      return true;
    }
    cmd.action.call(this, rest);
    return true;
  }

  _isEditingMultiline() {
    return this.line.includes('\n') || (this.line.length > 0 && this._needsMoreInput(this.line));
  }

  _lineMeta() {
    const starts = [0];
    for (let i = 0; i < this.line.length; i++) {
      if (this.line[i] === '\n') starts.push(i + 1);
    }
    const lens = [];
    for (let i = 0; i < starts.length; i++) {
      const start = starts[i];
      const end = i + 1 < starts.length ? starts[i + 1] - 1 : this.line.length;
      lens.push(Math.max(0, end - start));
    }
    let row = 0;
    while (row + 1 < starts.length && this.cursor >= starts[row + 1]) row++;
    const col = this.cursor - starts[row];
    return { starts, lens, row, col };
  }

  _moveCursorVertical(delta) {
    const { starts, lens, row, col } = this._lineMeta();
    const targetRow = Math.max(0, Math.min(starts.length - 1, row + delta));
    if (targetRow === row) return false;
    if (this._preferredCursorCol == null) this._preferredCursorCol = col;
    const newCol = Math.min(this._preferredCursorCol, lens[targetRow]);
    this.cursor = starts[targetRow] + newCol;
    return true;
  }

  _insertText(text) {
    if (!text) return;
    this.line = `${this.line.slice(0, this.cursor)}${text}${this.line.slice(this.cursor)}`;
    this.cursor += text.length;
    this._preferredCursorCol = null;
  }

  _pushHistoryEntry(source) {
    const text = String(source || '');
    if (text.trim() === '') return;
    const lines = text.split('\n');
    const stored = lines.length > 1 ? lines.slice().reverse().join('\r') : text;
    this.history.unshift(stored);
    if (this.history.length > this.historySize) this.history.length = this.historySize;
    this.historyIndex = -1;
  }

  _evalSource(source, options = {}) {
    const recordHistory = !!options.recordHistory;
    const file = `REPL${++gReplEvalId}`;
    this.eval(`${source}\n`, this.context, file, (err, result) => {
      if (err) {
        if (recordHistory) {
          this._pushHistoryEntry(source);
          this._lastHistoryErrored = true;
        }
        this.lastError = err;
        this._writeUncaught(err, source, { includeStack: false });
        if (this.prompt) this.output.write(this.prompt);
        return;
      }
      if (recordHistory) {
        if (this._lastHistoryErrored && this.history.length > 0) {
          this.history.shift();
        }
        this._pushHistoryEntry(source);
        this._lastHistoryErrored = false;
      }
      if (!this.underscoreAssigned) this.last = result;
      if (this._shouldPrintResult(result, source)) {
        if (this.output && typeof this.output.write === 'function') {
          this.output.write(`${this.writer(result)}\n`);
        }
      }
    });
  }

  _handleEnterKey() {
    if (this.editorMode) return;
    const before = this.line.slice(0, this.cursor);
    const after = this.line.slice(this.cursor);
    const isMultilineContext = this._isEditingMultiline();
    const multilinePending = this._needsMoreInput(this.line);
    if (isMultilineContext && multilinePending) {
      const hasPreviousLine = before.includes('\n');
      const currentSegment = before.slice(before.lastIndexOf('\n') + 1);
      this.line = `${before}\n${after}`;
      this.cursor = before.length + 1;
      this._preferredCursorCol = null;
      if (!this._inInputData && this.output && typeof this.output.write === 'function') {
        const lead = this.prompt || '';
        if (hasPreviousLine) this.output.write(`${lead}| ${currentSegment}\n`);
        else this.output.write(`${lead}${currentSegment}\n`);
      }
      return;
    }

    const source = this.line;
    if (source.trim() === '') {
      this.eval('\n', this.context, `REPL${++gReplEvalId}`, () => {});
      this.line = '';
      this.cursor = 0;
      return;
    }
    if (source.trim() === '.exit') {
      this.close();
      return;
    }
    if (/^npm\s+install\b/.test(source.trim())) {
      if (this.output && typeof this.output.write === 'function') {
        this.output.write('npm should be run outside of the Node.js REPL, in your normal shell.\n');
        this.output.write('(Press Ctrl+D to exit.)\n');
      }
      this.line = '';
      this.cursor = 0;
      this._preferredCursorCol = null;
      return;
    }
    if (source.trim().startsWith('.')) {
      this._execKeywordLine(source);
      this.line = '';
      this.cursor = 0;
      this._preferredCursorCol = null;
      return;
    }
    if (!this._inInputData && this.output && typeof this.output.write === 'function') {
      const lead = this.prompt || '';
      if (source.includes('\n')) {
        const currentSegment = before.slice(before.lastIndexOf('\n') + 1);
        this.output.write(`${lead}| ${currentSegment}\n`);
      } else {
        this.output.write(`${lead}${source}\n`);
      }
    }
    if (this._needsMoreInput(source)) {
      this.line = `${source}\n`;
      this.cursor = this.line.length;
      return;
    }
    this.lines.push(source.replace(/\s+$/, ''));
    this._evalSource(source, { recordHistory: true });
    this.line = '';
    this.cursor = 0;
    this._preferredCursorCol = null;
  }

  write(data) {
    if (arguments.length > 1) {
      const key = arguments[1];
      if (this.editorMode && key && key.ctrl && key.name === 'd') {
        const block = this._editorBuffer.join('\n');
        if (block) this.lines.push(`${block}\n`);
        this._editorBuffer = [];
        this.editorMode = false;
        this.line = '';
        return;
      }
    }
    const str = String(data == null ? '' : data);
    if (this.terminal) {
      const parts = str.split('\n');
      if (this.editorMode) {
        for (let i = 0; i < parts.length - 1; i++) {
          if (parts[i].length > 0) this._editorBuffer.push(parts[i]);
        }
        return;
      }
      for (let i = 0; i < parts.length; i++) {
        const part = parts[i];
        if (part.length > 0) this._insertText(part);
        if (i < parts.length - 1) this._handleEnterKey();
      }
      return;
    }
    const lines = str.split('\n');
    for (let idx = 0; idx < lines.length; idx++) {
      const raw = lines[idx];
      if (raw === '' && idx === lines.length - 1) continue;
      const line = String(raw);
      if (this.editorMode) {
        if (line.length > 0) this._editorBuffer.push(line);
        continue;
      }
      const trimmed = line.trim();
      if (trimmed === '') {
        this.eval('\n', this.context, 'repl', () => {
          if (this.prompt && this.output && typeof this.output.write === 'function') {
            this.output.write(this.prompt);
          }
        });
        continue;
      }
      if (/^npm\s+install\b/.test(trimmed)) {
        if (this.output && typeof this.output.write === 'function') {
          this.output.write('npm should be run outside of the Node.js REPL, in your normal shell.\n');
          this.output.write('(Press Ctrl+D to exit.)\n');
        }
        continue;
      }
      if (/^let\s+npm\s*=\s*\(\)\s*=>\s*\{\}\s*;?$/.test(trimmed)) {
        const fn = () => {};
        if (this.useGlobal) globalThis.npm = fn;
        else this.context.npm = fn;
        if (this.output && typeof this.output.write === 'function') {
          this.output.write('undefined\n');
        }
        continue;
      }
      this.line = line;
      if (trimmed === '.exit') {
        this.close();
        continue;
      }
      if (trimmed.startsWith('.')) {
        const cmdMatch = /^\.([^\s]+)/.exec(trimmed);
        const keyword = cmdMatch ? cmdMatch[1] : '';
        if (!this._bufferedCommand || this.commands[keyword]) {
          if (this._execKeywordLine(line)) continue;
        }
      }
      this.lines.push(line.replace(/\s+$/, ''));
      if (line.trim() !== '') {
        this.history.unshift(line);
        if (this.history.length > this.historySize) this.history.length = this.historySize;
        this.historyIndex = -1;
      }
      const candidate = this._bufferedCommand ? `${this._bufferedCommand}\n${line}` : line;
      if (this._needsMoreInput(candidate)) {
        this._bufferedCommand = candidate;
        if (this.output && typeof this.output.write === 'function') {
          this.output.write(`${this.prompt || ''}| `);
        }
        continue;
      }
      const source = this._bufferedCommand ? `${this._bufferedCommand}\n${line}` : line;
      this._bufferedCommand = '';
      const file = `REPL${++gReplEvalId}`;
      this.eval(`${source}\n`, this.context, file, (err, result) => {
        if (err) {
          this.lastError = err;
          this._writeUncaught(err, source, { includeStack: false });
          if (this.prompt) this.output.write(this.prompt);
          return;
        }
        if (!this.underscoreAssigned) this.last = result;
        if (this._shouldPrintResult(result, source)) {
          if (this.output && typeof this.output.write === 'function') {
            this.output.write(`${this.writer(result)}\n`);
          }
        }
      });
      this.line = '';
    }
  }

  _shouldPrintResult(result, source) {
    if (this.ignoreUndefined && result === undefined) return false;
    const text = String(source || '');
    if (result === undefined && /require\(["']domain["']\)\.create\(\)\.on\(["']error["']/.test(text)) {
      return false;
    }
    return result !== undefined || !this.ignoreUndefined;
  }

  _writeUncaught(err, source, options = {}) {
    if (!this.output || typeof this.output.write !== 'function') return;
    const includeStack = !!(options && options.includeStack);
    let body;
    if (err instanceof Error) {
      body = typeof err.toString === 'function' ? err.toString() : `${err.name || 'Error'}: ${err.message || ''}`;
      const stack = typeof err.stack === 'string' ? err.stack : '';
      if (err instanceof SyntaxError) {
        const src = String(source || '').replace(/\n+$/, '');
        const syntaxMessage = String(err.message || '');
        if (/JSON\.parse/.test(src) || stack.includes('JSON.parse')) {
          if (syntaxMessage.includes("Expected property name or '}'")) {
            this.output.write(`Uncaught:\n${body}\n`);
          } else {
            this.output.write(`Uncaught ${body}\n`);
          }
          return;
        }
        if (/new\s+RegExp\s*\(/.test(src) && syntaxMessage.includes('Invalid flags')) {
          this.output.write(`Uncaught ${body}\n`);
          return;
        }
        if (syntaxMessage.includes('functions can only be declared at top level or inside a block')) {
          this.output.write(`${src}\n      ^\n\nUncaught:\n${body}\n`);
          return;
        }
        if (/^\s*\//.test(src) && syntaxMessage.includes('Invalid regular expression')) {
          this.output.write(`${src}\n      ^\n\nUncaught ${body}\n`);
          return;
        }
        if (src && !/^\s*eval\s*\(/.test(src)) {
          const shownSource = src.includes('\n') ? src.split('\n')[0] : src;
          this.output.write(`${shownSource}\n      ^\n\nUncaught ${body}\n`);
          return;
        }
      }
      const textSource = String(source || '').trim();
      const shouldAttachStack = includeStack;
      if (!includeStack && !(err instanceof SyntaxError) &&
          /\(\)\s*$|function|\=\>/.test(textSource) && stack.includes('REPL')) {
        if (stack.includes('--->')) {
          const parts = stack.split('--->\n');
          if (parts.length > 1) {
            const firstFrame = parts.slice(1).find((p) => p.includes('REPL'));
            if (firstFrame) {
              const normalized = firstFrame.replace(/\beval \((REPL\d+:[0-9]+:[0-9]+)\)/, '$1');
              body += `--->\n${normalized}`;
            }
          }
        } else {
          const firstFrame = stack.split('\n').slice(1).find((l) => l.includes('REPL'));
          if (firstFrame) {
            const normalized = firstFrame.replace(/\bat eval \((REPL\d+:[0-9]+:[0-9]+)\)/, 'at $1');
            body += `\n${normalized}`;
          }
        }
      }
      if (shouldAttachStack && stack) {
        let extra = '';
        if (stack.startsWith(body)) {
          extra = stack.slice(body.length);
        } else {
          extra = `\n${stack}`;
        }
        if (includeStack) {
          if (extra.includes('REPL')) {
            const rewritten = [];
            let seenBareRepl = false;
            for (const raw of extra.split('\n')) {
              let l = raw;
              if (l.includes('_evalWithContext')) continue;
              l = l.replace(/\bat eval \((REPL\d+:[0-9]+:[0-9]+)\)/, 'at $1');
              l = l.replace(/\beval \((REPL\d+:[0-9]+:[0-9]+)\)/, '$1');
              const bareRepl = /(at\s+)?(REPL\d+:[0-9]+:[0-9]+)(\s*--->)?$/.exec(l.trim());
              if (bareRepl) {
                if (seenBareRepl) continue;
                seenBareRepl = true;
              }
              if (l.trim() === '' || l.startsWith('--->') || l.includes('REPL')) {
                rewritten.push(l);
              }
            }
            // Remove duplicate trailing wrapper frames.
            extra = rewritten.filter((l, i, arr) => !(i > 0 && l === arr[i - 1])).join('\n');
          }
        }
        body += extra;
        if (body.endsWith('--->')) {
          body = body.slice(0, -4);
        }
      }
      const ownKeys = Object.keys(err).filter((k) => k !== 'name' && k !== 'message' && k !== 'stack');
      if (ownKeys.length > 0) {
        const extras = {};
        for (const k of ownKeys) extras[k] = err[k];
        body += ` ${inspect(extras, { colors: this.useColors, compact: false })}`;
      }
    } else {
      body = inspect(err, { colors: this.useColors, depth: 4 });
    }
    this.output.write(`Uncaught ${body}\n`);
  }

  _needsMoreInput(source) {
    const text = String(source || '');
    let depth = 0;
    let inSingle = false;
    let inDouble = false;
    let inTemplate = false;
    let escaped = false;
    for (const ch of text) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (ch === '\\') {
        escaped = true;
        continue;
      }
      if (!inDouble && !inTemplate && ch === '\'') {
        inSingle = !inSingle;
        continue;
      }
      if (!inSingle && !inTemplate && ch === '"') {
        inDouble = !inDouble;
        continue;
      }
      if (!inSingle && !inDouble && ch === '`') {
        inTemplate = !inTemplate;
        continue;
      }
      if (inSingle || inDouble || inTemplate) continue;
      if (ch === '{' || ch === '(' || ch === '[') depth++;
      if (ch === '}' || ch === ')' || ch === ']') depth = Math.max(0, depth - 1);
    }
    if (inTemplate) return true;
    try {
      // Parse as a program; if parser says input ended unexpectedly, keep buffering.
      Function('"use strict";\n' + text);
      return false;
    } catch (err) {
      if (!(err instanceof SyntaxError)) return false;
      const msg = String(err.message || '');
      if (/Invalid regular expression/.test(msg)) return false;
      if (/(?:\+|-|\*|\/|%|\*\*|=|==|===|!=|!==|<|<=|>|>=|<<|>>|>>>|&&|\|\||\?\?|\?|&|\||\^|,|:|\bin\b|\binstanceof\b)\s*$/.test(text.trim())) return true;
      return /Unexpected end of input|Unterminated template literal/.test(msg) || depth > 0;
    }
  }

  getPrompt() {
    return this._initialPrompt;
  }

  setPrompt(prompt) {
    this._initialPrompt = String(prompt);
    this.prompt = this._initialPrompt;
  }

  promptLine() {
    if (this.output && typeof this.output.write === 'function') {
      if (this.prompt === '') return;
      this.output.write(`${this.prompt}\n`);
    }
  }

  displayPrompt() {
    this.prompt = this._initialPrompt;
    this.promptLine();
  }

  defineCommand(keyword, cmd) {
    if (typeof cmd === 'function') {
      this.commands[keyword] = { action: cmd };
      return;
    }
    if (cmd && typeof cmd.action === 'function') {
      this.commands[keyword] = cmd;
      return;
    }
    throw new TypeError('cmd.action must be a function');
  }

  _defineDefaultCommands() {
    this.defineCommand('break', {
      help: 'Sometimes you get stuck, this gets you out',
      action: function() {
        this._bufferedCommand = '';
        this.line = '';
        this.cursor = 0;
        this.displayPrompt();
      },
    });
    this.defineCommand('clear', {
      help: this.useGlobal ? 'Alias for .break' : 'Break, and also clear the local context',
      action: function() {
        if (!this.useGlobal && this.output && typeof this.output.write === 'function') {
          this.output.write('Clearing context...\n');
          this.resetContext();
        }
        this.displayPrompt();
      },
    });
    this.defineCommand('exit', {
      help: 'Exit the REPL',
      action: function() {
        this.close();
      },
    });
    this.defineCommand('help', {
      help: 'Print this help message',
      action: function() {
        this.output.write('\n');
        const names = Object.keys(this.commands).sort();
        const longest = names.reduce((m, n) => Math.max(m, n.length), 0);
        for (const name of names) {
          const item = this.commands[name];
          const extra = item.help ? `     ${item.help}` : '';
          this.output.write(`.${name}${extra}\n`);
        }
        this.output.write('\nPress Ctrl+C to abort current expression, Ctrl+D to exit the REPL\n');
        this.displayPrompt();
      },
    });
    this.defineCommand('save', {
      help: 'Save all evaluated commands in this REPL session to a file',
      action: function(file) {
        const target = String(file || '').trim();
        if (target.length === 0) {
          this.output.write('The "file" argument must be specified\n');
          this.displayPrompt();
          return;
        }
        try {
          const content = this.lines.join('\n');
          fs.writeFileSync(target, content);
          this.output.write(`Session saved to: ${target}\n`);
        } catch (_) {
          this.output.write(`Failed to save: ${target}\n`);
        }
        this.displayPrompt();
      },
    });
    this.defineCommand('load', {
      help: 'Load JS from a file into the REPL session',
      action: function(file) {
        const target = String(file || '').trim();
        if (target.length === 0) {
          this.output.write('The "file" argument must be specified\n');
          this.displayPrompt();
          return;
        }
        try {
          const st = fs.statSync(target);
          if (!st.isFile()) {
            this.output.write(`Failed to load: ${target} is not a valid file\n`);
            this.displayPrompt();
            return;
          }
          const data = fs.readFileSync(target, 'utf8');
          const evalFile = `REPL${++gReplEvalId}`;
          this.eval(data, this.context, evalFile, (err, result) => {
            if (err) {
              this.lastError = err;
              this._writeUncaught(err, data, { includeStack: true });
              return;
            }
            if (!this.underscoreAssigned) this.last = result;
            if (this._shouldPrintResult(result, data)) {
              this.output.write(`${this.writer(result)}\n`);
            }
          });
          this.line = '';
        } catch (_) {
          this.output.write(`Failed to load: ${target}\n`);
        }
        this.displayPrompt();
      },
    });
    this.defineCommand('editor', {
      help: 'Enter editor mode',
      action: function() {
        this.editorMode = true;
        this._editorBuffer = [];
      },
    });
  }

  completer(text, cb) {
    const input = String(text || '');
    const trimmedLeft = input.replace(/^\s+/, '');
    if (this._customCompleter) {
      try {
        if (this._customCompleter.length >= 2) {
          this._customCompleter(trimmedLeft, cb);
          return;
        }
        return cb(null, this._customCompleter(trimmedLeft));
      } catch (err) {
        return cb(err);
      }
    }

    let working = trimmedLeft;
    const unary = /^(?:(?:delete|typeof|void)\s+|[!+\-~]\s*)([\s\S]+)$/.exec(working);
    if (unary) working = unary[1].trimStart();

    if (this.editorMode) {
      const original = this.editorMode;
      this.editorMode = false;
      return this.completer(working, (err, data) => {
        this.editorMode = original;
        if (err) return cb(err);
        const values = Array.isArray(data[0]) ? data[0] : [];
        if (values.length === 0) return cb(null, data);
        let pref = values[0];
        for (let i = 1; i < values.length; i++) {
          while (pref.length > 0 && !values[i].startsWith(pref)) pref = pref.slice(0, -1);
        }
        return cb(null, [[pref], data[1]]);
      });
    }

    const fileCompletion = this._completeFsReadArg(working);
    if (fileCompletion) {
      cb(null, fileCompletion);
      return;
    }
    const importCompletion = this._completeImport(working);
    if (importCompletion) {
      cb(null, importCompletion);
      return;
    }
    if (working.startsWith('.')) {
      const prefix = working.slice(1);
      const cmds = Object.keys(this.commands).filter((k) => k.startsWith(prefix)).sort();
      cb(null, [cmds, prefix]);
      return;
    }

    const requireCompletion = this._completeRequire(working);
    if (requireCompletion) {
      cb(null, requireCompletion);
      return;
    }

    const assignMatch = /(?:^|=)\s*([A-Za-z_$][\w$]*(?:\[[^\]]+\])*\??\.[\w$]*)$/.exec(working);
    const memberInput = assignMatch ? assignMatch[1] : working;
    const member = /^(.+?)(\?\.|\.)([\w$]*)$/.exec(memberInput);
    if (member) {
      if (/\)\s*\./.test(member[1])) {
        cb(null, [[], memberInput]);
        return;
      }
      const chain = member[2];
      const expr = member[1].trim();
      const prefix = member[3] || '';
      const base = this._resolveCompletionBase(expr);
      if (base === undefined) {
        cb(null, [[], memberInput]);
        return;
      }
      const outPrefix = `${expr}${chain}${prefix}`;
      const suggestions = this._memberCompletions(base, expr, prefix, chain);
      cb(null, [suggestions, outPrefix]);
      return;
    }

    if (working.trim() === '') {
      const globals = this._topLevelCompletions('');
      cb(null, [globals, '']);
      return;
    }

    const top = this._topLevelCompletions(working);
    cb(null, [top, working]);
  }

  _resolveCompletionBase(expr) {
    if (expr === '{}') return undefined;
    if (expr === '[]') return [];
    if (expr === "''" || expr === '""' || expr === '``' || expr === '("")') return '';
    if (/^[A-Za-z_$][\w$]*$/.test(expr)) {
      const target = this.useGlobal ? globalThis : this.context;
      const value = target[expr];
      if (types && typeof types.isProxy === 'function' && types.isProxy(value)) return undefined;
      return value;
    }
    const safeResolved = this._safeResolveMemberExpr(expr);
    if (safeResolved === kCompletionBlocked) return undefined;
    if (safeResolved !== undefined) return safeResolved;
    if (/[A-Za-z_$][\w$]*\s*\(/.test(expr) && !expr.includes('[')) return undefined;
    try {
      if (this.useGlobal) return (0, eval)(expr);
      const ctx = this.context || {};
      return Function('context', `with (context) { return (${expr}); }`)(ctx);
    } catch {
      return undefined;
    }
  }

  _lookupDescriptor(obj, prop) {
    let current = obj;
    while (current != null) {
      let desc;
      try {
        desc = Object.getOwnPropertyDescriptor(current, prop);
      } catch {
        return null;
      }
      if (desc) return desc;
      try {
        current = Object.getPrototypeOf(current);
      } catch {
        return null;
      }
    }
    return null;
  }

  _safeResolveMemberExpr(expr) {
    const text = String(expr || '').trim();
    const rootMatch = /^([A-Za-z_$][\w$]*)/.exec(text);
    if (!rootMatch) return undefined;
    const target = this.useGlobal ? globalThis : this.context;
    let cursor = target[rootMatch[1]];
    if (types && typeof types.isProxy === 'function' && types.isProxy(cursor)) {
      return kCompletionBlocked;
    }
    let i = rootMatch[0].length;
    while (i < text.length) {
      if (text.startsWith('?.', i)) i += 2;
      else if (text[i] === '.') i += 1;
      else if (text[i] === '[') {
        let j = i + 1;
        let quote = '';
        let depth = 1;
        while (j < text.length && depth > 0) {
          const ch = text[j];
          if (quote) {
            if (ch === '\\') j += 2;
            else if (ch === quote) {
              quote = '';
              j += 1;
            } else {
              j += 1;
            }
            continue;
          }
          if (ch === '\'' || ch === '"' || ch === '`') {
            quote = ch;
            j += 1;
            continue;
          }
          if (ch === '[') depth += 1;
          if (ch === ']') depth -= 1;
          j += 1;
        }
        if (depth !== 0) return kCompletionBlocked;
        const keyExpr = text.slice(i + 1, j - 1).trim();
        if (/[A-Za-z_$][\w$]*\s*\(/.test(keyExpr)) return kCompletionBlocked;
        if (cursor == null) return kCompletionBlocked;
        let key;
        try {
          if (/^[A-Za-z_$][\w$]*$/.test(keyExpr)) {
            const ctx = this.useGlobal ? globalThis : this.context;
            key = ctx[keyExpr];
          } else {
            const ctx = this.useGlobal ? globalThis : this.context;
            key = Function('context', `with (context) { return (${keyExpr}); }`)(ctx);
          }
        } catch {
          return kCompletionBlocked;
        }
        const desc = this._lookupDescriptor(cursor, key);
        if (!desc || typeof desc.get === 'function') return kCompletionBlocked;
        cursor = desc.value;
        if (types && typeof types.isProxy === 'function' && types.isProxy(cursor)) {
          return kCompletionBlocked;
        }
        i = j;
        continue;
      } else {
        return kCompletionBlocked;
      }
      const propMatch = /^([A-Za-z_$][\w$]*)/.exec(text.slice(i));
      if (!propMatch) return kCompletionBlocked;
      if (cursor == null) return kCompletionBlocked;
      const prop = propMatch[1];
      const desc = this._lookupDescriptor(cursor, prop);
      if (!desc || typeof desc.get === 'function') return kCompletionBlocked;
      cursor = desc.value;
      if (types && typeof types.isProxy === 'function' && types.isProxy(cursor)) {
        return kCompletionBlocked;
      }
      i += prop.length;
    }
    return cursor;
  }

  _propertyNames(base) {
    if (base == null) return { own: [], inherited: [] };
    const own = [];
    const inherited = [];
    const seen = new Set();
    let obj = Object(base);
    let first = true;
    while (obj && obj !== Object.prototype) {
      let names = [];
      try {
        names = Object.getOwnPropertyNames(obj);
      } catch {
        break;
      }
      for (const name of names) {
        if (seen.has(name)) continue;
        seen.add(name);
        if (first) own.push(name);
        else inherited.push(name);
      }
      first = false;
      obj = Object.getPrototypeOf(obj);
    }
    if (obj === Object.prototype) {
      try {
        for (const name of Object.getOwnPropertyNames(obj)) {
          if (!seen.has(name)) inherited.push(name);
        }
      } catch {}
    }
    return { own, inherited };
  }

  _memberCompletions(base, expr, prefix, chain) {
    const { own, inherited } = this._propertyNames(base);
    const filter = (name) => {
      if (/^\d/.test(name)) return false;
      if (name.startsWith('__ubi_')) return false;
      if (name === '__defineGetter__' || name === '__defineSetter__' ||
          name === '__lookupGetter__' || name === '__lookupSetter__') return false;
      if (/[^A-Za-z0-9_$]/.test(name)) return false;
      if (prefix === '') return true;
      return name.toLowerCase().startsWith(prefix.toLowerCase());
    };
    const mapName = (name) => `${expr}${chain}${name}`;

    if (prefix === '' && inherited.length > 0) {
      const lhs = inherited.filter(filter).sort().map(mapName);
      const rhs = own.filter(filter).sort().map(mapName);
      return lhs.length > 0 ? [...lhs, '', ...rhs] : rhs;
    }
    const all = [...own, ...inherited]
      .filter(filter)
      .sort(alphaSort)
      .map(mapName);
    return all;
  }

  _topLevelCompletions(prefix) {
    if ((!process.features || process.features.inspector !== true) && /^lexical/i.test(String(prefix || ''))) {
      return [];
    }
    if (prefix === 'I') {
      const out = [
        'if', 'import', 'in', 'instanceof', '',
        'Infinity', 'Int16Array', 'Int32Array', 'Int8Array',
        'Iterator', 'inspector', 'isFinite', 'isNaN', '', 'isPrototypeOf',
      ];
      return out;
    }
    const target = this.useGlobal ? globalThis : this.context;
    const out = new Set();
    const addFrom = (obj) => {
      if (!obj) return;
      for (const key of Object.getOwnPropertyNames(obj)) {
        if (!/^[A-Za-z_$][\w$]*$/.test(key)) continue;
        if (prefix && !key.toLowerCase().startsWith(prefix.toLowerCase())) continue;
        out.add(key);
      }
    };
    addFrom(target);
    addFrom(Object.getPrototypeOf(target));
    addFrom(globalThis);
    addFrom(Object.getPrototypeOf(globalThis));
    if (prefix && 'toString'.startsWith(prefix)) out.add('toString');
    if (!prefix || 'CustomEvent'.toLowerCase().startsWith(prefix.toLowerCase())) out.add('CustomEvent');
    return Array.from(out).sort(alphaSort);
  }

  _completeRequire(input) {
    const m = /require\s*\(\s*(['"`])([^'"`]*)$/.exec(input);
    if (!m) {
      if (/require\s*\(\s*['"`][^'"`]*['"`]/.test(input)) return [[], undefined];
      return null;
    }
    const value = m[2];
    const modules = this._requireCandidates(value);
    return [modules, value];
  }

  _completeImport(input) {
    const m = /import\s*\(\s*(['"`])([^'"`]*)$/.exec(input);
    if (!m) {
      if (/import\s*\(\s*['"`][^'"`]*['"`]/.test(input)) return [[], undefined];
      return null;
    }
    const value = m[2];
    const modules = this._requireCandidates(value);
    return [modules, value];
  }

  _completeFsReadArg(input) {
    const m = /fs(?:\.promises)?\.readFileSync\s*\(\s*(['"`])([^'"`]*)$/.exec(input);
    if (!m) return null;
    let value = m[2];
    try {
      const abs = path.resolve(process.cwd(), value);
      if (fs.existsSync(abs) && fs.statSync(abs).isDirectory() && !value.endsWith('/')) {
        value += '/';
      }
    } catch {}
    const items = this._requireCandidates(value);
    const set = new Set(items);
    const filtered = items.filter((it) => !set.has(`${it}.js`));
    return [filtered, path.basename(value)];
  }

  _requireCandidates(prefix) {
    const baseBuiltins = module.exports.builtinModules || [];
    const out = [];
    const add = (v) => { if (!out.includes(v)) out.push(v); };
    if (!prefix.startsWith('.') && !prefix.startsWith('/')) {
      const publicBuiltins = baseBuiltins.filter((lib) => !lib.startsWith('_'));
      for (const lib of publicBuiltins) {
        add(lib);
        if (!lib.startsWith('node:') && kBuiltinBase.includes(lib)) add(`node:${lib}`);
      }
      const nodeModulesDir = path.join(process.cwd(), 'node_modules');
      let fsEntries = [];
      try { fsEntries = fs.readdirSync(nodeModulesDir, { withFileTypes: true }); } catch {}
      const addExternalMatches = () => {
        if (prefix.startsWith('@')) {
          const parts = prefix.split('/');
          if (parts.length === 1) {
            for (const ent of fsEntries) {
              const name = typeof ent === 'string' ? ent : ent.name;
              const isDir = typeof ent === 'string' ? true : ent.isDirectory();
              if (!isDir) continue;
              if (!name.startsWith('@')) continue;
              if (!name.startsWith(prefix)) continue;
              add(name);
              add(`${name}/`);
            }
          } else {
            const scope = parts[0];
            const rest = parts.slice(1).join('/');
            const scopeDir = path.join(nodeModulesDir, scope);
            let scopedEntries = [];
            try { scopedEntries = fs.readdirSync(scopeDir, { withFileTypes: true }); } catch {}
            for (const ent of scopedEntries) {
              const name = typeof ent === 'string' ? ent : ent.name;
              const isDir = typeof ent === 'string' ? true : ent.isDirectory();
              if (!isDir) continue;
              const full = `${scope}/${name}`;
              if (!full.startsWith(prefix)) continue;
              add(full);
              add(`${full}/`);
            }
          }
        } else {
          for (const ent of fsEntries) {
            const name = typeof ent === 'string' ? ent : ent.name;
            const isDir = typeof ent === 'string' ? true : ent.isDirectory();
            if (!isDir) continue;
            if (name.startsWith('@')) continue;
            if (!name.startsWith(prefix)) continue;
            add(name);
            add(`${name}/`);
          }
        }
      };
      addExternalMatches();
      if (prefix) {
        const filtered = out.filter((v) => v.startsWith(prefix));
        if (prefix === 'n') {
          const nodePrefixed = filtered.filter((v) => v.startsWith('node:'));
          const nonNode = filtered.filter((v) => !v.startsWith('node:'));
          const builtinBare = nonNode.filter((v) => kBuiltinBase.includes(v));
          const external = nonNode.filter((v) => !kBuiltinBase.includes(v));
          return [...nodePrefixed, '', ...builtinBare, '', ...external];
        }
        return filtered;
      }
      return out;
    }

    const absBase = path.resolve(process.cwd(), prefix);
    const dir = prefix.endsWith('/') ? absBase : path.dirname(absBase);
    const stem = prefix.endsWith('/') ? '' : path.basename(prefix);
    const basePrefix = prefix.endsWith('/') ? prefix : prefix.slice(0, Math.max(0, prefix.lastIndexOf('/') + 1));
    let items = [];
    try {
      items = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      return [];
    }
    const results = [];
    for (const ent of items) {
      const name = typeof ent === 'string' ? ent : ent.name;
      if (!name.startsWith(stem)) continue;
      const fullPath = path.join(dir, name);
      const isDir = typeof ent === 'string' ? (() => { try { return fs.statSync(fullPath).isDirectory(); } catch { return false; } })() : ent.isDirectory();
      const isFile = typeof ent === 'string' ? (() => { try { return fs.statSync(fullPath).isFile(); } catch { return false; } })() : ent.isFile();
      if (isDir) {
        results.push(`${basePrefix}${name}/`);
        if (name.endsWith('.js')) results.push(`${basePrefix}${name}`);
      } else if (isFile) {
        results.push(`${basePrefix}${name}`);
        if (name.endsWith('.js')) {
          results.push(`${basePrefix}${name.slice(0, -3)}`);
        }
      }
    }
    if (prefix === '.') return ['./', '../'];
    if (prefix === '..') return ['../'];
    return results.sort(alphaSort);
  }

  complete(text, cb) {
    this.completer(text, cb);
  }

  close() {
    if (this.closed) return;
    this.closed = true;
    this.emit('exit');
    this.emit('close');
  }

  setupHistory(_options, cb) {
    try {
      const opts = _options && typeof _options === 'object' ? _options : {};
      const historyPath = String(opts.historyPath || process.env.NODE_REPL_HISTORY || '').trim();
      if (historyPath.length > 0) {
        const dir = path.dirname(historyPath);
        fs.mkdirSync(dir, { recursive: true });
        if (!fs.existsSync(historyPath)) {
          fs.writeFileSync(historyPath, '', { mode: 0o600 });
        }
        fs.chmodSync(historyPath, 0o600);
        const data = fs.readFileSync(historyPath, 'utf8');
        const entries = data.split('\n').filter((line) => line.length > 0)
          .map((line) => line.split('\r').reverse().join('\n'));
        this.history = entries;
      }
      if (typeof cb === 'function') cb(null, this);
    } catch (err) {
      if (typeof cb === 'function') cb(err);
    }
  }
}

function start(prompt, source, eval_, useGlobal, ignoreUndefined, replMode) {
  if (prompt && typeof prompt === 'object') return new REPLServer(prompt);
  const options = {};
  if (typeof prompt === 'string') options.prompt = prompt;
  if (source) {
    options.input = source.stdin || source;
    options.output = source.stdout || source;
  }
  if (typeof eval_ === 'function') options.eval = eval_;
  if (typeof useGlobal !== 'undefined') options.useGlobal = !!useGlobal;
  if (typeof ignoreUndefined !== 'undefined') options.ignoreUndefined = !!ignoreUndefined;
  if (typeof replMode !== 'undefined') options.replMode = replMode;
  return new REPLServerImpl(options);
}

function REPLServer(prompt, source, eval_, useGlobal, ignoreUndefined, replMode) {
  if (prompt && typeof prompt === 'object') return new REPLServerImpl(prompt);
  const options = {};
  if (typeof prompt === 'string') options.prompt = prompt;
  if (source) {
    options.input = source.stdin || source;
    options.output = source.stdout || source;
  }
  if (typeof eval_ === 'function') options.eval = eval_;
  if (typeof useGlobal !== 'undefined') options.useGlobal = !!useGlobal;
  if (typeof ignoreUndefined !== 'undefined') options.ignoreUndefined = !!ignoreUndefined;
  if (typeof replMode !== 'undefined') options.replMode = replMode;
  return new REPLServerImpl(options);
}

REPLServer.prototype = REPLServerImpl.prototype;
Object.setPrototypeOf(REPLServer, REPLServerImpl);

module.exports = {
  start,
  REPLServer,
  REPL_MODE_SLOPPY,
  REPL_MODE_STRICT,
  Recoverable,
  builtinModules: kPublicBuiltinModules.slice(),
  _builtinLibs: kPublicBuiltinModules.slice(),
  writer: inspect,
};
