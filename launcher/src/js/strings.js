// The interface's languages. `T` always holds the current language's texts (i18n/<code>.js); views read it when
// they are mounted, and main.js mounts them again when the language changes.

import en from "./i18n/en.js";
import es from "./i18n/es.js";
import ptBR from "./i18n/pt-BR.js";

export const LANGUAGES = [
  { code: "pt-BR", name: "Português (Brasil)", dict: ptBR },
  { code: "en", name: "English", dict: en },
  { code: "es", name: "Español", dict: es },
];

export const T = {};
let current = "";

export function setLanguage(code) {
  const lang = LANGUAGES.find((l) => l.code === code) ?? LANGUAGES[0];
  for (const k of Object.keys(T)) delete T[k];
  Object.assign(T, lang.dict);
  current = lang.code;
  if (typeof document !== "undefined") document.documentElement.lang = lang.code;
  return lang.code;
}

export const language = () => current;

/** The system's language, when it is one of ours (else English). */
export function detectLanguage(preferred = globalThis.navigator?.languages ?? [globalThis.navigator?.language ?? ""]) {
  for (const tag of preferred) {
    const t = String(tag).toLowerCase();
    if (t.startsWith("pt")) return "pt-BR";
    if (t.startsWith("es")) return "es";
    if (t.startsWith("en")) return "en";
  }
  return "en";
}

/** "Etapa {i} de {n}" + { i: 2, n: 8 } */
export function fmt(template, vars = {}) {
  return String(template).replace(/\{(\w+)\}/g, (m, k) => (k in vars ? String(vars[k]) : m));
}

setLanguage(detectLanguage());
