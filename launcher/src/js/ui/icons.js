// The launcher's icons: simple 24x24 line drawings made for REFLORA. "sparkle" is the four-point star of the logo.

const NS = "http://www.w3.org/2000/svg";

const PATHS = {
  home: ["M3.5 10.5 12 3.5l8.5 7", "M5.5 9v10a1.5 1.5 0 0 0 1.5 1.5h3.5v-6h3v6H17a1.5 1.5 0 0 0 1.5-1.5V9"],
  download: ["M12 3.5v11", "M7.5 10 12 14.5 16.5 10", "M4.5 16.5v2a2 2 0 0 0 2 2h11a2 2 0 0 0 2-2v-2"],
  info: ["M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z", "M12 11v5.5", "M12 7.6v.1"],
  refresh: ["M20 11a8 8 0 0 0-14.6-4.4L4 8.5", "M4 4v4.5h4.5", "M4 13a8 8 0 0 0 14.6 4.4l1.4-1.9", "M20 20v-4.5h-4.5"],
  check: ["M5 12.5 10 17.5 19.5 7"],
  close: ["M6 6l12 12", "M18 6 6 18"],
  minimize: ["M5 12h14"],
  maximize: ["M6 6h12v12H6z"],
  package: ["M12 3 20 7.5v9L12 21l-8-4.5v-9z", "M4 7.5 12 12l8-4.5", "M12 12v9"],
  folder: ["M3.5 7.5a2 2 0 0 1 2-2h4l2 2.5h7a2 2 0 0 1 2 2v7.5a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z"],
  list: ["M8.5 7h11", "M8.5 12h11", "M8.5 17h11", "M4.5 7h.1", "M4.5 12h.1", "M4.5 17h.1"],
  cpu: ["M7 7h10v10H7z", "M10 3.5v3.5", "M14 3.5v3.5", "M10 17v3.5", "M14 17v3.5", "M3.5 10H7", "M3.5 14H7", "M17 10h3.5", "M17 14h3.5"],
  clock: ["M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z", "M12 7v5l3.5 2"],
  stop: ["M7 7h10v10H7z"],
  flask: ["M9.5 3.5h5", "M10.5 3.5v6L5 19a1.3 1.3 0 0 0 1.1 2h11.8a1.3 1.3 0 0 0 1.1-2l-5.5-9.5v-6", "M7.5 15h9"],
  sliders: ["M4 7h9", "M17 7h3", "M15 5v4", "M4 17h3", "M11 17h9", "M9 15v4"],
  user: ["M12 12a4 4 0 1 0 0-8 4 4 0 0 0 0 8z", "M4.5 20.5c1-3.6 4-5.5 7.5-5.5s6.5 1.9 7.5 5.5"],
  globe: ["M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z", "M3.5 12h17", "M12 3c2.5 2.5 3.6 5.6 3.6 9s-1.1 6.5-3.6 9c-2.5-2.5-3.6-5.6-3.6-9S9.5 5.5 12 3z"],
  save: ["M5.5 3.5h10l3 3v12a2 2 0 0 1-2 2h-11a2 2 0 0 1-2-2v-13a2 2 0 0 1 2-2z", "M8 3.5v5h7v-5", "M8 20.5v-6h8v6"],
  shield: ["M12 3 19.5 6v6c0 4.5-3.2 7.6-7.5 9-4.3-1.4-7.5-4.5-7.5-9V6z", "M9 12l2.2 2.2L15.5 10"],
};

const FILLED = {
  play: "M8.5 5.2v13.6a1.2 1.2 0 0 0 1.8 1l11-6.8a1.2 1.2 0 0 0 0-2L10.3 4.2a1.2 1.2 0 0 0-1.8 1z",
  sparkle: "M12 2.5c.8 4.9 4.6 8.7 9.5 9.5-4.9.8-8.7 4.6-9.5 9.5-.8-4.9-4.6-8.7-9.5-9.5 4.9-.8 8.7-4.6 9.5-9.5z",
};

export function icon(name, extraClass = "") {
  const svg = document.createElementNS(NS, "svg");
  svg.setAttribute("viewBox", "0 0 24 24");
  svg.setAttribute("aria-hidden", "true");
  svg.setAttribute("class", `icon ${extraClass}`.trim());
  if (name === "spinner") {
    svg.classList.add("spinning");
    const c = document.createElementNS(NS, "path");
    c.setAttribute("d", "M12 3.5a8.5 8.5 0 1 1-8.5 8.5");
    svg.append(c);
    return svg;
  }
  if (FILLED[name]) {
    svg.classList.add("filled");
    const p = document.createElementNS(NS, "path");
    p.setAttribute("d", FILLED[name]);
    svg.append(p);
    return svg;
  }
  for (const d of PATHS[name] ?? PATHS.info) {
    const p = document.createElementNS(NS, "path");
    p.setAttribute("d", d);
    svg.append(p);
  }
  return svg;
}
