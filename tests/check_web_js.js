const fs = require("fs");
const path = require("path");

const htmlPath = path.join(__dirname, "..", "main", "web", "index.html");
const html = fs.readFileSync(htmlPath, "utf8");
const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/gi)];
if (!scripts.length) throw new Error("No inline script found in web/index.html");
for (const [index, match] of scripts.entries()) {
  new Function(match[1]);
  console.log(`SIMULATED PASS: inline script ${index + 1} parses`);
}

