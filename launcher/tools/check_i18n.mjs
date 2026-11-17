// Every language file must have exactly the keys of pt-BR.js (same nesting, same array lengths), and the same
// {placeholders} in each text.  node launcher/tools/check_i18n.mjs
import { pathToFileURL } from "node:url";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const dir = join(dirname(fileURLToPath(import.meta.url)), "..", "src", "js", "i18n");
const load = async (n) => (await import(pathToFileURL(join(dir, n)).href)).default;
const ref = await load("pt-BR.js");
const problems = [];

function placeholders(s) {
  return [...String(s).matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort().join(",");
}

function compare(a, b, path, file) {
  if (typeof a !== typeof b || Array.isArray(a) !== Array.isArray(b)) {
    problems.push(`${file}: ${path} has a different type`);
    return;
  }
  if (Array.isArray(a)) {
    if (a.length !== b.length) problems.push(`${file}: ${path} has ${b.length} items, pt-BR ${a.length}`);
    a.forEach((x, i) => b[i] !== undefined && compare(x, b[i], `${path}[${i}]`, file));
  } else if (a && typeof a === "object") {
    for (const k of Object.keys(a)) {
      if (!(k in b)) problems.push(`${file}: missing ${path}.${k}`);
      else compare(a[k], b[k], `${path}.${k}`, file);
    }
    for (const k of Object.keys(b)) if (!(k in a)) problems.push(`${file}: extra ${path}.${k}`);
  } else if (typeof a === "string" && placeholders(a) !== placeholders(b)) {
    problems.push(`${file}: ${path} placeholders {${placeholders(b)}} instead of {${placeholders(a)}}`);
  }
}

for (const f of ["en.js", "es.js"]) compare(ref, await load(f), "T", f);
console.log(problems.length ? problems.join("\n") : "i18n: en and es match pt-BR");
process.exit(problems.length ? 1 : 0);
