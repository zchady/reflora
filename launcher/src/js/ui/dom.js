// h("div.card#id", { attrs, on: { click } }, children...) -> Element. Strings become text nodes (never HTML).

export function h(spec, props, ...children) {
  if (props instanceof Node || typeof props === "string" || Array.isArray(props)) {
    children.unshift(props);
    props = {};
  }
  props = props || {};
  const [tagAndClasses, id] = spec.split("#");
  const [tag, ...classes] = tagAndClasses.split(".");
  const el = document.createElement(tag || "div");
  if (classes.length) el.className = classes.join(" ");
  if (id) el.id = id;
  for (const [k, v] of Object.entries(props)) {
    if (v === undefined || v === null || v === false) continue;
    if (k === "on") for (const [evt, fn] of Object.entries(v)) el.addEventListener(evt, fn);
    else if (k === "style") Object.assign(el.style, v);
    else if (k === "text") el.textContent = v;
    else el.setAttribute(k, v === true ? "" : v);
  }
  append(el, children);
  return el;
}

function append(el, children) {
  for (const c of children) {
    if (c === null || c === undefined || c === false) continue;
    if (Array.isArray(c)) append(el, c);
    else el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
}

export function setText(el, text) {
  if (el.textContent !== text) el.textContent = text;
}

export function setAttr(el, name, value) {
  if (value === null || value === undefined || value === false) el.removeAttribute(name);
  else if (el.getAttribute(name) !== String(value)) el.setAttribute(name, value === true ? "" : value);
}
