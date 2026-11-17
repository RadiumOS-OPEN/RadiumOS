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
/* Markup or an on* attribute was added since inline handlers were last bound
   (bindInlineHandlers, at the end of the file). */
let inlineStale = false;

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
                /* the engine's y counts screen rows; CSS px are half a row
                   (as in getBoundingClientRect) */
                clientX: x, clientY: y * 2, button: button < 0 ? 0 : button, buttons,
                shiftKey: !!(mods & 1), ctrlKey: !!(mods & 2), altKey: !!(mods & 4)};
  let ev;
  if (/^key/.test(type)) {
    const k = KEY_NAMES[key] || (key ? String.fromCharCode(key) : 'Unidentified');
    ev = new KeyboardEvent(type, Object.assign(init, {key: k, keyCode: key === 10 ? 13 : key,
      code: k.length === 1 && /[a-z]/i.test(k) ? 'Key' + k.toUpperCase() : k}));
  } else if (/^(click|mouse|dblclick|contextmenu)/.test(type)) {
    ev = new MouseEvent(type, init);
    /* offsetX/Y: from the target's box. Images and canvases show one pixel
       per screen row (they are not halved vertically), so on them offsetY
       counts rows: e.offsetX/e.offsetY are canvas pixels at the natural size. */
    const r = target && target.__h > 1 ? rb.rect(target.__h) : null;
    if (r) {
      const tag = rb.tag(target.__h);
      ev.offsetX = x - r[0];
      ev.offsetY = tag === 'canvas' || tag === 'img' ? y - r[1] : (y - r[1]) * 2;
    }
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
  if (inlineStale) bindInlineHandlers();
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
    if (child.nodeType === 1) inlineStale = true;
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
    if (rb.first(this.__h)) rb.setHtml(this.__h, '');   /* the children nobody holds */
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

/* Does a script hold this node or anything inside it? Nodes only reach
   scripts through wrap(), and every wrapper stays in the cache. */
function held(root) {
  let n = root;
  for (;;) {
    const c = cache.get(n);
    if (c && c.gen === rb.gen(n)) return true;
    let k = rb.first(n);
    while (!k) {
      if (n === root) return false;
      k = rb.next(n);
      if (!k) n = rb.parent(n);
    }
    n = k;
  }
}

/* Replacing an element's content in the engine recycles the old children,
   but scripts may still hold them (jQuery moves them right back): those are
   detached first so they stay valid. A child no script has ever seen is
   left for the engine to recycle: detaching it too kept every old node
   alive, and a page that sets textContent or innerHTML in a timer ran out
   of nodes after a few thousand updates. Text nodes have no children. */
function detachChildren(h) {
  if (rb.isText(h)) return;
  for (let c = rb.first(h), n; c; c = n) {
    n = rb.next(c);
    if (held(c) && !rb.detach(c)) break;
  }
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
  setAttribute(n, v) {
    n = String(n).toLowerCase();
    rb.setAttr(this.__h, n, String(v));
    if (n.startsWith('on')) inlineStale = true;
  }
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
    inlineStale = true;
  }
  get outerHTML() { return rb.html(this.__h, 1); }
  set outerHTML(v) {
    if (!rb.parent(this.__h)) return;
    rb.adjHtml(this.__h, 0, String(v));
    rb.detach(this.__h);
    inlineStale = true;
  }
  get innerText() { return this.textContent; }
  set innerText(v) { this.textContent = v; }
  get outerText() { return this.textContent; }
  insertAdjacentHTML(where, html) {
    const i = ['beforebegin', 'afterbegin', 'beforeend', 'afterend'].indexOf(String(where).toLowerCase());
    if (i < 0) throw new DOMException('Bad position', 'SyntaxError');
    rb.adjHtml(this.__h, i, String(html));
    inlineStale = true;
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

/* ---------------- <canvas> ----------------
   The 2D context keeps its state here; paths are flat Float64Array command
   lists (MOVE x y, LINE x y, QUAD, CUBIC, CLOSE; arcs become cubics) that
   the natives in rjs_canvas.c flatten, stroke and rasterize into the
   canvas's pixels (canvas.c). The context's current path is recorded in
   device space (each point through the transform of the moment, as the
   spec says); a Path2D is in user space and goes through the transform of
   the fill/stroke. Simplifications: clip() keeps the bounding box of the
   clip path, text is the 8x8 system font scaled to the font size (advance
   = half the size), patterns, shadows and filters do not paint, and
   composite operations other than source-over, copy, destination-out and
   lighter draw as source-over. */
const PATH_MOVE = 0, PATH_LINE = 1, PATH_QUAD = 2, PATH_CUBIC = 3, PATH_CLOSE = 4;
const TAU = 2 * Math.PI;
const IDENTITY = [1, 0, 0, 1, 0, 0];
const COMPOSITE = {'source-over': 0, 'copy': 1, 'destination-out': 2, 'lighter': 3};
const COMPOSITE_NAMES = new Set(['source-over', 'source-in', 'source-out', 'source-atop', 'destination-over',
  'destination-in', 'destination-out', 'destination-atop', 'lighter', 'copy', 'xor', 'multiply', 'screen',
  'overlay', 'darken', 'lighten', 'color-dodge', 'color-burn', 'hard-light', 'soft-light', 'difference',
  'exclusion', 'hue', 'saturation', 'color', 'luminosity']);
const CAPS = ['butt', 'round', 'square'], JOINS = ['miter', 'round', 'bevel'];

function finite(...a) { for (const v of a) if (!Number.isFinite(v)) return false; return true; }

class PathData {
  constructor(m) {
    this.d = new Float64Array(64);
    this.n = 0;
    this.m = m || null;      // transform applied while recording (the context's current path)
    this.started = false;    // there is a current point
    this.lx = 0; this.ly = 0; // current point, as recorded
    this.sx = 0; this.sy = 0; // start of the current subpath, as recorded
  }
  __room(k) {
    if (this.n + k <= this.d.length) return;
    let len = this.d.length * 2;
    while (len < this.n + k) len *= 2;
    const nd = new Float64Array(len);
    nd.set(this.d.subarray(0, this.n));
    this.d = nd;
  }
  __pt(x, y) {
    const m = this.m;
    if (m) { const X = m[0] * x + m[2] * y + m[4]; y = m[1] * x + m[3] * y + m[5]; x = X; }
    this.d[this.n++] = x; this.d[this.n++] = y;
    this.lx = x; this.ly = y;
  }
  clear() { this.n = 0; this.started = false; }
  move(x, y) {
    this.__room(3); this.d[this.n++] = PATH_MOVE; this.__pt(x, y);
    this.started = true; this.sx = this.lx; this.sy = this.ly;
  }
  line(x, y) {
    if (!this.started) { this.move(x, y); return; }
    this.__room(3); this.d[this.n++] = PATH_LINE; this.__pt(x, y);
  }
  quad(ax, ay, x, y) {
    if (!this.started) this.move(ax, ay);
    this.__room(5); this.d[this.n++] = PATH_QUAD; this.__pt(ax, ay); this.__pt(x, y);
  }
  cubic(ax, ay, bx, by, x, y) {
    if (!this.started) this.move(ax, ay);
    this.__room(7); this.d[this.n++] = PATH_CUBIC; this.__pt(ax, ay); this.__pt(bx, by); this.__pt(x, y);
  }
  close() {
    if (!this.started) return;
    this.__room(1); this.d[this.n++] = PATH_CLOSE;
    this.lx = this.sx; this.ly = this.sy;
  }
  /* The current point in user space: as recorded for a Path2D; for the
     context's path, the device point through the inverse of the current
     transform (the spec's rule for arcTo). */
  current(inv) {
    if (!this.m || !inv) return [this.lx, this.ly];
    return [inv[0] * this.lx + inv[2] * this.ly + inv[4], inv[1] * this.lx + inv[3] * this.ly + inv[5]];
  }
  ellipse(x, y, rx, ry, rot, a0, a1, ccw) {
    let sweep = a1 - a0;
    if (!ccw) {
      if (sweep >= TAU) sweep = TAU;
      else { sweep %= TAU; if (sweep < 0) sweep += TAU; }
    } else {
      if (-sweep >= TAU) sweep = -TAU;
      else { sweep %= TAU; if (sweep > 0) sweep -= TAU; }
    }
    const cr = Math.cos(rot), sr = Math.sin(rot);
    const at = (ux, uy) => [x + rx * cr * ux - ry * sr * uy, y + rx * sr * ux + ry * cr * uy];
    const p0 = at(Math.cos(a0), Math.sin(a0));
    if (this.started) this.line(p0[0], p0[1]); else this.move(p0[0], p0[1]);
    if (sweep === 0) return;
    const segs = Math.max(1, Math.ceil(Math.abs(sweep) / (Math.PI / 2) - 1e-9));
    const step = sweep / segs, k = 4 / 3 * Math.tan(step / 4);
    let a = a0;
    for (let i = 0; i < segs; i++) {
      const b = a + step;
      const ca = Math.cos(a), sa = Math.sin(a), cb = Math.cos(b), sb = Math.sin(b);
      const c1 = at(ca - k * sa, sa + k * ca), c2 = at(cb + k * sb, sb - k * cb), e = at(cb, sb);
      this.cubic(c1[0], c1[1], c2[0], c2[1], e[0], e[1]);
      a = b;
    }
  }
}

/* Path-building methods shared by Path2D and the context. __p() returns the
   PathData; __inv() the inverse transform for the current point (null: user
   space). */
const PathMethods = {
  moveTo(x, y) { x = +x; y = +y; if (finite(x, y)) this.__p().move(x, y); },
  lineTo(x, y) { x = +x; y = +y; if (finite(x, y)) this.__p().line(x, y); },
  quadraticCurveTo(ax, ay, x, y) { if (finite(+ax, +ay, +x, +y)) this.__p().quad(+ax, +ay, +x, +y); },
  bezierCurveTo(ax, ay, bx, by, x, y) {
    if (finite(+ax, +ay, +bx, +by, +x, +y)) this.__p().cubic(+ax, +ay, +bx, +by, +x, +y);
  },
  closePath() { this.__p().close(); },
  rect(x, y, w, h) {
    x = +x; y = +y; w = +w; h = +h;
    if (!finite(x, y, w, h)) return;
    const p = this.__p();
    p.move(x, y); p.line(x + w, y); p.line(x + w, y + h); p.line(x, y + h); p.close();
    p.move(x, y);
  },
  roundRect(x, y, w, h, radii) {
    x = +x; y = +y; w = +w; h = +h;
    if (!finite(x, y, w, h)) return;
    let r = radii === undefined ? [0] : (Array.isArray(radii) ? radii : [radii]);
    if (r.length < 1 || r.length > 4) throw new RangeError('roundRect: 1 to 4 radii');
    r = r.map(v => { const n = typeof v === 'object' && v ? +v.x : +v; if (n < 0) throw new RangeError('roundRect: negative radius'); return n || 0; });
    let tl, tr, br, bl;
    if (r.length === 1) tl = tr = br = bl = r[0];
    else if (r.length === 2) { tl = br = r[0]; tr = bl = r[1]; }
    else if (r.length === 3) { tl = r[0]; tr = bl = r[1]; br = r[2]; }
    else [tl, tr, br, bl] = r;
    if (w < 0) { x += w; w = -w; [tl, tr, br, bl] = [tr, tl, bl, br]; }
    if (h < 0) { y += h; h = -h; [tl, tr, br, bl] = [bl, br, tr, tl]; }
    const f = Math.min(1, w / (tl + tr || 1), w / (bl + br || 1), h / (tl + bl || 1), h / (tr + br || 1));
    tl *= f; tr *= f; br *= f; bl *= f;
    const p = this.__p();
    p.move(x + tl, y);
    p.line(x + w - tr, y);
    if (tr) p.ellipse(x + w - tr, y + tr, tr, tr, 0, -Math.PI / 2, 0, false);
    p.line(x + w, y + h - br);
    if (br) p.ellipse(x + w - br, y + h - br, br, br, 0, 0, Math.PI / 2, false);
    p.line(x + bl, y + h);
    if (bl) p.ellipse(x + bl, y + h - bl, bl, bl, 0, Math.PI / 2, Math.PI, false);
    p.line(x, y + tl);
    if (tl) p.ellipse(x + tl, y + tl, tl, tl, 0, Math.PI, Math.PI * 1.5, false);
    p.close();
    p.move(x, y);
  },
  arc(x, y, r, a0, a1, ccw) {
    x = +x; y = +y; r = +r; a0 = +a0; a1 = +a1;
    if (!finite(x, y, r, a0, a1)) return;
    if (r < 0) throw new DOMException('The radius provided (' + r + ') is negative.', 'IndexSizeError');
    this.__p().ellipse(x, y, r, r, 0, a0, a1, !!ccw);
  },
  ellipse(x, y, rx, ry, rot, a0, a1, ccw) {
    x = +x; y = +y; rx = +rx; ry = +ry; rot = +rot; a0 = +a0; a1 = +a1;
    if (!finite(x, y, rx, ry, rot, a0, a1)) return;
    if (rx < 0 || ry < 0) throw new DOMException('The radius provided is negative.', 'IndexSizeError');
    this.__p().ellipse(x, y, rx, ry, rot, a0, a1, !!ccw);
  },
  arcTo(x1, y1, x2, y2, r) {
    x1 = +x1; y1 = +y1; x2 = +x2; y2 = +y2; r = +r;
    if (!finite(x1, y1, x2, y2, r)) return;
    if (r < 0) throw new DOMException('The radius provided (' + r + ') is negative.', 'IndexSizeError');
    const p = this.__p();
    if (!p.started) { p.move(x1, y1); return; }
    const [x0, y0] = p.current(this.__inv());
    const ax = x0 - x1, ay = y0 - y1, bx = x2 - x1, by = y2 - y1;
    const la = Math.hypot(ax, ay), lb = Math.hypot(bx, by);
    const cross = ax * by - ay * bx;
    if ((x0 === x1 && y0 === y1) || (x1 === x2 && y1 === y2) || r === 0 || la === 0 || lb === 0 ||
        Math.abs(cross) < 1e-9 * la * lb) { p.line(x1, y1); return; }
    const cos = (ax * bx + ay * by) / (la * lb);
    const half = Math.acos(Math.max(-1, Math.min(1, cos))) / 2;
    const dist = r / Math.tan(half);
    const t0x = x1 + ax / la * dist, t0y = y1 + ay / la * dist;
    const t1x = x1 + bx / lb * dist, t1y = y1 + by / lb * dist;
    /* centre: from the corner along the bisector */
    const mx = ax / la + bx / lb, my = ay / la + by / lb, ml = Math.hypot(mx, my);
    const cd = r / Math.sin(half);
    const cx = x1 + mx / ml * cd, cy = y1 + my / ml * cd;
    const s = Math.atan2(t0y - cy, t0x - cx), e = Math.atan2(t1y - cy, t1x - cx);
    p.line(t0x, t0y);
    p.ellipse(cx, cy, r, r, 0, s, e, cross > 0);
  },
};

class Path2D {
  constructor(src) {
    this.__d = new PathData(null);
    if (src instanceof Path2D) {
      this.__d.__room(src.__d.n);
      this.__d.d.set(src.__d.d.subarray(0, src.__d.n));
      this.__d.n = src.__d.n;
      for (const k of ['started', 'lx', 'ly', 'sx', 'sy']) this.__d[k] = src.__d[k];
    } else if (typeof src === 'string') {
      parseSvgPath(this, src);
    }
  }
  __p() { return this.__d; }
  __inv() { return null; }
  addPath(path, m) {
    if (!(path instanceof Path2D)) throw new TypeError('addPath: not a Path2D');
    const t = m ? [m.a ?? 1, m.b ?? 0, m.c ?? 0, m.d ?? 1, m.e ?? 0, m.f ?? 0] : IDENTITY;
    const src = path.__d, d = this.__d;
    let i = 0;
    while (i < src.n) {
      const op = src.d[i];
      const k = op === PATH_CLOSE ? 0 : op === PATH_QUAD ? 4 : op === PATH_CUBIC ? 6 : 2;
      d.__room(1 + k);
      d.d[d.n++] = op;
      for (let j = 0; j < k; j += 2) {
        const x = src.d[i + 1 + j], y = src.d[i + 2 + j];
        d.d[d.n++] = t[0] * x + t[2] * y + t[4];
        d.d[d.n++] = t[1] * x + t[3] * y + t[5];
      }
      i += 1 + k;
    }
    if (src.started) {
      d.started = true;
      d.lx = t[0] * src.lx + t[2] * src.ly + t[4]; d.ly = t[1] * src.lx + t[3] * src.ly + t[5];
      d.sx = t[0] * src.sx + t[2] * src.sy + t[4]; d.sy = t[1] * src.sx + t[3] * src.sy + t[5];
    }
  }
}
Object.assign(Path2D.prototype, PathMethods);

/* SVG path data (new Path2D('M0 0 L10 10 ...')): M L H V C S Q T A Z,
   absolute and relative. Keeps what parsed before the first error, as the
   spec says. */
function parseSvgPath(path, str) {
  const toks = String(str).match(/[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?/g) || [];
  const p = path.__d;
  let i = 0, cmd = '', prev = '', x = 0, y = 0, sx = 0, sy = 0, c2x = 0, c2y = 0, qx = 0, qy = 0;
  const isCmd = t => /[A-Za-z]/.test(t);
  const num = () => { if (i >= toks.length || isCmd(toks[i])) throw 0; return parseFloat(toks[i++]); };
  const flag = () => { const v = num(); if (v !== 0 && v !== 1) throw 0; return v === 1; };
  try {
    while (i < toks.length) {
      if (isCmd(toks[i])) cmd = toks[i++];
      else if (!cmd || cmd === 'Z' || cmd === 'z') break;
      if (!prev && cmd !== 'M' && cmd !== 'm') break;
      const rel = cmd >= 'a', C = cmd.toUpperCase(), ox = rel ? x : 0, oy = rel ? y : 0;
      switch (C) {
        case 'Z': p.close(); x = sx; y = sy; break;
        case 'M': x = ox + num(); y = oy + num(); p.move(x, y); sx = x; sy = y; cmd = rel ? 'l' : 'L'; break;
        case 'L': x = ox + num(); y = oy + num(); p.line(x, y); break;
        case 'H': x = ox + num(); p.line(x, y); break;
        case 'V': y = oy + num(); p.line(x, y); break;
        case 'C': {
          const a = ox + num(), b = oy + num();
          c2x = ox + num(); c2y = oy + num(); x = ox + num(); y = oy + num();
          p.cubic(a, b, c2x, c2y, x, y);
          break;
        }
        case 'S': {
          const smooth = prev === 'C' || prev === 'S';
          const a = smooth ? 2 * x - c2x : x, b = smooth ? 2 * y - c2y : y;
          c2x = ox + num(); c2y = oy + num(); x = ox + num(); y = oy + num();
          p.cubic(a, b, c2x, c2y, x, y);
          break;
        }
        case 'Q': qx = ox + num(); qy = oy + num(); x = ox + num(); y = oy + num(); p.quad(qx, qy, x, y); break;
        case 'T':
          if (prev === 'Q' || prev === 'T') { qx = 2 * x - qx; qy = 2 * y - qy; } else { qx = x; qy = y; }
          x = ox + num(); y = oy + num();
          p.quad(qx, qy, x, y);
          break;
        case 'A': {
          const rx = Math.abs(num()), ry = Math.abs(num()), rot = num() * Math.PI / 180;
          const large = flag(), sweep = flag(), ex = ox + num(), ey = oy + num();
          svgArc(p, x, y, rx, ry, rot, large, sweep, ex, ey);
          x = ex; y = ey;
          break;
        }
        default: throw 0;
      }
      prev = C;
    }
  } catch (e) { /* malformed: keep what parsed */ }
}

function svgArc(p, x0, y0, rx, ry, phi, large, sweep, x, y) {
  if (x0 === x && y0 === y) return;
  if (!rx || !ry) { p.line(x, y); return; }
  const c = Math.cos(phi), s = Math.sin(phi);
  const dx = (x0 - x) / 2, dy = (y0 - y) / 2;
  const x1 = c * dx + s * dy, y1 = -s * dx + c * dy;
  const lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
  if (lam > 1) { rx *= Math.sqrt(lam); ry *= Math.sqrt(lam); }
  const num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
  const den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
  let k = Math.sqrt(Math.max(0, num / den));
  if (large === sweep) k = -k;
  const cxp = k * rx * y1 / ry, cyp = -k * ry * x1 / rx;
  const cx = c * cxp - s * cyp + (x0 + x) / 2, cy = s * cxp + c * cyp + (y0 + y) / 2;
  const ang = (ux, uy) => Math.atan2(uy, ux);
  const a0 = ang((x1 - cxp) / rx, (y1 - cyp) / ry);
  let da = ang((-x1 - cxp) / rx, (-y1 - cyp) / ry) - a0;
  if (sweep && da < 0) da += TAU;
  if (!sweep && da > 0) da -= TAU;
  p.ellipse(cx, cy, rx, ry, phi, a0, a0 + da, !sweep);
}

class CanvasGradient {
  constructor(kind, args) { this.__k = kind; this.__a = args; this.__stops = []; this.__f = null; }
  addColorStop(offset, color) {
    offset = +offset;
    if (!(offset >= 0 && offset <= 1)) throw new DOMException('The provided offset (' + offset + ') is outside the range (0.0, 1.0).', 'IndexSizeError');
    const c = rb.cvColor(String(color));
    if (c === null) throw new DOMException('The value provided (\'' + color + '\') could not be parsed as a color.', 'SyntaxError');
    /* stable: equal offsets keep their order */
    let i = this.__stops.length;
    while (i > 0 && this.__stops[i - 1][0] > offset) i--;
    this.__stops.splice(i, 0, [offset, c]);
    this.__f = null;
  }
  __send(m) {
    const st = this.__stops;
    if (!st.length || this.__k === 3) { rb.cvPaintNone(); return; }
    if (!this.__f) {
      const f = new Float64Array(st.length * 2);
      st.forEach((s, i) => { f[2 * i] = s[0]; f[2 * i + 1] = s[1]; });
      this.__f = f;
    }
    if (st.length === 1) { rb.cvPaint(st[0][1]); return; }
    const a = this.__a;
    rb.cvGradient(this.__k, a[0], a[1], a[2], a[3], a[4], a[5], this.__f, m[0], m[1], m[2], m[3], m[4], m[5]);
  }
}
class CanvasPattern { setTransform() {} }

class ImageData {
  constructor(a, b, c) {
    if (a instanceof Uint8ClampedArray) {
      const w = b >>> 0, h = c === undefined ? a.length / 4 / w : c >>> 0;
      if (!w || a.length % 4 || a.length !== w * h * 4) throw new DOMException('The input data length is not a multiple of (4 * width).', 'IndexSizeError');
      this.data = a; this.width = w; this.height = h;
    } else {
      const w = a >>> 0, h = b >>> 0;
      if (!w || !h) throw new DOMException('The source width is 0.', 'IndexSizeError');
      if (w * h > 16777216) throw new RangeError('ImageData too large');
      this.data = new Uint8ClampedArray(w * h * 4); this.width = w; this.height = h;
    }
    this.colorSpace = 'srgb';
  }
}

class DOMMatrix {
  constructor(init) {
    let m = IDENTITY;
    if (Array.isArray(init) && init.length === 6) m = init.map(Number);
    this.a = m[0]; this.b = m[1]; this.c = m[2]; this.d = m[3]; this.e = m[4]; this.f = m[5];
  }
  get m11() { return this.a; } get m12() { return this.b; } get m21() { return this.c; }
  get m22() { return this.d; } get m41() { return this.e; } get m42() { return this.f; }
  get is2D() { return true; }
  get isIdentity() { return this.a === 1 && !this.b && !this.c && this.d === 1 && !this.e && !this.f; }
  __m() { return [this.a, this.b, this.c, this.d, this.e, this.f]; }
  multiply(o) { return new DOMMatrix(matMul(this.__m(), [o.a, o.b, o.c, o.d, o.e, o.f])); }
  translate(x, y) { return this.multiply({a: 1, b: 0, c: 0, d: 1, e: +x || 0, f: +y || 0}); }
  scale(sx, sy) { if (sy === undefined) sy = sx; return this.multiply({a: +sx, b: 0, c: 0, d: +sy, e: 0, f: 0}); }
  rotate(deg) { const r = (+deg || 0) * Math.PI / 180, c = Math.cos(r), s = Math.sin(r); return this.multiply({a: c, b: s, c: -s, d: c, e: 0, f: 0}); }
  inverse() { const i = matInv(this.__m()); return new DOMMatrix(i || [NaN, NaN, NaN, NaN, NaN, NaN]); }
  transformPoint(p) { p = p || {}; const x = +p.x || 0, y = +p.y || 0; return {x: this.a * x + this.c * y + this.e, y: this.b * x + this.d * y + this.f, z: 0, w: 1}; }
  toString() { return 'matrix(' + this.__m().join(', ') + ')'; }
}

function matMul(m, n) {
  return [m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1],
          m[0] * n[2] + m[2] * n[3], m[1] * n[2] + m[3] * n[3],
          m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]];
}
function matInv(m) {
  const det = m[0] * m[3] - m[1] * m[2];
  if (!det || !Number.isFinite(det)) return null;
  return [m[3] / det, -m[1] / det, -m[2] / det, m[0] / det,
          (m[2] * m[5] - m[3] * m[4]) / det, (m[1] * m[4] - m[0] * m[5]) / det];
}

function colorString(c) {
  const a = Math.floor(c / 16777216), r = (c >>> 16) & 255, g = (c >>> 8) & 255, b = c & 255;
  if (a === 255) return '#' + ((1 << 24) | (r << 16) | (g << 8) | b).toString(16).slice(1);
  let as = String(Math.round(a / 255 * 1000) / 1000);
  if (a === 0) as = '0';
  return 'rgba(' + r + ', ' + g + ', ' + b + ', ' + as + ')';
}

function fontSize(f) {
  const m = /(?:^|\s)(\d*\.?\d+)(px|pt|em|rem|%)(?:\s*\/\s*\S+)?\s+\S/.exec(f);
  if (!m) return 0;
  const v = parseFloat(m[1]);
  return m[2] === 'px' ? v : m[2] === 'pt' ? v * 4 / 3 : m[2] === '%' ? v * 16 / 100 : v * 16;
}

function defaultState() {
  return {
    m: IDENTITY.slice(), fill: 0xFF000000, stroke: 0xFF000000, lw: 1, cap: 0, join: 0, miter: 10,
    dash: [], dashF: null, dashOff: 0, alpha: 1, op: 'source-over', clip: null,
    font: '10px sans-serif', size: 10, align: 'start', baseline: 'alphabetic', direction: 'inherit',
    smooth: true, smoothQ: 'low', shadowBlur: 0, shadowColor: 'rgba(0, 0, 0, 0)', shadowX: 0, shadowY: 0,
    filter: 'none', letterSpacing: '0px', wordSpacing: '0px', textRendering: 'auto', fontKerning: 'auto',
  };
}

function copyState(s) { const c = Object.assign({}, s); c.m = s.m.slice(); c.dash = s.dash.slice(); return c; }

function codePoints(t) { let n = 0; for (const ch of t) n++; return n; }

class CanvasRenderingContext2D {
  constructor(canvas, slot) {
    this.__c = canvas;
    this.__slot = slot;
    this.__s = defaultState();
    this.__stack = [];
    this.__path = new PathData(this.__s.m);
  }
  get canvas() { return this.__c; }
  __p() { return this.__path; }
  __inv() { return matInv(this.__s.m); }
  __reset() {
    this.__s = defaultState();
    this.__stack = [];
    this.__path = new PathData(this.__s.m);
  }
  __setM(m) {
    this.__s.m = m;
    this.__path.m = m;
  }
  /* send paint + compositing to the natives before a draw */
  __prep(style) {
    const s = this.__s;
    if (typeof style === 'number') rb.cvPaint(style);
    else if (style instanceof CanvasGradient) style.__send(s.m);
    else rb.cvPaintNone();
    const c = s.clip;
    rb.cvComp(Math.round(s.alpha * 255), COMPOSITE[s.op] ?? 0, s.smooth ? 1 : 0,
              c ? c[0] : -1, c ? c[1] : -1, c ? c[2] : -1, c ? c[3] : -1);
  }
  __dashF() {
    const s = this.__s;
    if (!s.dash.length) return null;
    if (!s.dashF) s.dashF = new Float64Array(s.dash);
    return s.dashF;
  }

  /* state */
  save() { this.__stack.push(copyState(this.__s)); }
  restore() {
    if (!this.__stack.length) return;
    this.__s = this.__stack.pop();
    this.__path.m = this.__s.m;
  }
  reset() {
    this.__c.__clear();
    this.__reset();
  }
  isContextLost() { return false; }
  getContextAttributes() { return {alpha: true, colorSpace: 'srgb', desynchronized: false, willReadFrequently: false}; }

  /* transform */
  scale(x, y) { x = +x; y = +y; if (finite(x, y)) this.__setM(matMul(this.__s.m, [x, 0, 0, y, 0, 0])); }
  rotate(a) { a = +a; if (finite(a)) { const c = Math.cos(a), s = Math.sin(a); this.__setM(matMul(this.__s.m, [c, s, -s, c, 0, 0])); } }
  translate(x, y) { x = +x; y = +y; if (finite(x, y)) this.__setM(matMul(this.__s.m, [1, 0, 0, 1, x, y])); }
  transform(a, b, c, d, e, f) {
    const m = [+a, +b, +c, +d, +e, +f];
    if (finite(...m)) this.__setM(matMul(this.__s.m, m));
  }
  setTransform(a, b, c, d, e, f) {
    if (a === undefined) { this.__setM(IDENTITY.slice()); return; }
    const m = typeof a === 'object' ? [a.a ?? a.m11 ?? 1, a.b ?? a.m12 ?? 0, a.c ?? a.m21 ?? 0, a.d ?? a.m22 ?? 1, a.e ?? a.m41 ?? 0, a.f ?? a.m42 ?? 0].map(Number)
                                    : [+a, +b, +c, +d, +e, +f];
    if (finite(...m)) this.__setM(m);
  }
  resetTransform() { this.__setM(IDENTITY.slice()); }
  getTransform() { return new DOMMatrix(this.__s.m.slice()); }

  /* styles */
  get fillStyle() { const f = this.__s.fill; return typeof f === 'number' ? colorString(f) : f; }
  set fillStyle(v) { const c = this.__style(v); if (c !== undefined) this.__s.fill = c; }
  get strokeStyle() { const f = this.__s.stroke; return typeof f === 'number' ? colorString(f) : f; }
  set strokeStyle(v) { const c = this.__style(v); if (c !== undefined) this.__s.stroke = c; }
  __style(v) {
    if (v instanceof CanvasGradient || v instanceof CanvasPattern) return v;
    if (typeof v !== 'string' && !(v instanceof String)) return undefined;
    const c = rb.cvColor(String(v));
    return c === null ? undefined : c;
  }
  get globalAlpha() { return this.__s.alpha; }
  set globalAlpha(v) { v = +v; if (v >= 0 && v <= 1) this.__s.alpha = v; }
  get globalCompositeOperation() { return this.__s.op; }
  set globalCompositeOperation(v) { v = String(v); if (COMPOSITE_NAMES.has(v)) this.__s.op = v; }
  get lineWidth() { return this.__s.lw; }
  set lineWidth(v) { v = +v; if (v > 0 && Number.isFinite(v)) this.__s.lw = v; }
  get lineCap() { return CAPS[this.__s.cap]; }
  set lineCap(v) { const i = CAPS.indexOf(String(v)); if (i >= 0) this.__s.cap = i; }
  get lineJoin() { return JOINS[this.__s.join]; }
  set lineJoin(v) { const i = JOINS.indexOf(String(v)); if (i >= 0) this.__s.join = i; }
  get miterLimit() { return this.__s.miter; }
  set miterLimit(v) { v = +v; if (v > 0 && Number.isFinite(v)) this.__s.miter = v; }
  get lineDashOffset() { return this.__s.dashOff; }
  set lineDashOffset(v) { v = +v; if (Number.isFinite(v)) this.__s.dashOff = v; }
  setLineDash(a) {
    const v = Array.from(a || [], Number);
    if (v.some(x => !(x >= 0) || !Number.isFinite(x))) return;
    this.__s.dash = v.length % 2 ? v.concat(v) : v;
    this.__s.dashF = null;
  }
  getLineDash() { return this.__s.dash.slice(); }
  get imageSmoothingEnabled() { return this.__s.smooth; }
  set imageSmoothingEnabled(v) { this.__s.smooth = !!v; }
  get imageSmoothingQuality() { return this.__s.smoothQ; }
  set imageSmoothingQuality(v) { if (['low', 'medium', 'high'].includes(v)) this.__s.smoothQ = v; }
  get shadowBlur() { return this.__s.shadowBlur; }
  set shadowBlur(v) { v = +v; if (v >= 0 && Number.isFinite(v)) this.__s.shadowBlur = v; }
  get shadowColor() { return this.__s.shadowColor; }
  set shadowColor(v) { const c = rb.cvColor(String(v)); if (c !== null) this.__s.shadowColor = colorString(c); }
  get shadowOffsetX() { return this.__s.shadowX; }
  set shadowOffsetX(v) { v = +v; if (Number.isFinite(v)) this.__s.shadowX = v; }
  get shadowOffsetY() { return this.__s.shadowY; }
  set shadowOffsetY(v) { v = +v; if (Number.isFinite(v)) this.__s.shadowY = v; }
  get filter() { return this.__s.filter; }
  set filter(v) { this.__s.filter = String(v); }
  get font() { return this.__s.font; }
  set font(v) { v = String(v); const n = fontSize(v); if (n > 0) { this.__s.font = v; this.__s.size = n; } }
  get textAlign() { return this.__s.align; }
  set textAlign(v) { if (['start', 'end', 'left', 'right', 'center'].includes(v)) this.__s.align = v; }
  get textBaseline() { return this.__s.baseline; }
  set textBaseline(v) { if (['top', 'hanging', 'middle', 'alphabetic', 'ideographic', 'bottom'].includes(v)) this.__s.baseline = v; }
  get direction() { return this.__s.direction; }
  set direction(v) { if (['ltr', 'rtl', 'inherit'].includes(v)) this.__s.direction = v; }
  get letterSpacing() { return this.__s.letterSpacing; }
  set letterSpacing(v) { this.__s.letterSpacing = String(v); }
  get wordSpacing() { return this.__s.wordSpacing; }
  set wordSpacing(v) { this.__s.wordSpacing = String(v); }
  get fontKerning() { return this.__s.fontKerning; }
  set fontKerning(v) { this.__s.fontKerning = String(v); }
  get textRendering() { return this.__s.textRendering; }
  set textRendering(v) { this.__s.textRendering = String(v); }

  createLinearGradient(x0, y0, x1, y1) {
    const a = [+x0, +y0, 0, +x1, +y1, 0];
    if (!finite(...a)) throw new TypeError('createLinearGradient: non-finite argument');
    return new CanvasGradient(1, a);
  }
  createRadialGradient(x0, y0, r0, x1, y1, r1) {
    const a = [+x0, +y0, +r0, +x1, +y1, +r1];
    if (!finite(...a)) throw new TypeError('createRadialGradient: non-finite argument');
    if (r0 < 0 || r1 < 0) throw new DOMException('The radius provided is negative.', 'IndexSizeError');
    return new CanvasGradient(2, a);
  }
  createConicGradient(a, x, y) { return new CanvasGradient(3, [+x, +y, +a, 0, 0, 0]); }
  createPattern(img, rep) {
    if (img == null) throw new TypeError('createPattern: no image');
    return new CanvasPattern();
  }

  /* paths */
  beginPath() { this.__path.clear(); }
  __pathArgs(a, b) {
    /* (fillRule) | (path, fillRule) -> [PathData, matrix, rule] */
    if (a instanceof Path2D) return [a.__d, this.__s.m, b === 'evenodd' ? 1 : 0];
    return [this.__path, IDENTITY, a === 'evenodd' ? 1 : 0];
  }
  fill(a, b) {
    const [p, m, eo] = this.__pathArgs(a, b);
    if (!this.__c.__ready() || !p.n) return;
    this.__prep(this.__s.fill);
    rb.cvFill(this.__slot, p.d, p.n, m[0], m[1], m[2], m[3], m[4], m[5], eo);
  }
  stroke(a) {
    const [p, m] = this.__pathArgs(a);
    if (!this.__c.__ready() || !p.n) return;
    const s = this.__s;
    this.__prep(s.stroke);
    /* the current path is in device space already: its line width still
       scales with the transform of the stroke */
    const lm = p === this.__path ? s.m : m;
    const sc = Math.sqrt(Math.abs(lm[0] * lm[3] - lm[1] * lm[2]));
    if (p === this.__path) {
      rb.cvStroke(this.__slot, p.d, p.n, 1, 0, 0, 1, 0, 0, s.lw * sc, s.cap, s.join, s.miter,
                  this.__dashF() && new Float64Array(s.dash.map(v => v * sc)), s.dashOff * sc);
    } else {
      rb.cvStroke(this.__slot, p.d, p.n, m[0], m[1], m[2], m[3], m[4], m[5], s.lw, s.cap, s.join, s.miter,
                  this.__dashF(), s.dashOff);
    }
  }
  clip(a, b) {
    const [p, m] = this.__pathArgs(a, b);
    const r = p.n ? rb.cvBounds(p.d, p.n, m[0], m[1], m[2], m[3], m[4], m[5]) : null;
    let c = r ? [Math.max(0, Math.floor(r[0])), Math.max(0, Math.floor(r[1])), Math.ceil(r[2]), Math.ceil(r[3])] : [0, 0, 0, 0];
    const o = this.__s.clip;
    if (o) c = [Math.max(c[0], o[0]), Math.max(c[1], o[1]), Math.min(c[2], o[2]), Math.min(c[3], o[3])];
    if (c[2] < c[0]) c[2] = c[0];
    if (c[3] < c[1]) c[3] = c[1];
    this.__s.clip = c;
  }
  isPointInPath(a, b, c, d) {
    let p, m, x, y, rule;
    if (a instanceof Path2D) { p = a.__d; m = this.__s.m; x = +b; y = +c; rule = d; }
    else { p = this.__path; m = IDENTITY; x = +a; y = +b; rule = c; }
    if (!p.n || !finite(x, y)) return false;
    return rb.cvInside(p.d, p.n, m[0], m[1], m[2], m[3], m[4], m[5], x, y, rule === 'evenodd' ? 1 : 0);
  }
  isPointInStroke(a, b, c) {
    let p, m, x, y;
    if (a instanceof Path2D) { p = a.__d; m = this.__s.m; x = +b; y = +c; }
    else { p = this.__path; m = IDENTITY; x = +a; y = +b; }
    if (!p.n || !finite(x, y)) return false;
    const s = this.__s;
    const sc = p === this.__path ? Math.sqrt(Math.abs(s.m[0] * s.m[3] - s.m[1] * s.m[2])) : 1;
    return rb.cvInStroke(p.d, p.n, m[0], m[1], m[2], m[3], m[4], m[5], s.lw * sc, s.cap, s.join, s.miter,
                         null, 0, x, y);
  }

  /* rectangles: a reusable path, in user space under the transform */
  __rectPath(x, y, w, h) {
    const p = this.__rp || (this.__rp = new PathData(null));
    p.clear();
    p.move(x, y); p.line(x + w, y); p.line(x + w, y + h); p.line(x, y + h); p.close();
    return p;
  }
  fillRect(x, y, w, h) {
    x = +x; y = +y; w = +w; h = +h;
    if (!finite(x, y, w, h) || !w || !h || !this.__c.__ready()) return;
    const m = this.__s.m, p = this.__rectPath(x, y, w, h);
    this.__prep(this.__s.fill);
    rb.cvFill(this.__slot, p.d, p.n, m[0], m[1], m[2], m[3], m[4], m[5], 0);
  }
  strokeRect(x, y, w, h) {
    x = +x; y = +y; w = +w; h = +h;
    if (!finite(x, y, w, h) || !this.__c.__ready()) return;
    const s = this.__s, m = s.m, p = this.__rectPath(x, y, w, h);
    this.__prep(s.stroke);
    rb.cvStroke(this.__slot, p.d, p.n, m[0], m[1], m[2], m[3], m[4], m[5], s.lw, s.cap, s.join, s.miter,
                this.__dashF(), s.dashOff);
  }
  clearRect(x, y, w, h) {
    x = +x; y = +y; w = +w; h = +h;
    if (!finite(x, y, w, h) || !w || !h || !this.__c.__ready()) return;
    const s = this.__s, m = s.m, p = this.__rectPath(x, y, w, h), c = s.clip;
    rb.cvPaint(0);
    rb.cvComp(255, 4, 1, c ? c[0] : -1, c ? c[1] : -1, c ? c[2] : -1, c ? c[3] : -1);
    rb.cvFill(this.__slot, p.d, p.n, m[0], m[1], m[2], m[3], m[4], m[5], 0);
  }

  /* text: the 8x8 font, glyph cells size/2 wide and size tall */
  measureText(t) {
    t = String(t);
    const size = this.__s.size, w = codePoints(t) * size / 2;
    const asc = this.__ascent(), lead = this.__leadX(w);
    return {
      width: w, actualBoundingBoxLeft: -lead, actualBoundingBoxRight: w + lead,
      actualBoundingBoxAscent: asc, actualBoundingBoxDescent: size - asc,
      fontBoundingBoxAscent: size * 0.75, fontBoundingBoxDescent: size * 0.25,
      emHeightAscent: size * 0.75, emHeightDescent: size * 0.25,
      hangingBaseline: size * 0.65, alphabeticBaseline: 0, ideographicBaseline: -size * 0.25,
    };
  }
  /* how far the baseline sits below the top of the cell */
  __ascent() {
    const s = this.__s.size;
    switch (this.__s.baseline) {
      case 'top': return 0;
      case 'hanging': return s * 0.1;
      case 'middle': return s * 0.5;
      case 'ideographic': case 'bottom': return s;
      default: return s * 0.75;
    }
  }
  __leadX(w) {
    const a = this.__s.align;
    const rtl = this.__s.direction === 'rtl';
    if (a === 'center') return -w / 2;
    if (a === 'right' || (a === 'end' && !rtl) || (a === 'start' && rtl)) return -w;
    return 0;
  }
  __text(t, x, y, maxW, style) {
    t = String(t); x = +x; y = +y;
    if (!finite(x, y) || !this.__c.__ready() || !t) return;
    const s = this.__s, size = s.size;
    let w = codePoints(t) * size / 2, m = s.m;
    const lead = this.__leadX(w);
    let ox = x + lead;
    if (maxW !== undefined) {
      maxW = +maxW;
      if (!(maxW > 0)) return;
      if (w > maxW) {
        /* squeeze horizontally about the anchor */
        const k = maxW / w;
        m = matMul(m, [k, 0, 0, 1, x * (1 - k), 0]);
      }
    }
    this.__prep(style);
    rb.cvText(this.__slot, t, ox, y - this.__ascent(), size, m[0], m[1], m[2], m[3], m[4], m[5]);
  }
  fillText(t, x, y, maxW) { this.__text(t, x, y, maxW, this.__s.fill); }
  strokeText(t, x, y, maxW) { this.__text(t, x, y, maxW, this.__s.stroke); }

  /* images */
  drawImage(img, ...a) {
    if (!this.__c.__ready()) return;
    let kind, src;
    if (img instanceof HTMLImageElement) { kind = 0; src = img.__h; }
    else if (img instanceof HTMLCanvasElement) {
      const c = img.__ctx;
      if (!c || c.__slot < 0) return;
      kind = 1; src = c.__slot;
    } else if (img && img.__bitmap) { kind = img.__bitmap[0]; src = img.__bitmap[1]; }
    else throw new TypeError('drawImage: unsupported image source');
    const sz = rb.cvImageSize(kind, src);
    if (!sz) return;                      /* not loaded, broken: nothing to draw */
    let sx = 0, sy = 0, sw = sz[0], sh = sz[1], dx, dy, dw, dh;
    if (a.length === 2) { [dx, dy] = a; dw = sw; dh = sh; }
    else if (a.length === 4) { [dx, dy, dw, dh] = a; }
    else if (a.length === 8) { [sx, sy, sw, sh, dx, dy, dw, dh] = a; }
    else throw new TypeError('drawImage: wrong number of arguments');
    const v = [sx, sy, sw, sh, dx, dy, dw, dh].map(Number);
    if (!finite(...v)) return;
    const m = this.__s.m;
    this.__prep(0xFF000000);
    rb.cvImage(this.__slot, kind, src, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], m[0], m[1], m[2], m[3], m[4], m[5]);
  }

  /* pixels: device space, no transform, no compositing */
  createImageData(a, b) {
    if (a instanceof ImageData) return new ImageData(a.width, a.height);
    return new ImageData(Math.abs(a | 0), Math.abs(b | 0));
  }
  getImageData(x, y, w, h) {
    x = Math.floor(+x); y = Math.floor(+y); w = Math.trunc(+w); h = Math.trunc(+h);
    if (!w || !h) throw new DOMException('The source width is 0.', 'IndexSizeError');
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    const img = new ImageData(w, h);
    if (this.__c.__ready()) rb.cvGet(this.__slot, x, y, w, h, img.data);
    return img;
  }
  putImageData(img, dx, dy, x0, y0, w, h) {
    if (!(img instanceof ImageData)) throw new TypeError('putImageData: not an ImageData');
    if (!this.__c.__ready()) return;
    dx = Math.floor(+dx) || 0; dy = Math.floor(+dy) || 0;
    if (x0 === undefined) { x0 = 0; y0 = 0; w = img.width; h = img.height; }
    x0 = Math.floor(+x0) || 0; y0 = Math.floor(+y0) || 0; w = Math.floor(+w) || 0; h = Math.floor(+h) || 0;
    if (w < 0) { x0 += w; w = -w; }
    if (h < 0) { y0 += h; h = -h; }
    rb.cvPut(this.__slot, img.data, img.width, img.height, dx, dy, x0, y0, x0 + w, y0 + h);
  }
  drawFocusIfNeeded() {}
  scrollPathIntoView() {}
}
Object.assign(CanvasRenderingContext2D.prototype, PathMethods);

class HTMLCanvasElement extends HTMLElement {
  get width() { return rb.cvAttrSize(this.__h)[0]; }
  set width(v) { this.setAttribute('width', String(v >>> 0)); }
  get height() { return rb.cvAttrSize(this.__h)[1]; }
  set height(v) { this.setAttribute('height', String(v >>> 0)); }
  setAttribute(n, v) {
    HTMLElement.prototype.setAttribute.call(this, n, v);
    n = String(n).toLowerCase();
    if (n === 'width' || n === 'height') this.__clear(true);
  }
  removeAttribute(n) {
    HTMLElement.prototype.removeAttribute.call(this, n);
    n = String(n).toLowerCase();
    if (n === 'width' || n === 'height') this.__clear(true);
  }
  /* (re)size the pixels to the attributes, transparent black; setting the
     size also resets the context's state */
  __clear(resetState) {
    const c = this.__ctx;
    if (!c) return;
    if (c.__slot < 0) c.__slot = rb.cvOpen(this.__h);
    const s = rb.cvAttrSize(this.__h);
    if (c.__slot >= 0) rb.cvResize(c.__slot, s[0], s[1]);
    if (resetState) c.__reset();
  }
  /* the context can draw: its slot is still this element's (a page change
     or table pressure may have recycled it) */
  __ready() {
    const c = this.__ctx;
    if (!c) return false;
    const slot = rb.cvOpen(this.__h);
    if (slot !== c.__slot) {
      c.__slot = slot;
      if (slot >= 0) { const s = rb.cvAttrSize(this.__h); rb.cvResize(slot, s[0], s[1]); }
    }
    return slot >= 0;
  }
  getContext(type) {
    if (String(type) !== '2d') return null;
    if (!this.__ctx) {
      this.__ctx = new CanvasRenderingContext2D(this, -1);
      this.__clear(false);
    }
    return this.__ctx;
  }
  toDataURL() {
    const c = this.getContext('2d');
    if (!c.__c.__ready()) return 'data:,';
    return rb.cvPng(c.__slot) || 'data:,';
  }
  toBlob(cb) { setTimeout(() => cb(null), 0); }
  captureStream() { throw new DOMException('captureStream is not supported', 'NotSupportedError'); }
  transferControlToOffscreen() { throw new DOMException('OffscreenCanvas is not supported', 'NotSupportedError'); }
}

/* createImageBitmap(img|canvas): a handle drawImage accepts */
function createImageBitmap(src) {
  return new Promise((res, rej) => {
    const go = () => {
      let b;
      if (src instanceof HTMLImageElement) b = [0, src.__h];
      else if (src instanceof HTMLCanvasElement && src.__ctx) b = [1, src.__ctx.__slot];
      else { rej(new DOMException('createImageBitmap: unsupported source', 'InvalidStateError')); return; }
      const sz = rb.cvImageSize(b[0], b[1]);
      if (!sz) { rej(new DOMException('The source image could not be decoded.', 'InvalidStateError')); return; }
      res({__bitmap: b, width: sz[0], height: sz[1], close() {}});
    };
    if (src instanceof HTMLImageElement && !src.complete) src.decode().then(go, rej); else go();
  });
}

Object.assign(global, {HTMLCanvasElement, CanvasRenderingContext2D, Path2D, CanvasGradient, CanvasPattern,
  ImageData, DOMMatrix, createImageBitmap});

/* Custom elements: a defined class becomes the prototype of new wrappers
   for that tag (constructor/connectedCallback are not run). */
const customProtos = new Map();
function elementProto(tag) {
  return customProtos.get(tag) || (tag === 'img' ? HTMLImageElement.prototype
    : tag === 'canvas' ? HTMLCanvasElement.prototype : HTMLElement.prototype);
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
    inlineStale = true;
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
  if (inlineStale) bindInlineHandlers();
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
   each script so handlers created by markup in later scripts work too, and
   again at the end of a timer or event turn that added markup (inlineStale):
   a list rendered with innerHTML carries its buttons' onclick. */
const HANDLER_ATTRS = ['click', 'change', 'input', 'submit', 'keydown', 'keyup', 'keypress',
  'mousedown', 'mouseup', 'mouseover', 'mouseout', 'focus', 'blur', 'load', 'error'];
const HANDLER_SEL = HANDLER_ATTRS.map(t => '[on' + t + ']').join(',');
const handlerDone = new Set();
function bindInlineHandlers() {
  inlineStale = false;
  for (const h of rb.query(1, HANDLER_SEL)) {
    for (const t of HANDLER_ATTRS) {
      const code = rb.getAttr(h, 'on' + t);
      if (code === null) continue;
      const key = h + ':' + rb.gen(h) + ':' + t;
      if (handlerDone.has(key)) continue;
      handlerDone.add(key);
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
  if (inlineStale) bindInlineHandlers();
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
