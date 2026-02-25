'use strict';

const kEvents = Symbol('kEvents');
const kResistStopPropagation = Symbol('kResistStopPropagation');
const kWeakHandler = Symbol('kWeakHandler');
const kDispatchPath = Symbol('kDispatchPath');
const kTarget = Symbol('kTarget');
const kCurrentTarget = Symbol('kCurrentTarget');
const kEventPhase = Symbol('kEventPhase');
const kCanceled = Symbol('kCanceled');
const kCancelable = Symbol('kCancelable');
const kType = Symbol('kType');
const kDetail = Symbol('kDetail');

function defineEventPhases(Ctor) {
  Object.defineProperties(Ctor, {
    NONE: { value: 0, enumerable: true, writable: false, configurable: false },
    CAPTURING_PHASE: { value: 1, enumerable: true, writable: false, configurable: false },
    AT_TARGET: { value: 2, enumerable: true, writable: false, configurable: false },
    BUBBLING_PHASE: { value: 3, enumerable: true, writable: false, configurable: false },
  });
}

function validateType(type) {
  if (type === undefined || typeof type === 'symbol') {
    throw new TypeError('The "type" argument must be of type string');
  }
}

class Event {
  constructor(type, options = undefined) {
    validateType(type);
    if (options !== undefined && (options === null || typeof options !== 'object')) {
      let shown = String(options).slice(0, 25);
      if (String(options).length > 25) shown += '...';
      if (typeof options === 'string') shown = `'${shown}'`;
      const err = new TypeError(
        `The "options" argument must be of type object. Received type ${typeof options} (${shown})`
      );
      err.code = 'ERR_INVALID_ARG_TYPE';
      throw err;
    }
    this[kType] = String(type);
    this[kCancelable] = !!(options && options.cancelable);
    this[kCanceled] = false;
    this[kTarget] = null;
    this[kCurrentTarget] = null;
    this[kEventPhase] = Event.NONE;
    this[kDispatchPath] = [];
    this.bubbles = false;
    this.composed = false;
    this.isTrusted = false;
    this.timeStamp = Date.now();
  }

  get type() { return this[kType]; }
  get target() { return this[kTarget]; }
  get currentTarget() { return this[kCurrentTarget]; }
  get srcElement() { return this[kTarget]; }
  get cancelable() { return this[kCancelable]; }
  get defaultPrevented() { return this[kCanceled]; }
  get eventPhase() { return this[kEventPhase]; }
  get returnValue() { return !this.defaultPrevented; }
  set returnValue(v) { if (!v) this.preventDefault(); }
  get cancelBubble() { return this._stop === true; }
  set cancelBubble(v) { if (v) this.stopPropagation(); }

  stopPropagation() { this._stop = true; }
  stopImmediatePropagation() { this._stop = true; this._stopImmediate = true; }
  preventDefault() { if (this[kCancelable]) this[kCanceled] = true; }
  composedPath() { return this[kDispatchPath].slice(); }
}
Object.defineProperty(Event.prototype, Symbol.toStringTag, { value: 'Event' });
defineEventPhases(Event);

class CustomEvent extends Event {
  constructor(type, options = undefined) {
    super(type, options);
    this[kDetail] = options && Object.prototype.hasOwnProperty.call(options, 'detail') ?
      options.detail :
      null;
  }
  get detail() { return this[kDetail]; }
}
Object.defineProperty(CustomEvent.prototype, Symbol.toStringTag, { value: 'CustomEvent' });
defineEventPhases(CustomEvent);

function isEventTarget(value) {
  return !!(value != null &&
    typeof value === 'object' &&
    typeof value.addEventListener === 'function' &&
    typeof value.removeEventListener === 'function' &&
    typeof value.dispatchEvent === 'function');
}

function getRoot(map, type) {
  let root = map.get(type);
  if (!root) {
    root = { next: undefined };
    map.set(type, root);
  }
  return root;
}

function normalizeHandler(listener) {
  if (typeof listener === 'function') return listener;
  if (listener && typeof listener.handleEvent === 'function') return listener;
  return null;
}

class EventTarget {
  constructor() {
    this[kEvents] = new Map();
  }

  addEventListener(type, listener, options = undefined) {
    const key = String(type);
    const normalized = normalizeHandler(listener);
    if (!normalized) return;
    const root = getRoot(this[kEvents], key);
    const once = !!(options && typeof options === 'object' && options.once);
    let node = root.next;
    let prev = root;
    while (node) {
      if (node.listener === normalized) return;
      prev = node;
      node = node.next;
    }
    prev.next = {
      listener: normalized,
      original: listener,
      once,
      weak: !!(options && typeof options === 'object' && options[kWeakHandler]),
      next: undefined,
    };
  }

  removeEventListener(type, listener) {
    const key = String(type);
    const root = this[kEvents].get(key);
    if (!root) return;
    const normalized = normalizeHandler(listener);
    if (!normalized) return;
    let prev = root;
    let node = root.next;
    while (node) {
      if (node.listener === normalized) {
        prev.next = node.next;
        return;
      }
      prev = node;
      node = node.next;
    }
  }

  dispatchEvent(event) {
    if (!(event instanceof Event)) {
      throw new TypeError('The "event" argument must be an instance of Event');
    }
    const root = this[kEvents].get(event.type);
    event[kTarget] = this;
    event[kCurrentTarget] = this;
    event[kEventPhase] = Event.AT_TARGET;
    event[kDispatchPath] = [this];
    if (root) {
      let node = root.next;
      while (node) {
        const current = node;
        node = node.next;
        if (current.once) {
          this.removeEventListener(event.type, current.original);
        }
        const fn = current.listener;
        if (typeof fn === 'function') {
          fn.call(this, event);
        } else if (fn && typeof fn.handleEvent === 'function') {
          fn.handleEvent(event);
        }
        if (event._stopImmediate) break;
      }
    }
    event[kEventPhase] = Event.NONE;
    event[kCurrentTarget] = null;
    event[kDispatchPath] = [];
    return !event.defaultPrevented;
  }
}

if (typeof globalThis.Event !== 'function') globalThis.Event = Event;
if (typeof globalThis.CustomEvent !== 'function') globalThis.CustomEvent = CustomEvent;
if (typeof globalThis.EventTarget !== 'function') globalThis.EventTarget = EventTarget;

module.exports = {
  Event,
  EventTarget,
  CustomEvent,
  isEventTarget,
  kEvents,
  kWeakHandler,
  kResistStopPropagation,
};
