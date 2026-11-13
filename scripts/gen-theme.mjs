#!/usr/bin/env node
// devdatapod P1: design token generator.
//   design/devconsole.pen (variables)  ->  src/ui/theme/generated/
//       tokens.json   consumed by C++ (Theme::color / Theme::font)
//       theme.qss     filled from src/ui/theme/theme.qss.in
//       Tokens.qml    QtObject instance exposed to QML as context property "Tokens"
//
// Nothing else may carry a literal color value (AGENTS.md hard rule 4 + docs/DESIGN-SPEC.md).
// Run:  node scripts/gen-theme.mjs [--check]

import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const penPath = join(repoRoot, 'design', 'devconsole.pen');
const templatePath = join(repoRoot, 'src', 'ui', 'theme', 'theme.qss.in');
const outDir = join(repoRoot, 'src', 'ui', 'theme', 'generated');

const CJK_FALLBACK = ['Noto Sans SC', 'Microsoft YaHei', 'PingFang SC'];

function fail(message) {
    console.error('gen-theme: ' + message);
    process.exit(1);
}

const pen = JSON.parse(readFileSync(penPath, 'utf8'));
const variables = pen.variables;
if (!variables) fail('no "variables" block in ' + penPath);

const colors = {};
const families = {};
for (const [name, entry] of Object.entries(variables)) {
    if (entry.type === 'color') colors[name] = entry.value.toUpperCase();
    else if (entry.type === 'string' && name.startsWith('font-')) families[name.slice(5)] = entry.value;
    else fail(`variable "${name}" has unsupported type "${entry.type}"`);
}

for (const required of ['surface', 'card', 'ink', 'ink-2', 'line', 'accent', 'sidebar']) {
    if (!(required in colors)) fail(`required color token "${required}" missing from .pen`);
}
for (const required of ['heading', 'body', 'cap']) {
    if (!(required in families)) fail(`required font token "font-${required}" missing from .pen`);
}

// MiSans is embedded in the binary and registered at startup, so it leads every stack.
// Funnel Sans / Inter / Geist have no CJK glyphs: without an explicit fallback chain
// Windows drops to YaHei and macOS to PingFang and the two stop matching.
const stacks = {};
for (const [key, family] of Object.entries(families)) {
    stacks[key] = ['MiSans', family, ...CJK_FALLBACK].map((f) => `"${f}"`).join(', ');
}

// rgba() needs the channel triplet split out; derived only, never a second colour source.
const rgb = {};
for (const [name, hex] of Object.entries(colors)) {
    const n = parseInt(hex.slice(1), 16);
    rgb[name] = [(n >> 16) & 255, (n >> 8) & 255, n & 255].join(', ');
}

const tokens = {
    source: 'design/devconsole.pen',
    generator: 'scripts/gen-theme.mjs',
    colors,
    rgb,
    families,
    fontStacks: stacks,
};

function substitute(text, origin) {
    const out = text.replace(/\{\{([a-zA-Z0-9_-]+(?:\.[a-zA-Z0-9_-]+)*)\}\}/g, (whole, path) => {
        const parts = path.split('.');
        let node = tokens;
        for (const part of parts) {
            if (!node || typeof node !== 'object' || !(part in node)) {
                fail(`unknown token placeholder ${whole} (from ${path}) in ${origin}`);
            }
            node = node[part];
        }
        // A path that stops at a group would stringify to "[object Object]".
        if (node !== null && typeof node === 'object') {
            fail(`token placeholder ${whole} in ${origin} resolves to a group, not a value`);
        }
        return String(node);
    });
    const leftover = out.match(/\{\{[^{}]*\}\}/);
    if (leftover) fail(`malformed token placeholder ${leftover[0]} in ${origin}`);
    return out;
}

const qmlLines = ['import QtQuick', '', 'QtObject {'];
for (const [name, value] of Object.entries(colors)) {
    qmlLines.push(`    readonly property color ${camel(name)}: "${value}"`);
}
for (const [key, stack] of Object.entries(stacks)) {
    qmlLines.push(`    readonly property string font${upperFirst(key)}Stack: ${JSON.stringify(stack)}`);
    qmlLines.push(`    readonly property string font${upperFirst(key)}Family: ${JSON.stringify(families[key])}`);
}
qmlLines.push('}');
const qmlText = qmlLines.join('\n') + '\n';

function camel(name) {
    return name.replace(/-([a-z0-9])/g, (m, ch) => ch.toUpperCase());
}
function upperFirst(text) {
    return text.charAt(0).toUpperCase() + text.slice(1);
}

if (!existsSync(outDir)) mkdirSync(outDir, { recursive: true });
const outputs = {
    'tokens.json': JSON.stringify(tokens, null, 2) + '\n',
    'theme.qss': substitute(readFileSync(templatePath, 'utf8'), 'src/ui/theme/theme.qss.in'),
    'Tokens.qml': qmlText,
};

let changed = false;
for (const [name, content] of Object.entries(outputs)) {
    const target = join(outDir, name);
    const previous = existsSync(target) ? readFileSync(target, 'utf8') : null;
    if (previous !== content) {
        if (process.argv.includes('--check')) {
            console.error('gen-theme: ' + name + ' is out of date, re-run without --check');
            changed = true;
            continue;
        }
        writeFileSync(target, content);
        console.log('gen-theme: wrote ' + name);
        changed = true;
    }
}
if (!changed) console.log('gen-theme: up to date');
