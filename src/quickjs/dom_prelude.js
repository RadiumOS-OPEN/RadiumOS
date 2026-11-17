/* dom_prelude.js - the browser DOM for QuickJS, written over the __rb natives
 * (rjs_dom.c). Compiled into the kernel as dom_prelude.c by
 * tools/rbrowse/js2c.py (the makefile regenerates it when this file changes).
 *
 * Node handles are engine ints; wrap() keeps one JS object per live node
 * (keyed by handle + generation, so a recycled slot gets a fresh wrapper).
 * Events the engine produces (click, input, keydown, ...) arrive through
 * __rb_fire; anything else (custom events, DOMContentLoaded) is dispatched
 * here in JS with the same capture/target/bubble walk.
 */
(function (global) {
'use strict';
const rb = global.__rb;
delete global.__rb;

/* ---------------- wrappers ---------------- */

const cache = new Map();          // handle -> {gen, obj}
const listeners = new Map();      // handle -> Map(type -> [{fn, capture, once}])
const nativeHooked = new Set();   // "handle:type:capture" registered with the engine
const fragments = new WeakSet();  // DocumentFragment wrappers (detached containers)

function wrap(h) {
  if (!h) return null;
  const g = rb.gen(h);
  if (!g) return null;
  const c = cache.get(h);
  if (c && c.gen === g) return c.obj;
  let obj;
  if (h === 1) obj = document;
  else if (rb.isText(h)) obj = Object.create(Text.prototype);
  else obj = Object.create(elementProto(rb.tag(h)));
  obj.__h = h;
  cache.set(h, {gen: g, obj});
  return obj;
}

function handle(x) {
  if (x == null) return 0;
  if (typeof x.__h === 'number') return x.__h;
  throw new TypeError('not a Node');
}

function list(hs) {
  const out = [];
  for (const h of hs) { const w = wrap(h); if (w) out.push(w); }
  return out;
}

function children(h, elementsOnly) {
  const out = [];
  for (let c = rb.first(h); c; c = rb.next(c)) {
    if (elementsOnly && rb.isText(c)) continue;
    const w = wrap(c);
    if (w) out.push(w);
  }
  return out;
}

function camelToKebab(s) {
  return s.replace(/[A-Z]/g, m => '-' + m.toLowerCase());
}

/* ---------------- events ---------------- */

class Event {
  constructor(type, init) {
    init = init || {};
    this.type = String(type);
    this.bubbles = !!init.bubbles;
    this.cancelable = !!init.cancelable;
    this.composed = !!init.composed;
    this.defaultPrevented = false;
    this.target = null;
    this.currentTarget = null;
    this.eventPhase = 0;
    this.timeStamp = rb.now();
    this.isTrusted = false;
    this.__stop = false;
    this.__stopImm = false;
    this.__native = false;
  }
  preventDefault() {
    if (this.cancelable || this.__native) this.defaultPrevented = true;
    if (this.__native) rb.evPrevent();
  }
  stopPropagation() { this.__stop = true; if (this.__native) rb.evStop(); }
  stopImmediatePropagation() {
    this.__stop = this.__stopImm = true;
    if (this.__native) rb.evStopImm();
  }
  composedPath() {
    const p = [];
    for (let n = this.target; n; n = n.parentNode) p.push(n);
    p.push(global);
    return p;
  }
  get returnValue() { return !this.defaultPrevented; }
  set returnValue(v) { if (!v) this.preventDefault(); }
  get srcElement() { return this.target; }
  initEvent(type, bubbles, cancelable) {
    this.type = String(type); this.bubbles = !!bubbles; this.cancelable = !!cancelable;
  }
}
Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;

class CustomEvent extends Event {
  constructor(type, init) { super(type, init); this.detail = init && 'detail' in init ? init.detail : null; }
}
class UIEvent extends Event {
  constructor(type, init) { super(type, init); this.view = global; this.detail = 0; }
}
class MouseEvent extends UIEvent {
  constructor(type, init) {
    super(type, init);
    init = init || {};
    this.clientX = this.pageX = this.x = this.offsetX = this.screenX = init.clientX || 0;
    this.clientY = this.pageY = this.y = this.offsetY = this.screenY = init.clientY || 0;
    this.button = init.button || 0;
    this.buttons = init.buttons || 0;
    this.shiftKey = !!init.shiftKey; this.ctrlKey = !!init.ctrlKey;
    this.altKey = !!init.altKey; this.metaKey = !!init.metaKey;
    this.relatedTarget = init.relatedTarget || null;
  }
}
class KeyboardEvent extends UIEvent {
  constructor(type, init) {
    super(type, init);
    init = init || {};
    this.key = init.key || '';
    this.code = init.code || '';
    this.keyCode = this.which = this.charCode = init.keyCode || 0;
    this.shiftKey = !!init.shiftKey; this.ctrlKey = !!init.ctrlKey;
    this.altKey = !!init.altKey; this.metaKey = !!init.metaKey;
    this.repeat = false;
  }
}
class FocusEvent extends UIEvent {}
class InputEvent extends UIEvent {}
class SubmitEvent extends Event {}

const KEY_NAMES = {8: 'Backspace', 9: 'Tab', 10: 'Enter', 13: 'Enter', 27: 'Escape', 32: ' ', 127: 'Delete'};

function nativeEvent(type, target) {
  const [key, x, y, button, buttons, mods, prevented] = rb.evInfo();
  const init = {bubbles: !/^(focus|blur|load|scroll|mouseenter|mouseleave)$/.test(type), cancelable: true,
                clientX: x, clientY: y, button: button < 0 ? 0 : button, buttons,
                shiftKey: !!(mods & 1), ctrlKey: !!(mods & 2), altKey: !!(mods & 4)};
  let ev;
  if (/^key/.test(type)) {
    const k = KEY_NAMES[key] || (key ? String.fromCharCode(key) : 'Unidentified');
    ev = new KeyboardEvent(type, Object.assign(init, {key: k, keyCode: key === 10 ? 13 : key,
      code: k.length === 1 && /[a-z]/i.test(k) ? 'Key' + k.toUpperCase() : k}));
  } else if (/^(click|mouse|dblclick|contextmenu)/.test(type)) {
    ev = new MouseEvent(type, init);
  } else if (type === 'focus' || type === 'blur') {
    ev = new FocusEvent(type, init);
  } else if (type === 'input') {
    ev = new InputEvent(type, init);
  } else {
    ev = new Event(type, init);
  }
  ev.isTrusted = true;
  ev.__native = true;
  ev.target = target;
  ev.defaultPrevented = !!prevented;
  return ev;
}

function invoke(node, ev, capture) {
  const m = node === global ? windowListeners : listeners.get(node.__h);
  const arr = m && m.get(ev.type);
  ev.currentTarget = node;
  if (arr) {
  for (const l of arr.slice()) {
    if (l.capture !== capture) continue;
    if (l.once) removeListener(node, ev.type, l.fn, l.capture);
    try {
      if (typeof l.fn === 'function') l.fn.call(node, ev);
      else if (l.fn && typeof l.fn.handleEvent === 'function') l.fn.handleEvent(ev);
    } catch (e) {
      reportError(e);
    }
    if (ev.__stopImm) break;
  }
  }
  const on = node === global ? global['on' + ev.type] : handlerProps.get(node) && handlerProps.get(node)[ev.type];
  if (!capture && typeof on === 'function' && !ev.__stopImm) {
    try {
      if (on.call(node, ev) === false) ev.preventDefault();
    } catch (e) { reportError(e); }
  }
}

/* One engine listener call = one node. Each gets a fresh Event: the engine
   itself carries stopPropagation/preventDefault across nodes (the natives
   forward them), and defaultPrevented is read back from it. */
global.__rb_fire = function (type, target, current, capture) {
  const t = wrap(target), cur = wrap(current);
  if (!t || !cur) return;
  const ev = nativeEvent(type, t);
  ev.eventPhase = target === current ? 2 : capture ? 1 : 3;
  invoke(cur, ev, !!capture);
  /* window sees engine events after the document: bubbling ones, and
     load (dispatched on the document itself) */
  if (current === 1 && !capture && (ev.bubbles || target === 1) && !ev.__stop) {
    ev.eventPhase = target === 1 ? 2 : 3;
    invoke(global, ev, false);
  }
};

function jsDispatch(target, ev) {
  ev.target = target;
  const path = [];
  for (let n = target.parentNode; n; n = n.parentNode) path.push(n);
  /* as in browsers, a load event at an element (image, script) does not
     reach window */
  if ((target === document || path.includes(document)) && !(ev.type === 'load' && target !== document))
    path.push(global);
  ev.eventPhase = 1;
  for (let i = path.length - 1; i >= 0 && !ev.__stop; i--) invoke(path[i], ev, true);
  if (!ev.__stop) {
    ev.eventPhase = 2;
    invoke(target, ev, true);
    if (!ev.__stopImm) invoke(target, ev, false);
  }
  if (ev.bubbles) {
    ev.eventPhase = 3;
    for (let i = 0; i < path.length && !ev.__stop; i++) invoke(path[i], ev, false);
  }
  ev.eventPhase = 0;
  ev.currentTarget = null;
  return !ev.defaultPrevented;
}

const windowListeners = new Map();

function listenerMap(node, create) {
  if (node === global) return windowListeners;
  const h = node.__h;
  let m = listeners.get(h);
  if (!m && create) { m = new Map(); listeners.set(h, m); }
  return m;
}

function hookNative(node, type, capture) {
  const h = node === global ? 1 : node.__h;
  const key = h + ':' + type + ':' + capture;
  if (!nativeHooked.has(key) && rb.listen(h, type, capture ? 1 : 0)) nativeHooked.add(key);
}

/* el.onclick = fn style handlers. */
const handlerProps = new WeakMap();
const EVENT_PROPS = ['click', 'dblclick', 'mousedown', 'mouseup', 'mouseover', 'mouseout', 'mousemove',
  'mouseenter', 'mouseleave', 'input', 'change', 'submit', 'reset', 'focus', 'blur', 'keydown', 'keyup',
  'keypress', 'scroll', 'load', 'error', 'abort', 'contextmenu', 'wheel', 'touchstart', 'touchend',
  'pointerdown', 'pointerup', 'transitionend', 'animationend', 'toggle', 'invalid', 'select', 'resize'];

function addListener(node, type, fn, opts) {
  if (!fn) return;
  const capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
  const once = !!(opts && typeof opts === 'object' && opts.once);
  type = String(type);
  const m = listenerMap(node, true);
  let arr = m.get(type);
  if (!arr) { arr = []; m.set(type, arr); }
  if (arr.some(l => l.fn === fn && l.capture === capture)) return;
  arr.push({fn, capture, once});
  /* engine-produced types need an engine listener on this node */
  hookNative(node, type, capture);
  if (opts && opts.signal && typeof opts.signal.addEventListener === 'function')
    opts.signal.addEventListener('abort', () => removeListener(node, type, fn, capture));
}

function removeListener(node, type, fn, opts) {
  const capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
  const m = listenerMap(node, false);
  const arr = m && m.get(String(type));
  if (!arr) return;
  const i = arr.findIndex(l => l.fn === fn && l.capture === capture);
  if (i >= 0) arr.splice(i, 1);
}

class EventTarget {
  addEventListener(type, fn, opts) { addListener(this, type, fn, opts); }
  removeEventListener(type, fn, opts) { removeListener(this, type, fn, opts); }
  dispatchEvent(ev) {
    if (!(ev instanceof Event)) throw new TypeError('dispatchEvent needs an Event');
    ev.__stop = ev.__stopImm = false;
    return jsDispatch(this, ev);
  }
}

function reportError(e) {
  const msg = (e && e.stack) ? String(e) + '\n' + e.stack : String(e);
  console.error('Uncaught ' + msg);
  if (typeof global.onerror === 'function') {
    try { global.onerror(String(e), '', 0, 0, e); } catch (_) {}
  }
}

/* ---------------- Node ---------------- */

class Node extends EventTarget {
  get nodeType() { return 1; }
  get nodeName() { return this.tagName; }
  get ownerDocument() { return document; }
  get parentNode() { return wrap(rb.parent(this.__h)); }
  get parentElement() {
    const p = rb.parent(this.__h);
    return p && p !== 1 ? wrap(p) : null;
  }
  get childNodes() { return children(this.__h, false); }
  get firstChild() { return wrap(rb.first(this.__h)); }
  get lastChild() { return wrap(rb.last(this.__h)); }
  get nextSibling() { return wrap(rb.next(this.__h)); }
  get previousSibling() { return wrap(rb.prev(this.__h)); }
  get isConnected() {
    let h = this.__h;
    while (h && h !== 1) h = rb.parent(h);
    return h === 1;
  }
  get textContent() {
    /* a parsed <script> keeps its body in the engine's script table; one
       built by JS has ordinary text children */
    return (rb.tag(this.__h) === 'script' && rb.scriptText(this.__h)) || rb.text(this.__h);
  }
  set textContent(v) {
    detachChildren(this.__h);
    rb.setText(this.__h, v == null ? '' : String(v));
  }
  get nodeValue() { return null; }
  set nodeValue(v) {}
  hasChildNodes() { return !!rb.first(this.__h); }
  contains(other) {
    if (!other) return false;
    for (let h = other.__h; h; h = rb.parent(h)) if (h === this.__h) return true;
    return false;
  }
  getRootNode() { return this.isConnected ? document : this; }
  isSameNode(o) { return o === this; }
  isEqualNode(o) { return !!o && o.nodeType === this.nodeType && o.outerHTML === this.outerHTML; }
  compareDocumentPosition(o) {
    if (o === this) return 0;
    if (this.contains(o)) return 20;
    if (o.contains(this)) return 10;
    const all = document.querySelectorAll('*');
    return all.indexOf(o) > all.indexOf(this) ? 4 : 2;
  }
  normalize() {}

  appendChild(child) { return this.insertBefore(child, null); }
  insertBefore(child, ref) {
    if (fragments.has(child)) {
      for (const c of child.childNodes) this.insertBefore(c, ref);
      return child;
    }
    const ok = ref ? rb.insert(this.__h, handle(child), handle(ref)) : rb.append(this.__h, handle(child));
    if (!ok) throw new DOMException('The node cannot be inserted here', 'HierarchyRequestError');
    if (child.nodeType === 1 && child.isConnected) startInsertedScripts(child);
    return child;
  }
  removeChild(child) {
    if (rb.parent(handle(child)) !== this.__h) throw new DOMException('Not a child of this node', 'NotFoundError');
    rb.detach(child.__h);
    return child;
  }
  replaceChild(newChild, oldChild) {
    if (fragments.has(newChild)) {
      this.insertBefore(newChild, oldChild);
      return this.removeChild(oldChild);
    }
    /* insert + detach: the engine's replace recycles the old node, but JS
       may keep using it */
    if (rb.parent(handle(oldChild)) !== this.__h) throw new DOMException('Not a child of this node', 'NotFoundError');
    if (newChild === oldChild) return oldChild;
    this.insertBefore(newChild, oldChild);
    rb.detach(oldChild.__h);
    return oldChild;
  }
  cloneNode(deep) {
    if (this instanceof Text) return document.createTextNode(this.data);
    if (fragments.has(this)) {
      const f = document.createDocumentFragment();
      if (deep) for (const c of this.childNodes) f.appendChild(c.cloneNode(true));
      return f;
    }
    const el = document.createElement(this.localName);
    for (const [n, v] of rb.attrs(this.__h)) rb.setAttr(el.__h, n, v);
    if (deep) rb.setHtml(el.__h, rb.html(this.__h, 0));
    return el;
  }

  /* ChildNode / ParentNode mixins */
  remove() { if (rb.parent(this.__h)) rb.detach(this.__h); }
  before(...nodes) { const p = this.parentNode; if (p) for (const n of nodes) p.insertBefore(toNode(n), this); }
  after(...nodes) {
    const p = this.parentNode;
    if (!p) return;
    const ref = this.nextSibling;
    for (const n of nodes) p.insertBefore(toNode(n), ref);
  }
  replaceWith(...nodes) { this.before(...nodes); this.remove(); }
  append(...nodes) { for (const n of nodes) this.appendChild(toNode(n)); }
  prepend(...nodes) {
    const ref = this.firstChild;
    for (const n of nodes) this.insertBefore(toNode(n), ref);
  }
  replaceChildren(...nodes) {
    detachChildren(this.__h);
    this.append(...nodes);
  }
  get children() { return children(this.__h, true); }
  get childElementCount() { return this.children.length; }
  get firstElementChild() { return this.children[0] || null; }
  get lastElementChild() { const c = this.children; return c[c.length - 1] || null; }
  querySelector(sel) { return wrap(rb.query(this.__h, String(sel), 1)[0] || 0); }
  querySelectorAll(sel) { return nodeList(list(rb.query(this.__h, String(sel)))); }
  getElementsByTagName(name) {
    name = String(name);
    return nodeList(list(rb.query(this.__h, name === '*' ? '*' : name.replace(/[^\w-]/g, ''))));
  }
  getElementsByClassName(names) {
    const sel = String(names).trim().split(/\s+/).filter(Boolean).map(c => '.' + cssEscape(c)).join('');
    return sel ? nodeList(list(rb.query(this.__h, sel))) : nodeList([]);
  }
}
for (const t of EVENT_PROPS) {
  Object.defineProperty(Node.prototype, 'on' + t, {
    configurable: true,
    get() { const m = handlerProps.get(this); return (m && m[t]) || null; },
    set(fn) {
      let m = handlerProps.get(this);
      if (!m) { m = {}; handlerProps.set(this, m); }
      m[t] = typeof fn === 'function' ? fn : null;
      if (m[t]) hookNative(this, t, false);
    },
  });
}
Node.ELEMENT_NODE = 1; Node.TEXT_NODE = 3; Node.COMMENT_NODE = 8;
Node.DOCUMENT_NODE = 9; Node.DOCUMENT_FRAGMENT_NODE = 11;
Node.DOCUMENT_POSITION_PRECEDING = 2; Node.DOCUMENT_POSITION_FOLLOWING = 4;
Node.DOCUMENT_POSITION_CONTAINS = 8; Node.DOCUMENT_POSITION_CONTAINED_BY = 16;

function nodeList(arr) {
  arr.item = i => arr[i] || null;
  return arr;
}

/* Replacing an element's content in the engine recycles the old children,
   but scripts may still hold them (jQuery moves them right back): detach
   first so they stay valid. Text nodes have no children to save. */
function detachChildren(h) {
  if (rb.isText(h)) return;
  for (let c = rb.first(h); c; c = rb.first(h)) if (!rb.detach(c)) break;
}

function toNode(n) {
  return (n instanceof Node) ? n : document.createTextNode(String(n));
}

function cssEscape(s) {
  return String(s).replace(/([^\w-])/g, '\\$1').replace(/^(\d)/, '\\3$1 ');
}

class CharacterData extends Node {
  get data() { return rb.text(this.__h); }
  set data(v) { rb.setText(this.__h, String(v)); }
  get length() { return this.data.length; }
  get nodeValue() { return this.data; }
  set nodeValue(v) { this.data = v; }
  appendData(s) { this.data += s; }
}

class Text extends CharacterData {
  get nodeType() { return 3; }
  get nodeName() { return '#text'; }
  get wholeText() { return this.data; }
}

class Comment extends CharacterData {
  get nodeType() { return 8; }
  get nodeName() { return '#comment'; }
}

/* ---------------- Element ---------------- */

const styleObjects = new WeakMap();

function parseStyle(text) {
  const out = new Map();
  for (const part of String(text || '').split(';')) {
    const i = part.indexOf(':');
    if (i < 0) continue;
    const k = part.slice(0, i).trim().toLowerCase();
    if (k) out.set(k, part.slice(i + 1).trim());
  }
  return out;
}

function writeStyle(el, map) {
  const s = [...map].filter(([, v]) => v !== '').map(([k, v]) => k + ': ' + v).join('; ');
  if (s) rb.setAttr(el.__h, 'style', s);
  else rb.removeAttr(el.__h, 'style');
}

function styleFor(el) {
  let st = styleObjects.get(el);
  if (st) return st;
  const api = {
    getPropertyValue(k) { return parseStyle(rb.getAttr(el.__h, 'style')).get(String(k).toLowerCase()) || ''; },
    setProperty(k, v, prio) {
      const m = parseStyle(rb.getAttr(el.__h, 'style'));
      v = v == null ? '' : String(v);
      if (prio === 'important' && v) v += ' !important';
      m.set(String(k).toLowerCase(), v);
      writeStyle(el, m);
    },
    removeProperty(k) {
      const m = parseStyle(rb.getAttr(el.__h, 'style'));
      const old = m.get(String(k).toLowerCase()) || '';
      m.delete(String(k).toLowerCase());
      writeStyle(el, m);
      return old;
    },
    get cssText() { return rb.getAttr(el.__h, 'style') || ''; },
    set cssText(v) { if (v) rb.setAttr(el.__h, 'style', String(v)); else rb.removeAttr(el.__h, 'style'); },
    get length() { return parseStyle(rb.getAttr(el.__h, 'style')).size; },
    item(i) { return [...parseStyle(rb.getAttr(el.__h, 'style')).keys()][i] || ''; },
  };
  st = new Proxy(api, {
    get(t, p) {
      if (typeof p !== 'string' || p in t) return t[p];
      if (p === 'cssFloat') p = 'float';
      return t.getPropertyValue(camelToKebab(p));
    },
    set(t, p, v) {
      if (p === 'cssText') { t.cssText = v; return true; }
      if (typeof p !== 'string') return true;
      if (p === 'cssFloat') p = 'float';
      t.setProperty(p.startsWith('--') ? p : camelToKebab(p), v);
      return true;
    },
  });
  styleObjects.set(el, st);
  return st;
}

class DOMTokenList {
  constructor(el, attr) { this.__el = el; this.__attr = attr; }
  __get() { return (rb.getAttr(this.__el.__h, this.__attr) || '').split(/\s+/).filter(Boolean); }
  __set(arr) { rb.setAttr(this.__el.__h, this.__attr, arr.join(' ')); }
  get length() { return this.__get().length; }
  get value() { return rb.getAttr(this.__el.__h, this.__attr) || ''; }
  set value(v) { rb.setAttr(this.__el.__h, this.__attr, String(v)); }
  item(i) { return this.__get()[i] || null; }
  contains(t) { return this.__get().includes(String(t)); }
  add(...ts) {
    const a = this.__get();
    for (const t of ts) if (!a.includes(String(t))) a.push(String(t));
    this.__set(a);
  }
  remove(...ts) { this.__set(this.__get().filter(x => !ts.map(String).includes(x))); }
  toggle(t, force) {
    t = String(t);
    const has = this.contains(t);
    const want = force === undefined ? !has : !!force;
    if (want && !has) this.add(t);
    if (!want && has) this.remove(t);
    return want;
  }
  replace(a, b) {
    const arr = this.__get();
    const i = arr.indexOf(String(a));
    if (i < 0) return false;
    arr[i] = String(b);
    this.__set(arr);
    return true;
  }
  supports() { return true; }
  forEach(fn, thisArg) { this.__get().forEach(fn, thisArg); }
  entries() { return this.__get().entries(); }
  keys() { return this.__get().keys(); }
  values() { return this.__get().values(); }
  [Symbol.iterator]() { return this.__get()[Symbol.iterator](); }
  toString() { return this.value; }
}

function datasetFor(el) {
  return new Proxy({}, {
    get(t, p) {
      if (typeof p !== 'string') return undefined;
      const v = rb.getAttr(el.__h, 'data-' + camelToKebab(p));
      return v === null ? undefined : v;
    },
    set(t, p, v) { rb.setAttr(el.__h, 'data-' + camelToKebab(String(p)), String(v)); return true; },
    deleteProperty(t, p) { rb.removeAttr(el.__h, 'data-' + camelToKebab(String(p))); return true; },
    has(t, p) { return rb.hasAttr(el.__h, 'data-' + camelToKebab(String(p))); },
    ownKeys() {
      return rb.attrs(el.__h).filter(([n]) => n.startsWith('data-'))
        .map(([n]) => n.slice(5).replace(/-([a-z])/g, (m, c) => c.toUpperCase()));
    },
    getOwnPropertyDescriptor(t, p) {
      const v = rb.getAttr(el.__h, 'data-' + camelToKebab(String(p)));
      return v === null ? undefined : {value: v, enumerable: true, configurable: true, writable: true};
    },
  });
}

const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'source', 'track', 'wbr']);

function reflect(proto, prop, attr, kind) {
  Object.defineProperty(proto, prop, {
    configurable: true,
    get() {
      const v = rb.getAttr(this.__h, attr);
      if (kind === 'bool') return v !== null;
      if (kind === 'url') return v === null ? '' : (rb.resolve(v) || v);
      if (kind === 'int') return v === null ? 0 : parseInt(v, 10) || 0;
      return v === null ? '' : v;
    },
    set(v) {
      if (kind === 'bool') { if (v) rb.setAttr(this.__h, attr, ''); else rb.removeAttr(this.__h, attr); }
      else rb.setAttr(this.__h, attr, String(v));
    },
  });
}

class Element extends Node {
  get nodeType() { return 1; }
  get localName() { return rb.tag(this.__h); }
  get tagName() { return this.localName.toUpperCase(); }
  get namespaceURI() { return 'http://www.w3.org/1999/xhtml'; }

  getAttribute(n) { return rb.getAttr(this.__h, String(n).toLowerCase()); }
  setAttribute(n, v) { rb.setAttr(this.__h, String(n).toLowerCase(), String(v)); }
  removeAttribute(n) { rb.removeAttr(this.__h, String(n).toLowerCase()); }
  hasAttribute(n) { return rb.hasAttr(this.__h, String(n).toLowerCase()); }
  toggleAttribute(n, force) {
    const has = this.hasAttribute(n);
    const want = force === undefined ? !has : !!force;
    if (want && !has) this.setAttribute(n, '');
    if (!want && has) this.removeAttribute(n);
    return want;
  }
  getAttributeNames() { return rb.attrs(this.__h).map(a => a[0]); }
  hasAttributes() { return rb.attrs(this.__h).length > 0; }
  getAttributeNS(ns, n) { return this.getAttribute(n); }
  setAttributeNS(ns, n, v) { this.setAttribute(n, v); }
  removeAttributeNS(ns, n) { this.removeAttribute(n); }
  get attributes() {
    const arr = rb.attrs(this.__h).map(([name, value]) => ({name, value, localName: name, nodeName: name, specified: true}));
    arr.getNamedItem = n => arr.find(a => a.name === String(n).toLowerCase()) || null;
    arr.item = i => arr[i] || null;
    return arr;
  }

  get id() { return this.getAttribute('id') || ''; }
  set id(v) { this.setAttribute('id', v); }
  get className() { return this.getAttribute('class') || ''; }
  set className(v) { this.setAttribute('class', v); }
  get classList() { return new DOMTokenList(this, 'class'); }
  get dataset() { return datasetFor(this); }
  get style() { return styleFor(this); }
  set style(v) { styleFor(this).cssText = v; }

  get innerHTML() { return rb.html(this.__h, 0); }
  set innerHTML(v) {
    detachChildren(this.__h);
    rb.setHtml(this.__h, v == null ? '' : String(v));
  }
  get outerHTML() { return rb.html(this.__h, 1); }
  set outerHTML(v) {
    if (!rb.parent(this.__h)) return;
    rb.adjHtml(this.__h, 0, String(v));
    rb.detach(this.__h);
  }
  get innerText() { return this.textContent; }
  set innerText(v) { this.textContent = v; }
  get outerText() { return this.textContent; }
  insertAdjacentHTML(where, html) {
    const i = ['beforebegin', 'afterbegin', 'beforeend', 'afterend'].indexOf(String(where).toLowerCase());
    if (i < 0) throw new DOMException('Bad position', 'SyntaxError');
    rb.adjHtml(this.__h, i, String(html));
  }
  insertAdjacentElement(where, el) {
    switch (String(where).toLowerCase()) {
    case 'beforebegin': this.before(el); break;
    case 'afterbegin': this.prepend(el); break;
    case 'beforeend': this.append(el); break;
    case 'afterend': this.after(el); break;
    }
    return el;
  }
  insertAdjacentText(where, text) { this.insertAdjacentElement(where, document.createTextNode(text)); }

  matches(sel) { return rb.matches(this.__h, String(sel)); }
  get webkitMatchesSelector() { return this.matches; }
  closest(sel) { return wrap(rb.closest(this.__h, String(sel))); }
  get nextElementSibling() {
    for (let h = rb.next(this.__h); h; h = rb.next(h)) if (!rb.isText(h)) return wrap(h);
    return null;
  }
  get previousElementSibling() {
    for (let h = rb.prev(this.__h); h; h = rb.prev(h)) if (!rb.isText(h)) return wrap(h);
    return null;
  }

  /* Geometry. Horizontal engine px are CSS px; vertical ones are half
     scale (the 8px font stands in for 16px text), so y is doubled. */
  getBoundingClientRect() {
    const r = rb.rect(this.__h) || [0, 0, 0, 0];
    const x = r[0], y = r[1] * 2, w = r[2], h = r[3] * 2;
    return {x, y, left: x, top: y, width: w, height: h, right: x + w, bottom: y + h, toJSON() { return this; }};
  }
  getClientRects() { const r = this.getBoundingClientRect(); return r.width || r.height ? [r] : []; }
  get offsetWidth() { return this.getBoundingClientRect().width; }
  get offsetHeight() { return this.getBoundingClientRect().height; }
  get offsetLeft() { return this.getBoundingClientRect().left; }
  get offsetTop() { return this.getBoundingClientRect().top; }
  get offsetParent() { return this.parentElement; }
  get clientWidth() { return this.offsetWidth; }
  get clientHeight() { return this.offsetHeight; }
  get clientTop() { return 0; }
  get clientLeft() { return 0; }
  get scrollWidth() { return this.offsetWidth; }
  get scrollHeight() { return this.offsetHeight; }
  get scrollTop() { return 0; }
  set scrollTop(v) {}
  get scrollLeft() { return 0; }
  set scrollLeft(v) {}
  scrollIntoView() {}
  scroll() {}
  scrollTo() {}
  scrollBy() {}

  click() {
    if (rb.dispatch(this.__h, 'click') < 0) this.dispatchEvent(new MouseEvent('click', {bubbles: true, cancelable: true}));
  }
  focus() { rb.focus(this.__h); }
  blur() { if (document.activeElement === this) rb.blur(); }

  attachShadow() { return this; }
  get shadowRoot() { return null; }
  animate() { return {finished: Promise.resolve(), cancel() {}, play() {}, pause() {}, onfinish: null}; }
  getAnimations() { return []; }
}

class HTMLElement extends Element {}
const P = HTMLElement.prototype;
for (const [prop, attr, kind] of [
  ['title', 'title'], ['lang', 'lang'], ['dir', 'dir'], ['hidden', 'hidden', 'bool'],
  ['tabIndex', 'tabindex', 'int'], ['accessKey', 'accesskey'], ['draggable', 'draggable', 'bool'],
  ['href', 'href', 'url'], ['src', 'src', 'url'], ['alt', 'alt'], ['rel', 'rel'], ['target', 'target'],
  ['name', 'name'], ['type', 'type'], ['placeholder', 'placeholder'], ['action', 'action', 'url'],
  ['method', 'method'], ['disabled', 'disabled', 'bool'], ['readOnly', 'readonly', 'bool'],
  ['required', 'required', 'bool'], ['multiple', 'multiple', 'bool'], ['htmlFor', 'for'],
  ['width', 'width', 'int'], ['height', 'height', 'int'], ['colSpan', 'colspan', 'int'],
  ['rowSpan', 'rowspan', 'int'], ['content', 'content'], ['async', 'async', 'bool'],
  ['defer', 'defer', 'bool'], ['charset', 'charset'], ['download', 'download'],
  ['autofocus', 'autofocus', 'bool'], ['autocomplete', 'autocomplete'], ['min', 'min'], ['max', 'max'],
  ['step', 'step'], ['pattern', 'pattern'], ['maxLength', 'maxlength', 'int'], ['label', 'label'],
  ['selected', 'selected', 'bool'], ['open', 'open', 'bool'], ['media', 'media'], ['srcset', 'srcset'],
  ['loading', 'loading'], ['role', 'role'], ['contentEditable', 'contenteditable'],
]) reflect(P, prop, attr, kind);

Object.defineProperty(P, 'value', {
  configurable: true,
  get() { return rb.value(this.__h) || ''; },
  set(v) { rb.setValue(this.__h, v == null ? '' : String(v)); },
});
Object.defineProperty(P, 'defaultValue', {
  configurable: true,
  get() { return this.getAttribute('value') || ''; },
  set(v) { this.setAttribute('value', v); },
});
Object.defineProperty(P, 'checked', {
  configurable: true,
  get() { return rb.checked(this.__h); },
  set(v) { rb.setChecked(this.__h, v ? 1 : 0); },
});
Object.defineProperty(P, 'form', { configurable: true, get() { return this.closest('form'); } });
Object.defineProperty(P, 'text', {
  configurable: true,
  get() { return this.textContent; },
  set(v) { this.textContent = v; },
});
Object.defineProperty(P, 'options', {
  configurable: true,
  get() { return this.localName === 'select' ? this.querySelectorAll('option') : undefined; },
});
Object.defineProperty(P, 'selectedIndex', {
  configurable: true,
  get() { const o = this.querySelectorAll('option'); const v = this.value; return o.findIndex(x => x.value === v || x.textContent === v); },
  set(i) { const o = this.querySelectorAll('option')[i]; if (o) this.value = o.value || o.textContent; },
});
Object.defineProperty(P, 'elements', {
  configurable: true,
  get() { return this.localName === 'form' ? this.querySelectorAll('input,select,textarea,button') : undefined; },
});
P.submit = function () { if (this.localName === 'form') rb.dispatch(this.__h, 'submit'); };
P.requestSubmit = P.submit;
P.reset = function () {};
P.select = function () {};
P.setSelectionRange = function () {};
P.checkValidity = function () { return true; };
P.reportValidity = function () { return true; };
P.setCustomValidity = function () {};
P.play = function () { return Promise.resolve(); };
P.pause = function () {};
P.getContext = function () { return null; };

/* Every HTML*Element name resolves to HTMLElement, so instanceof checks and
   prototype patches written for specific element types keep working. */
const ELEMENT_CLASSES = ['Anchor', 'Area', 'Audio', 'BR', 'Base', 'Body', 'Button', 'Canvas', 'DList',
  'Data', 'DataList', 'Details', 'Dialog', 'Div', 'Embed', 'FieldSet', 'Form', 'Head', 'Heading', 'HR',
  'Html', 'IFrame', 'Image', 'Input', 'LI', 'Label', 'Legend', 'Link', 'Map', 'Media', 'Menu', 'Meta',
  'Meter', 'Mod', 'OList', 'Object', 'OptGroup', 'Option', 'Output', 'Paragraph', 'Picture', 'Pre',
  'Progress', 'Quote', 'Script', 'Select', 'Slot', 'Source', 'Span', 'Style', 'Table', 'TableCaption',
  'TableCell', 'TableCol', 'TableRow', 'TableSection', 'Template', 'TextArea', 'Time', 'Title', 'Track',
  'UList', 'Unknown', 'Video'];
for (const n of ELEMENT_CLASSES) global['HTML' + n + 'Element'] = HTMLElement;
global.SVGElement = HTMLElement;

/* <img>. The host loads images after the page is shown (rbrowse.c): the
   engine's per-image record says whether one decoded, and load/error are
   fired at the element when it finishes (__rb_img_event). A new Image()
   that is never inserted loads too (the host scans detached images), so
   the preload-then-draw pattern works. width/height read the laid-out size
   when the image has a box, else the attribute, else the natural size. */
const IMG_NONE = 0, IMG_OK = 1;
class HTMLImageElement extends HTMLElement {
  get complete() { const i = rb.img(this.__h); return i[0] !== IMG_NONE || !this.getAttribute('src'); }
  get naturalWidth() { return rb.img(this.__h)[1]; }
  get naturalHeight() { return rb.img(this.__h)[2]; }
  get currentSrc() { const s = this.getAttribute('src'); return s ? (rb.resolve(s) || s) : ''; }
  __dim(i) {
    const r = this.isConnected ? rb.rect(this.__h) : null;
    if (r && (r[2] || r[3])) return r[2 + i];        /* images are not halved vertically */
    const a = parseInt(this.getAttribute(i ? 'height' : 'width'), 10);
    if (a >= 0) return a;
    return rb.img(this.__h)[1 + i];
  }
  get width() { return this.__dim(0); }
  set width(v) { this.setAttribute('width', String(v | 0)); }
  get height() { return this.__dim(1); }
  set height(v) { this.setAttribute('height', String(v | 0)); }
  getBoundingClientRect() {
    const r = HTMLElement.prototype.getBoundingClientRect.call(this);
    const h = (rb.rect(this.__h) || [0, 0, 0, 0])[3];
    return Object.assign(r, {height: h, bottom: r.top + h});
  }
  decode() {
    if (this.complete) {
      return rb.img(this.__h)[0] === IMG_OK || !this.getAttribute('src') ? Promise.resolve()
        : Promise.reject(new DOMException('The source image cannot be decoded.', 'EncodingError'));
    }
    return new Promise((res, rej) => {
      const done = ok => () => {
        this.removeEventListener('load', onload);
        this.removeEventListener('error', onerror);
        if (ok) res(); else rej(new DOMException('The source image cannot be decoded.', 'EncodingError'));
      };
      const onload = done(true), onerror = done(false);
      this.addEventListener('load', onload);
      this.addEventListener('error', onerror);
    });
  }
}

global.HTMLImageElement = HTMLImageElement;
/* new Image(w, h): an <img>; instanceof Image works through the shared prototype */
function Image(w, h) {
  const el = document.createElement('img');
  if (w !== undefined) el.width = w;
  if (h !== undefined) el.height = h;
  return el;
}
Image.prototype = HTMLImageElement.prototype;

/* Custom elements: a defined class becomes the prototype of new wrappers
   for that tag (constructor/connectedCallback are not run). */
const customProtos = new Map();
function elementProto(tag) {
  return customProtos.get(tag) || (tag === 'img' ? HTMLImageElement.prototype : HTMLElement.prototype);
}
global.customElements = {
  define(name, cls) { customProtos.set(String(name).toLowerCase(), cls.prototype); },
  get(name) { const p = customProtos.get(String(name).toLowerCase()); return p ? p.constructor : undefined; },
  whenDefined() { return Promise.resolve(); },
  upgrade() {},
};

class DOMException extends Error {
  constructor(message, name) { super(message || ''); this.name = name || 'Error'; }
}

/* ---------------- scripts inserted by scripts ----------------
   A <script> put into the document by JS runs once: inline ones at once,
   src ones after their download (async, then load/error on the element).
   Scripts parsed from markup (the page's own, innerHTML) are not run here:
   the host ran the page's, and innerHTML scripts never run in browsers. */
const startedScripts = new Set();   // "handle:generation"

function scriptKey(h) { return h + ':' + rb.gen(h); }

function isClassicType(t) {
  t = (t || '').trim().toLowerCase();
  return !t || /javascript|ecmascript|jscript/.test(t);
}

function runScriptText(h, text) {
  const prev = currentScript;
  currentScript = h;
  try { (0, eval)(text); } catch (e) { reportError(e); }
  currentScript = prev;
}

/* Only a <script> inserted itself: one inside an inserted subtree came from
   markup (innerHTML) or was already handled when it was put there. */
function startInsertedScripts(s) {
  if (s.localName !== 'script') return;
  const key = scriptKey(s.__h);
  if (startedScripts.has(key) || !isClassicType(s.getAttribute('type'))) return;
  startedScripts.add(key);
  const src = s.getAttribute('src');
  if (src === null) {
    runScriptText(s.__h, s.textContent);
    return;
  }
  const abs = rb.resolve(src);
  setTimeout(() => {
    const text = abs ? rb.loadScript(abs) : null;
    if (text === null) {
      jsDispatch(s, new Event('error'));
      return;
    }
    runScriptText(s.__h, text);
    jsDispatch(s, new Event('load'));
  }, 0);
}

/* ---------------- Document ---------------- */

let currentScript = 0;
let readyState = 'loading';
let cookieJar = new Map();

class Document extends Node {
  get nodeType() { return 9; }
  get nodeName() { return '#document'; }
  get documentElement() { return wrap(rb.query(1, 'html', 1)[0] || rb.first(1)); }
  /* The engine does not synthesize <head>/<body> the way browsers do; fall
     back to the root element so document.head.appendChild(...) still works. */
  get head() { return wrap(rb.query(1, 'head', 1)[0] || 0) || this.documentElement; }
  get body() { return wrap(rb.query(1, 'body', 1)[0] || 0) || this.documentElement; }
  get title() { return rb.title(); }
  set title(v) {
    let t = rb.query(1, 'title', 1)[0];
    if (t) rb.setText(t, String(v));
  }
  get readyState() { return readyState; }
  get currentScript() { return wrap(currentScript); }
  get activeElement() { return wrap(rb.active()) || this.body; }
  get defaultView() { return global; }
  get location() { return location; }
  set location(v) { location.href = v; }
  get URL() { return rb.url(); }
  get documentURI() { return rb.url(); }
  get baseURI() { return rb.url(); }
  get referrer() { return ''; }
  get domain() { return location.hostname; }
  get characterSet() { return 'UTF-8'; }
  get charset() { return 'UTF-8'; }
  get contentType() { return 'text/html'; }
  get compatMode() { return 'CSS1Compat'; }
  get hidden() { return false; }
  get visibilityState() { return 'visible'; }
  get cookie() { return [...cookieJar].map(([k, v]) => k + '=' + v).join('; '); }
  set cookie(s) {
    const first = String(s).split(';')[0];
    const i = first.indexOf('=');
    if (i <= 0) return;
    const k = first.slice(0, i).trim(), v = first.slice(i + 1).trim();
    if (/;\s*(max-age=0|expires=Thu, 01 Jan 1970)/i.test(s)) cookieJar.delete(k);
    else cookieJar.set(k, v);
  }
  get forms() { return this.querySelectorAll('form'); }
  get images() { return this.querySelectorAll('img'); }
  get links() { return this.querySelectorAll('a[href],area[href]'); }
  get scripts() { return this.querySelectorAll('script'); }
  get styleSheets() { return []; }
  get fonts() { return {ready: Promise.resolve(), load: () => Promise.resolve([]), check: () => true, add() {}}; }
  get implementation() {
    return {hasFeature: () => true, createHTMLDocument: t => createInertDocument(t)};
  }

  getElementById(id) { return wrap(rb.byId(String(id))); }
  getElementsByName(n) { return this.querySelectorAll('[name="' + String(n).replace(/"/g, '\\"') + '"]'); }
  createElement(name) {
    const h = rb.create(String(name).toLowerCase());
    if (!h) throw new DOMException('Cannot create element (engine full)', 'QuotaExceededError');
    return wrap(h);
  }
  createElementNS(ns, name) { return this.createElement(String(name).replace(/^.*:/, '')); }
  createTextNode(text) {
    const h = rb.createText(String(text));
    if (!h) throw new DOMException('Cannot create text (engine full)', 'QuotaExceededError');
    return wrap(h);
  }
  createComment() {
    const t = this.createTextNode('');
    Object.setPrototypeOf(t, Comment.prototype);
    return t;
  }
  createDocumentFragment() {
    const f = this.createElement('div');
    fragments.add(f);
    Object.defineProperty(f, 'nodeType', {value: 11});
    return f;
  }
  createEvent(kind) {
    const k = String(kind);
    if (/Mouse/i.test(k)) return new MouseEvent('');
    if (/Key/i.test(k)) return new KeyboardEvent('');
    if (/Custom/i.test(k)) return new CustomEvent('');
    return new Event('');
  }
  createRange() {
    return {setStart() {}, setEnd() {}, selectNodeContents() {}, collapse() {},
            getBoundingClientRect: () => ({x: 0, y: 0, width: 0, height: 0, top: 0, left: 0, right: 0, bottom: 0}),
            createContextualFragment: html => { const f = this.createDocumentFragment(); f.innerHTML = html; return f; }};
  }
  createTreeWalker(root) {
    const all = [root, ...root.querySelectorAll('*')];
    let i = 0;
    return {currentNode: root, nextNode() { i++; this.currentNode = all[i] || null; return this.currentNode; }};
  }
  importNode(n, deep) { return n.cloneNode(deep); }
  adoptNode(n) { return n; }
  hasFocus() { return true; }
  getSelection() { return {toString: () => '', removeAllRanges() {}, addRange() {}, rangeCount: 0}; }
  execCommand() { return false; }
  elementFromPoint() { return this.body; }
  /* document.write while a script runs inserts after that script, which is
     where the parser would have put it; afterwards it appends to <body>. */
  write(...parts) {
    const html = parts.join('');
    if (currentScript) rb.adjHtml(currentScript, 3, html);
    else if (this.body) rb.adjHtml(this.body.__h, 2, html);
  }
  writeln(...parts) { this.write(...parts, '\n'); }
  open() { return this; }
  close() {}
}

/* A separate, detached document (createHTMLDocument, DOMParser): its own
   <html><head><body>, never rendered. Libraries parse markup into these
   precisely so it cannot touch the page. */
class InertDocument extends Document {
  get documentElement() { return wrap(this.__h); }
  get head() { return wrap(rb.query(this.__h, 'head', 1)[0] || 0); }
  get body() { return wrap(rb.query(this.__h, 'body', 1)[0] || 0); }
  get title() { const t = rb.query(this.__h, 'title', 1)[0]; return t ? rb.text(t) : ''; }
  set title(v) {}
  get readyState() { return 'complete'; }
  get location() { return null; }
  get URL() { return 'about:blank'; }
  getElementById(id) { return this.querySelector('#' + cssEscape(id)); }
  write(...parts) { const b = this.body; if (b) rb.adjHtml(b.__h, 2, parts.join('')); }
}

function createInertDocument(title) {
  const html = rb.create('html');
  rb.append(html, rb.create('head'));
  rb.append(html, rb.create('body'));
  const d = Object.create(InertDocument.prototype);
  d.__h = html;
  if (title !== undefined) {
    const t = rb.create('title');
    rb.setText(t, String(title));
    rb.append(rb.first(html), t);
  }
  return d;
}

class DOMParser {
  parseFromString(markup, type) {
    const d = createInertDocument();
    const m = String(markup);
    const body = /<body[^>]*>([\s\S]*?)(<\/body>|$)/i.exec(m);
    const head = /<head[^>]*>([\s\S]*?)<\/head>/i.exec(m);
    if (head) rb.setHtml(rb.first(d.__h), head[1]);
    rb.setHtml(rb.query(d.__h, 'body', 1)[0], body ? body[1] : m.replace(/<\/?(html|head)[^>]*>/gi, ''));
    return d;
  }
}

const document = Object.create(Document.prototype);
document.__h = 1;
cache.set(1, {gen: rb.gen(1), obj: document});

/* ---------------- location, history, navigator ---------------- */

function parseURL(href) {
  const m = /^([a-z][a-z0-9+.-]*:)(?:\/\/([^/?#]*))?([^?#]*)(\?[^#]*)?(#.*)?$/i.exec(href) || [];
  const protocol = (m[1] || '').toLowerCase();
  const host = m[2] || '';
  const hm = /^(?:[^@]*@)?(\[[^\]]*\]|[^:]*)(?::(\d+))?$/.exec(host) || [];
  return {
    href, protocol, host, hostname: hm[1] || '', port: hm[2] || '',
    pathname: m[3] || '/', search: m[4] && m[4] !== '?' ? m[4] : '', hash: m[5] && m[5] !== '#' ? m[5] : '',
    origin: protocol + '//' + host,
  };
}

class URLSearchParams {
  constructor(init) {
    this.__p = [];
    if (init == null) return;
    if (typeof init === 'string') {
      for (const part of init.replace(/^\?/, '').split('&')) {
        if (!part) continue;
        const i = part.indexOf('=');
        const dec = s => { try { return decodeURIComponent(s.replace(/\+/g, ' ')); } catch (e) { return s; } };
        this.__p.push(i < 0 ? [dec(part), ''] : [dec(part.slice(0, i)), dec(part.slice(i + 1))]);
      }
    } else if (typeof init[Symbol.iterator] === 'function') {
      for (const [k, v] of init) this.__p.push([String(k), String(v)]);
    } else {
      for (const k of Object.keys(init)) this.__p.push([k, String(init[k])]);
    }
  }
  get(k) { const e = this.__p.find(p => p[0] === k); return e ? e[1] : null; }
  getAll(k) { return this.__p.filter(p => p[0] === k).map(p => p[1]); }
  has(k) { return this.__p.some(p => p[0] === k); }
  set(k, v) { this.delete(k); this.__p.push([String(k), String(v)]); }
  append(k, v) { this.__p.push([String(k), String(v)]); }
  delete(k) { this.__p = this.__p.filter(p => p[0] !== k); }
  forEach(fn) { for (const [k, v] of this.__p) fn(v, k, this); }
  keys() { return this.__p.map(p => p[0])[Symbol.iterator](); }
  values() { return this.__p.map(p => p[1])[Symbol.iterator](); }
  entries() { return this.__p.map(p => [p[0], p[1]])[Symbol.iterator](); }
  [Symbol.iterator]() { return this.entries(); }
  get size() { return this.__p.length; }
  sort() { this.__p.sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0); }
  toString() {
    const enc = s => encodeURIComponent(s).replace(/%20/g, '+');
    return this.__p.map(([k, v]) => enc(k) + '=' + enc(v)).join('&');
  }
}

class URL {
  constructor(href, base) {
    href = String(href);
    let abs = href;
    if (!/^[a-z][a-z0-9+.-]*:/i.test(href)) {
      if (base === undefined) throw new TypeError('Invalid URL: ' + href);
      const b = String(base);
      if (/^\/\//.test(href)) abs = parseURL(b).protocol + href;
      else if (href[0] === '/') abs = parseURL(b).origin + href;
      else if (href[0] === '?' ) abs = b.replace(/[?#].*$/, '') + href;
      else if (href[0] === '#') abs = b.replace(/#.*$/, '') + href;
      else abs = b.replace(/[?#].*$/, '').replace(/[^/]*$/, '') + href;
    }
    abs = abs.replace(/\/\.\//g, '/');
    while (/\/[^/.][^/]*\/\.\.\//.test(abs)) abs = abs.replace(/\/[^/.][^/]*\/\.\.\//, '/');
    const parts = parseURL(abs);
    delete parts.href;    /* href is the accessor below */
    Object.assign(this, parts);
    if (/^https?:$/.test(this.protocol) && !this.pathname) this.pathname = '/';
    this.searchParams = new URLSearchParams(this.search);
    this.username = '';
    this.password = '';
  }
  get href() { return this.protocol + (this.host || this.protocol === 'file:' ? '//' : '') + this.host + this.pathname + this.search + this.hash; }
  set href(v) { Object.assign(this, new URL(v)); }
  toString() { return this.href; }
  toJSON() { return this.href; }
  static createObjectURL() { return 'blob:radiumos'; }
  static revokeObjectURL() {}
  static canParse(u, b) { try { new URL(u, b); return true; } catch (e) { return false; } }
}

function navigate(href) {
  const target = rb.resolve(String(href));
  if (target) rb.nav(target);
}

const location = {
  get href() { return rb.url(); },
  set href(v) { navigate(v); },
  get protocol() { return parseURL(rb.url()).protocol; },
  get host() { return parseURL(rb.url()).host; },
  get hostname() { return parseURL(rb.url()).hostname; },
  get port() { return parseURL(rb.url()).port; },
  get pathname() { return parseURL(rb.url()).pathname; },
  get search() { return parseURL(rb.url()).search; },
  set search(v) { const u = new URL(rb.url()); u.search = v ? '?' + String(v).replace(/^\?/, '') : ''; navigate(u.href); },
  get hash() { return parseURL(rb.url()).hash; },
  set hash(v) {},
  get origin() { return parseURL(rb.url()).origin; },
  assign(u) { navigate(u); },
  replace(u) { navigate(u); },
  reload() { rb.nav(rb.url()); },
  toString() { return rb.url(); },
};

const history = {
  length: 1, state: null, scrollRestoration: 'auto',
  pushState(state) { this.state = state; },
  replaceState(state) { this.state = state; },
  back() {}, forward() {}, go() {},
};

const navigator = {
  userAgent: 'Mozilla/5.0 (RadiumOS; i686) rbrowse/0.3 QuickJS',
  appName: 'Netscape', appVersion: '5.0 (RadiumOS)', appCodeName: 'Mozilla', product: 'Gecko',
  platform: 'RadiumOS i686', vendor: 'RadiumOS', language: 'en-US', languages: ['en-US', 'en'],
  onLine: true, cookieEnabled: true, doNotTrack: '1', hardwareConcurrency: 1, maxTouchPoints: 0,
  webdriver: false, pdfViewerEnabled: false,
  sendBeacon() { return true; },
  javaEnabled() { return false; },
  clipboard: {writeText: () => Promise.resolve(), readText: () => Promise.resolve('')},
  serviceWorker: undefined,
};

/* ---------------- storage ---------------- */

class Storage {
  constructor() { Object.defineProperty(this, '__m', {value: new Map()}); }
  get length() { return this.__m.size; }
  key(i) { return [...this.__m.keys()][i] ?? null; }
  getItem(k) { k = String(k); return this.__m.has(k) ? this.__m.get(k) : null; }
  setItem(k, v) { this.__m.set(String(k), String(v)); }
  removeItem(k) { this.__m.delete(String(k)); }
  clear() { this.__m.clear(); }
}

/* ---------------- timers ---------------- */

const timers = new Map();   // id -> {due, fn, args, every}
let nextTimer = 1;

function scheduleNext() {
  let due = -1;
  for (const t of timers.values()) if (due < 0 || t.due < due) due = t.due;
  rb.setNextDue(due);
}

function addTimer(fn, ms, args, every) {
  if (typeof fn === 'string') { const code = fn; fn = () => (0, eval)(code); }
  if (typeof fn !== 'function') return 0;
  ms = Math.max(every ? 4 : 0, Number(ms) || 0);
  const id = nextTimer++;
  timers.set(id, {due: rb.now() + ms, fn, args, every: every ? ms : 0});
  scheduleNext();
  return id;
}

global.setTimeout = (fn, ms, ...args) => addTimer(fn, ms, args, false);
global.setInterval = (fn, ms, ...args) => addTimer(fn, ms, args, true);
global.clearTimeout = global.clearInterval = id => { timers.delete(id); scheduleNext(); };
global.requestAnimationFrame = fn => addTimer(() => fn(rb.now()), 16, [], false);
global.cancelAnimationFrame = global.clearTimeout;
global.requestIdleCallback = fn => addTimer(() => fn({didTimeout: false, timeRemaining: () => 10}), 1, [], false);
global.cancelIdleCallback = global.clearTimeout;
global.queueMicrotask = fn => { Promise.resolve().then(fn); };

/* Due timers in due order; one pass per tick, so an interval that is always
   late cannot starve the main loop. */
global.__rb_tick = function (now) {
  const due = [...timers].filter(([, t]) => t.due <= now).sort((a, b) => a[1].due - b[1].due);
  for (const [id, t] of due) {
    if (!timers.has(id)) continue;
    if (t.every) t.due = now + t.every;
    else timers.delete(id);
    try { t.fn.apply(global, t.args); } catch (e) { reportError(e); }
  }
  scheduleNext();
};

/* ---------------- network ---------------- */

class Headers {
  constructor(init) {
    this.__h = new Map();
    if (init) for (const [k, v] of (init instanceof Headers ? init.__h : Object.entries(init))) this.set(k, v);
  }
  get(k) { return this.__h.get(String(k).toLowerCase()) ?? null; }
  set(k, v) { this.__h.set(String(k).toLowerCase(), String(v)); }
  append(k, v) { this.set(k, v); }
  has(k) { return this.__h.has(String(k).toLowerCase()); }
  delete(k) { this.__h.delete(String(k).toLowerCase()); }
  forEach(fn) { for (const [k, v] of this.__h) fn(v, k, this); }
  entries() { return this.__h.entries(); }
  [Symbol.iterator]() { return this.__h.entries(); }
}

class Response {
  constructor(body, init) {
    init = init || {};
    this.__body = body == null ? '' : String(body);
    this.status = init.status ?? 200;
    this.statusText = init.statusText || '';
    this.ok = this.status >= 200 && this.status < 300;
    this.headers = new Headers(init.headers);
    this.url = init.url || '';
    this.redirected = false;
    this.type = 'basic';
    this.bodyUsed = false;
  }
  text() { this.bodyUsed = true; return Promise.resolve(this.__body); }
  json() { this.bodyUsed = true; return Promise.resolve().then(() => JSON.parse(this.__body)); }
  clone() { return new Response(this.__body, this); }
  arrayBuffer() { return Promise.reject(new TypeError('arrayBuffer is not supported')); }
  blob() { return Promise.reject(new TypeError('blob is not supported')); }
}

/* GET only: the OS's HTTP client has no request bodies for scripts yet.
   The transfer blocks the browser, then settles asynchronously. */
function httpGet(url) {
  const abs = rb.resolve(String(url));
  if (!abs) return null;
  return {abs, body: rb.fetch(abs)};
}

global.fetch = function (input, init) {
  const url = typeof input === 'string' ? input : (input && input.url) || String(input);
  const method = String((init && init.method) || 'GET').toUpperCase();
  return new Promise((resolve, reject) => {
    setTimeout(() => {
      if (method !== 'GET') { reject(new TypeError('fetch: only GET is supported')); return; }
      const r = httpGet(url);
      if (!r || r.body === null) reject(new TypeError('Failed to fetch ' + url));
      else resolve(new Response(r.body, {status: 200, url: r.abs}));
    }, 0);
  });
};

class XMLHttpRequest extends EventTarget {
  constructor() {
    super();
    this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = '';
    this.response = ''; this.responseType = ''; this.responseURL = ''; this.timeout = 0;
    this.withCredentials = false; this.upload = {addEventListener() {}};
    this.__l = new Map();
  }
  open(method, url, async) { this.__m = String(method).toUpperCase(); this.__u = url; this.__async = async !== false; this.readyState = 1; }
  setRequestHeader() {}
  getResponseHeader(n) { return /content-type/i.test(n) ? 'text/plain' : null; }
  getAllResponseHeaders() { return ''; }
  overrideMimeType() {}
  abort() { this.__aborted = true; }
  addEventListener(t, fn) { (this.__l.get(t) || this.__l.set(t, []).get(t)).push(fn); }
  removeEventListener(t, fn) { const a = this.__l.get(t); if (a) { const i = a.indexOf(fn); if (i >= 0) a.splice(i, 1); } }
  __emit(t) {
    const ev = new Event(t);
    ev.target = ev.currentTarget = this;
    if (typeof this['on' + t] === 'function') try { this['on' + t](ev); } catch (e) { reportError(e); }
    for (const fn of (this.__l.get(t) || []).slice()) try { fn.call(this, ev); } catch (e) { reportError(e); }
  }
  send() {
    const run = () => {
      if (this.__aborted) return;
      const r = this.__m === 'GET' ? httpGet(this.__u) : null;
      this.readyState = 4;
      if (r && r.body !== null) {
        this.status = 200; this.statusText = 'OK'; this.responseURL = r.abs;
        this.responseText = r.body;
        this.response = this.responseType === 'json' ? (() => { try { return JSON.parse(r.body); } catch (e) { return null; } })() : r.body;
        this.__emit('readystatechange'); this.__emit('load');
      } else {
        this.status = 0;
        this.__emit('readystatechange'); this.__emit('error');
      }
      this.__emit('loadend');
    };
    if (this.__async) setTimeout(run, 0); else run();
  }
}
XMLHttpRequest.UNSENT = 0; XMLHttpRequest.OPENED = 1; XMLHttpRequest.DONE = 4;

/* ---------------- misc web APIs scripts probe for ---------------- */

class Observer {
  constructor(cb) { this.__cb = cb; }
  observe() {}
  unobserve() {}
  disconnect() {}
  takeRecords() { return []; }
}

/* Elements are reported visible once, so lazy loaders do their work. */
class IntersectionObserver extends Observer {
  observe(el) {
    setTimeout(() => {
      try {
        this.__cb([{target: el, isIntersecting: true, intersectionRatio: 1,
                    boundingClientRect: el.getBoundingClientRect(), time: rb.now()}], this);
      } catch (e) { reportError(e); }
    }, 0);
  }
}

class AbortSignal extends EventTarget {
  constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
  throwIfAborted() { if (this.aborted) throw this.reason; }
  static abort(r) { const s = new AbortSignal(); s.aborted = true; s.reason = r; return s; }
  static timeout() { return new AbortSignal(); }
}
// AbortSignal is not a node: give it a JS-only listener table.
AbortSignal.prototype.addEventListener = function (t, fn) { (this.__ls || (this.__ls = [])).push(fn); };
AbortSignal.prototype.dispatchEvent = function (ev) { for (const f of this.__ls || []) f(ev); return true; };
class AbortController {
  constructor() { this.signal = new AbortSignal(); }
  abort(reason) {
    if (this.signal.aborted) return;
    this.signal.aborted = true;
    this.signal.reason = reason;
    const ev = new Event('abort');
    if (typeof this.signal.onabort === 'function') this.signal.onabort(ev);
    this.signal.dispatchEvent(ev);
  }
}

/* The page viewport in CSS px (rb_set_viewport): 622x664 on the 640x360
   screen, 304x400 in the 320x200 fallback. @media width adds the scrollbar. */
const viewW = rb.view(0), viewH = rb.view(1);

function matchMedia(q) {
  const w = viewW + 16;
  const s = String(q);
  let matches = /screen|all/.test(s) || !/print/.test(s);
  const mn = /min-width:\s*(\d+)px/.exec(s), mx = /max-width:\s*(\d+)px/.exec(s);
  if (mn && w < +mn[1]) matches = false;
  if (mx && w > +mx[1]) matches = false;
  if (/prefers-color-scheme:\s*dark/.test(s)) matches = false;
  if (/prefers-reduced-motion:\s*reduce/.test(s)) matches = true;
  return {matches, media: s, onchange: null, addListener() {}, removeListener() {},
          addEventListener() {}, removeEventListener() {}};
}

function getComputedStyle(el) {
  const inline = el && el.style ? el.style : null;
  return new Proxy({}, {
    get(t, p) {
      if (p === 'getPropertyValue') return k => inline ? inline.getPropertyValue(k) : '';
      if (typeof p !== 'string') return undefined;
      if (p === 'display') return el.hidden ? 'none' : (inline && inline.display) || (el.getBoundingClientRect().width ? 'block' : 'none');
      if (p === 'visibility') return (inline && inline.visibility) || 'visible';
      return inline ? inline[p] : '';
    },
  });
}

class TextEncoder {
  get encoding() { return 'utf-8'; }
  encode(s) {
    const u = unescape(encodeURIComponent(String(s ?? '')));
    const out = new Uint8Array(u.length);
    for (let i = 0; i < u.length; i++) out[i] = u.charCodeAt(i);
    return out;
  }
}
class TextDecoder {
  get encoding() { return 'utf-8'; }
  decode(b) {
    if (!b) return '';
    const a = b instanceof Uint8Array ? b : new Uint8Array(b.buffer || b);
    let s = '';
    for (let i = 0; i < a.length; i++) s += String.fromCharCode(a[i]);
    try { return decodeURIComponent(escape(s)); } catch (e) { return s; }
  }
}

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function btoa(s) {
  s = String(s);
  let out = '';
  for (let i = 0; i < s.length; i += 3) {
    const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
    if (a > 255 || b > 255 || c > 255) throw new DOMException('Invalid character', 'InvalidCharacterError');
    const n = (a << 16) | ((b || 0) << 8) | (c || 0);
    out += B64[n >> 18 & 63] + B64[n >> 12 & 63] + (i + 1 < s.length ? B64[n >> 6 & 63] : '=') + (i + 2 < s.length ? B64[n & 63] : '=');
  }
  return out;
}
function atob(s) {
  s = String(s).replace(/[\s=]/g, '');
  let out = '';
  for (let i = 0; i < s.length; i += 4) {
    const n = (B64.indexOf(s[i]) << 18) | (B64.indexOf(s[i + 1]) << 12) |
              ((B64.indexOf(s[i + 2]) & 63) << 6) | (B64.indexOf(s[i + 3]) & 63);
    out += String.fromCharCode(n >> 16 & 255);
    if (i + 2 < s.length) out += String.fromCharCode(n >> 8 & 255);
    if (i + 3 < s.length) out += String.fromCharCode(n & 255);
  }
  return out;
}

/* Hardware RNG only (rb.randomFill throws without one): Math.random is not
   acceptable for values pages treat as unguessable. */
const crypto = {
  getRandomValues(a) { return rb.randomFill(a); },
  randomUUID() {
    const b = rb.randomFill(new Uint8Array(16));
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    const h = [...b].map(x => x.toString(16).padStart(2, '0')).join('');
    return h.slice(0, 8) + '-' + h.slice(8, 12) + '-' + h.slice(12, 16) + '-' + h.slice(16, 20) + '-' + h.slice(20);
  },
  subtle: undefined,
};

const performance = {
  now: () => rb.now() - startTime,
  timeOrigin: Date.now(),
  mark() {}, measure() {}, getEntriesByType: () => [], getEntriesByName: () => [], getEntries: () => [],
  clearMarks() {}, clearMeasures() {},
  timing: {navigationStart: Date.now()},
};
const startTime = rb.now();

/* ---------------- window ---------------- */

Object.assign(global, {
  window: global, self: global, top: global, parent: global, frames: global, globalThis: global,
  document, location, history, navigator, performance, crypto,
  localStorage: new Storage(), sessionStorage: new Storage(),
  Node, Element, HTMLElement, Text, Comment, CharacterData, Document, HTMLDocument: Document,
  DocumentFragment: HTMLElement, EventTarget, Event, CustomEvent, UIEvent, MouseEvent, KeyboardEvent,
  FocusEvent, InputEvent, SubmitEvent, PointerEvent: MouseEvent, TouchEvent: UIEvent, WheelEvent: MouseEvent,
  DOMException, DOMTokenList, URL, URLSearchParams, Headers, Response, Request: class Request {
    constructor(u, init) { this.url = String(u); this.method = (init && init.method) || 'GET'; }
  },
  XMLHttpRequest, DOMParser, XMLSerializer: class XMLSerializer { serializeToString(n) { return n.outerHTML || n.textContent || ''; } },
  FormData: class FormData { constructor() { this.__p = []; } append(k, v) { this.__p.push([k, v]); } get(k) { const e = this.__p.find(p => p[0] === k); return e ? e[1] : null; } },
  Storage, MutationObserver: Observer, ResizeObserver: Observer, PerformanceObserver: Observer,
  IntersectionObserver, AbortController, AbortSignal, TextEncoder, TextDecoder,
  Image,
  Audio: function Audio() { return document.createElement('audio'); },
  matchMedia, getComputedStyle, btoa, atob,
  alert(msg) { console.log('[alert] ' + msg); },
  confirm(msg) { console.log('[confirm] ' + msg); return false; },
  prompt(msg) { console.log('[prompt] ' + msg); return null; },
  open() { return null; }, close() {}, stop() {}, focus() {}, blur() {}, print() {},
  postMessage() {}, scroll() {}, scrollTo() {}, scrollBy() {},
  getSelection: () => document.getSelection(),
  innerWidth: viewW, innerHeight: viewH, outerWidth: viewW + 16, outerHeight: viewH + 56,
  devicePixelRatio: 1, scrollX: 0, scrollY: 0, pageXOffset: 0, pageYOffset: 0,
  screen: {width: viewW + 16, height: viewH + 56, availWidth: viewW + 16, availHeight: viewH + 56,
           colorDepth: 24, pixelDepth: 24,
           orientation: {type: 'landscape-primary', angle: 0, addEventListener() {}}},
  isSecureContext: true, origin: location.origin, name: '', closed: false, length: 0,
  visualViewport: {width: viewW, height: viewH, scale: 1, addEventListener() {}, removeEventListener() {}},
  CSS: {supports: () => false, escape: cssEscape},
  reportError,
});
global.addEventListener = (t, fn, o) => addListener(global, t, fn, o);
global.removeEventListener = (t, fn, o) => removeListener(global, t, fn, o);
global.dispatchEvent = ev => {
  ev.target = ev.currentTarget = global;
  ev.eventPhase = 2;
  invoke(global, ev, true);
  invoke(global, ev, false);
  return !ev.defaultPrevented;
};

/* ---------------- host hooks ---------------- */

/* Inline handler attributes (onclick="...") become listeners. Done after
   each script so handlers created by markup in later scripts work too. */
const HANDLER_ATTRS = ['click', 'change', 'input', 'submit', 'keydown', 'keyup', 'keypress',
  'mousedown', 'mouseup', 'mouseover', 'mouseout', 'focus', 'blur', 'load', 'error'];
const handlerDone = new Set();
function bindInlineHandlers() {
  for (const t of HANDLER_ATTRS) {
    for (const h of rb.query(1, '[on' + t + ']')) {
      const key = h + ':' + rb.gen(h) + ':' + t;
      if (handlerDone.has(key)) continue;
      handlerDone.add(key);
      const code = rb.getAttr(h, 'on' + t);
      let fn;
      try { fn = new Function('event', code); } catch (e) { reportError(e); continue; }
      const el = wrap(h);
      if (t === 'load' && el.localName === 'body') {
        global.addEventListener('load', ev => fn.call(global, ev));
        continue;
      }
      addListener(el, t, function (ev) {
        if (fn.call(this, ev) === false) ev.preventDefault();
      }, false);
    }
  }
}

for (const t of ['click', 'mousedown', 'mouseup', 'input', 'change', 'submit', 'focus', 'blur',
                 'keydown', 'keyup', 'keypress', 'scroll', 'load']) hookNative(document, t, false);

global.__rb_begin_script = function (h) {
  currentScript = h;
  if (h) startedScripts.add(scriptKey(h));
  if (!h) bindInlineHandlers();
};

global.__rb_img_event = function (h, ok) {
  const el = wrap(h);
  if (el) jsDispatch(el, new Event(ok ? 'load' : 'error'));
};

global.__rb_loaded = function () {
  bindInlineHandlers();
  readyState = 'interactive';
  document.dispatchEvent(new Event('readystatechange'));
  document.dispatchEvent(new Event('DOMContentLoaded', {bubbles: true}));
  readyState = 'complete';
  document.dispatchEvent(new Event('readystatechange'));
};

})(globalThis);
