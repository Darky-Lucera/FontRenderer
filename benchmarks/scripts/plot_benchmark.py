"""Turns the CSV files that benchmarkDraw writes with --csv into an HTML page with charts.

Usage:  python benchmarks/scripts/plot_benchmark.py bin/benchmark_a.csv [bin/benchmark_b.csv ...] [-o bin/benchmark.html]

Each CSV is one run, usually one per compiler or per processor. The page shows a heatmap per run, with the speed of every variant
against Baseline in every scenario, and the detail of one variant: its speed in each scenario, with every sample,
so the noise can be seen. It needs no server and no library: open it in a browser.

Only the standard library is used, so there is nothing to install.
"""

import argparse
import csv
import json
import pathlib
import sys

REQUIRED_COLUMNS = {
    'compiler', 'scenario', 'format', 'variant', 'median_us', 'min_us', 'speed_pct',
    'pixels_check', 'compared_with', 'different_pixels', 'different_levels', 'samples_us',
}


def read_rows(path):
    with open(path, newline='', encoding='utf-8') as file:
        reader  = csv.DictReader(file)
        missing = REQUIRED_COLUMNS - set(reader.fieldnames or [])
        if missing:
            sys.exit(f'{path}: missing columns {", ".join(sorted(missing))}. Was it written by benchmarkDraw --csv?')

        # Files written before benchmarkDraw printed the processor have no such column.
        return [{
            'compiler':   f"{row['compiler']}, {row['processor']}" if row.get('processor') else row['compiler'],
            'scenario':   row['scenario'],
            'format':     row['format'],
            'variant':    row['variant'],
            'median':     float(row['median_us']),
            'min':        float(row['min_us']),
            'speed':      float(row['speed_pct']),
            'check':      row['pixels_check'],
            'target':     row['compared_with'],
            'diffPixels': int(row['different_pixels']),
            'diffLevels': int(row['different_levels']),
            'samples':    [float(value) for value in row['samples_us'].split()],
        } for row in reader]


def main():
    parser = argparse.ArgumentParser(description='Charts for the CSV files of benchmarkDraw --csv.')
    parser.add_argument('csv', nargs='+', help='CSV files, one per run')
    parser.add_argument('-o', '--output', help='HTML file to write; by default, next to the first CSV')
    args = parser.parse_args()

    rows      = []
    compilers = {}
    for path in args.csv:
        run = read_rows(path)
        if not run:
            sys.exit(f'{path}: no rows')
        # Two runs of the same compiler would mix in one heatmap, so the name of the file tells them apart.
        label = run[0]['compiler']
        if label in compilers.values():
            label = f'{label} ({pathlib.Path(path).stem})'
        compilers[path] = label
        for row in run:
            row['compiler'] = label
        rows.extend(run)

    output = pathlib.Path(args.output) if args.output else pathlib.Path(args.csv[0]).with_suffix('.html')
    # json.dumps escapes neither < nor >, so a name with </script> would end the script early.
    data = json.dumps(rows).replace('<', '\\u003c')
    output.write_text(TEMPLATE.replace('__DATA__', data), encoding='utf-8')
    print(f'Wrote {output}')


TEMPLATE = r'''<!doctype html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Benchmark de DrawText</title>
<style>
:root {
    color-scheme: light;
    --page:           #f9f9f7;
    --surface:        #fcfcfb;
    --text-primary:   #0b0b0b;
    --text-secondary: #52514e;
    --muted:          #898781;
    --grid:           #e1e0d9;
    --axis:           #c3c2b7;
    --border:         rgba(11, 11, 11, 0.10);
    --series-1:       #8a36b8;
    --series-2:       #a88900;
    --series-3:       #199e70;
    --diverging-low:  #e34948;
    --diverging-mid:  #f0efec;
    --diverging-high: #2a78d6;
    --ink-slower:     #9c1c1c;
    --ink-faster:     #14479a;
    --noise:          rgba(137, 135, 129, 0.16);
}
@media (prefers-color-scheme: dark) {
    :root:where(:not([data-theme="light"])) {
        color-scheme: dark;
        --page:           #0d0d0d;
        --surface:        #1a1a19;
        --text-primary:   #ffffff;
        --text-secondary: #c3c2b7;
        --muted:          #898781;
        --grid:           #2c2c2a;
        --axis:           #383835;
        --border:         rgba(255, 255, 255, 0.10);
        --series-1:       #b266e0;
        --series-2:       #b09000;
        --series-3:       #199e70;
        --diverging-low:  #e66767;
        --diverging-mid:  #383835;
        --diverging-high: #3987e5;
        --ink-slower:     #ffa399;
        --ink-faster:     #9cc6ff;
        --noise:          rgba(137, 135, 129, 0.20);
    }
}
:root[data-theme="dark"] {
    color-scheme: dark;
    --page:           #0d0d0d;
    --surface:        #1a1a19;
    --text-primary:   #ffffff;
    --text-secondary: #c3c2b7;
    --muted:          #898781;
    --grid:           #2c2c2a;
    --axis:           #383835;
    --border:         rgba(255, 255, 255, 0.10);
    --series-1:       #b266e0;
    --series-2:       #b09000;
    --series-3:       #199e70;
    --diverging-low:  #e66767;
    --diverging-mid:  #383835;
    --diverging-high: #3987e5;
    --ink-slower:     #ffa399;
    --ink-faster:     #9cc6ff;
    --noise:          rgba(137, 135, 129, 0.20);
}
body {
    margin: 0;
    background: var(--page);
    color: var(--text-primary);
    font: 14px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif;
}
main {
    max-width: 1240px;
    margin: 0 auto;
    padding: 24px 16px 48px;
}
h1 {
    font-size: 22px;
    margin: 0 0 4px;
}
h2 {
    font-size: 17px;
    margin: 32px 0 8px;
}
h3 {
    font-size: 14px;
    margin: 20px 0 6px;
    color: var(--text-secondary);
    font-weight: 600;
}
p {
    margin: 4px 0 8px;
    color: var(--text-secondary);
    max-width: 90ch;
}
.card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 12px 16px 16px;
}
.scroll {
    overflow-x: auto;
}
.controls {
    display: flex;
    flex-wrap: wrap;
    gap: 16px;
    align-items: center;
    margin: 8px 0 12px;
    color: var(--text-secondary);
}
select {
    font: inherit;
    padding: 3px 6px;
    background: var(--surface);
    color: var(--text-primary);
    border: 1px solid var(--axis);
    border-radius: 4px;
}
.legend {
    display: flex;
    flex-wrap: wrap;
    gap: 16px;
    align-items: center;
    margin: 4px 0 8px;
    color: var(--text-secondary);
    font-size: 12px;
}
.swatch {
    display: inline-block;
    width: 10px;
    height: 10px;
    border-radius: 50%;
    margin-right: 6px;
    vertical-align: -1px;
}
svg text {
    font-family: system-ui, -apple-system, "Segoe UI", sans-serif;
}
.variant-label {
    cursor: pointer;
}
.variant-label:hover {
    text-decoration: underline;
}
#tooltip {
    position: fixed;
    pointer-events: none;
    z-index: 10;
    background: var(--surface);
    color: var(--text-primary);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 8px 10px;
    font-size: 12px;
    line-height: 1.5;
    box-shadow: 0 4px 16px rgba(0, 0, 0, 0.18);
    max-width: 340px;
    display: none;
}
#tooltip .secondary {
    color: var(--text-secondary);
}
table {
    border-collapse: collapse;
    font-size: 12px;
    margin-top: 8px;
    font-variant-numeric: tabular-nums;
}
th, td {
    padding: 3px 10px;
    border-bottom: 1px solid var(--grid);
    text-align: left;
    white-space: nowrap;
}
th {
    color: var(--text-secondary);
    font-weight: 600;
}
td.number {
    text-align: right;
}
.slower {
    color: var(--ink-slower);
}
.faster {
    color: var(--ink-faster);
}
details summary {
    cursor: pointer;
    color: var(--text-secondary);
    margin-top: 12px;
}
</style>
</head>
<body>
<main>
    <h1>Benchmark de DrawText</h1>
    <p>La velocidad se compara siempre con Baseline, dentro de la misma ejecución: «un 50 % más rápido» significa que hace 1.5 veces
       el trabajo en el mismo tiempo, y «un 20 % más lento», que hace el 80 %. Una diferencia menor del 4 % no es fiable.</p>

    <h2>Todas las variantes</h2>
    <p>Azul: más rápida que Baseline. Rojo: más lenta. Pasa el ratón por una celda para ver los números, o pulsa el nombre de una
       variante para ver su detalle.</p>
    <div class="controls">
        <label><input type="checkbox" id="show-values" checked> Mostrar el porcentaje en cada celda</label>
    </div>
    <div class="legend" id="heatmap-legend"></div>
    <div id="heatmaps"></div>

    <h2 id="detail-title">Detalle de una variante</h2>
    <p>Cada punto grande es la mediana; los puntos pequeños, las muestras. La banda gris es el ruido: ±4 % alrededor de Baseline.</p>
    <div class="controls">
        <label>Variante <select id="variant"></select></label>
    </div>
    <div class="card">
        <div class="legend" id="detail-legend"></div>
        <div id="detail"></div>
        <details>
            <summary>Tabla con los números</summary>
            <div class="scroll" id="detail-table"></div>
        </details>
    </div>
</main>
<div id="tooltip"></div>
<script>
const DATA = __DATA__;

const NOISE_PCT    = 4;
// The color of a cell saturates at 2.5 times faster or slower, so SIMD does not wash out the scalar variants.
const SATURATION   = 2.5;
const SVG_NS       = 'http://www.w3.org/2000/svg';

const compilers = unique(DATA.map(row => row.compiler));
const variants  = unique(DATA.map(row => row.variant));
const scenarios = unique(DATA.map(row => row.scenario));
const rowByKey  = new Map(DATA.map(row => [makeKey(row.compiler, row.scenario, row.variant), row]));

function unique(values) {
    return [...new Set(values)];
}

function makeKey(compiler, scenario, variant) {
    return compiler + '\u0001' + scenario + '\u0001' + variant;
}

function findRow(compiler, scenario, variant) {
    return rowByKey.get(makeKey(compiler, scenario, variant));
}

// "Roboto 14 px, rotation, opaque" -> group "Roboto 14 px" and label "rot. · opaco".
function splitScenario(name) {
    const parts = name.split(',').map(part => part.trim());
    const words = {
        'rotation':    'rot.',
        'no rotation': 'sin rot.',
        'opaque':      'opaco',
        'translucent': 'transl.',
    };
    return {
        group: parts[0],
        label: parts.slice(1).map(part => words[part] || part).join(' · '),
    };
}

function cssVar(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

function formatNumber(value, digits) {
    return value.toLocaleString('es-ES', { minimumFractionDigits: digits, maximumFractionDigits: digits });
}

function speedText(speed) {
    if (Math.abs(speed) < 0.05) {
        return 'igual que Baseline';
    }
    return speed > 0 ? `un ${formatNumber(speed, 1)} % más rápido que Baseline`
                     : `un ${formatNumber(-speed, 1)} % más lento que Baseline`;
}

function speedClass(speed) {
    if (Math.abs(speed) < 0.05) {
        return '';
    }
    return speed > 0 ? 'faster' : 'slower';
}

function shortSpeed(speed) {
    const rounded = Math.round(speed);
    return rounded > 0 ? `+${rounded}` : rounded < 0 ? `−${-rounded}` : '0';
}

function pixelsText(row) {
    const target = row.target === 'Reference' ? 'DrawText' : row.target;
    if (row.check === 'same') {
        return `igual que ${target}`;
    }
    return `distinto de ${target}: ${row.diffPixels} píxeles, hasta ${row.diffLevels} niveles`;
}

function paintsLikeDrawText(row) {
    return row.check === 'same' && row.target === 'Reference';
}

// The speed of one sample, against the median of Baseline in the same run and scenario.
function sampleSpeed(row, sample) {
    const baseline = findRow(row.compiler, row.scenario, 'Baseline');
    return baseline ? (baseline.median / sample - 1) * 100 : NaN;
}

//---------------------------------------------------------------------------------------------------------------------
// Colors are interpolated in OKLab, so the steps look even to the eye.

function hexToRgb(hex) {
    const value = parseInt(hex.replace('#', ''), 16);
    return [(value >> 16) & 255, (value >> 8) & 255, value & 255].map(channel => channel / 255);
}

function toLinear(channel) {
    return channel <= 0.04045 ? channel / 12.92 : Math.pow((channel + 0.055) / 1.055, 2.4);
}

function fromLinear(channel) {
    return channel <= 0.0031308 ? channel * 12.92 : 1.055 * Math.pow(channel, 1 / 2.4) - 0.055;
}

function rgbToOklab(rgb) {
    const [r, g, b] = rgb.map(toLinear);
    const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return [
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
    ];
}

function oklabToRgb(lab) {
    const [L, a, b] = lab;
    const l = Math.pow(L + 0.3963377774 * a + 0.2158037573 * b, 3);
    const m = Math.pow(L - 0.1055613458 * a - 0.0638541728 * b, 3);
    const s = Math.pow(L - 0.0894841775 * a - 1.2914855480 * b, 3);
    return [
         4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
    ].map(channel => Math.min(1, Math.max(0, fromLinear(channel))));
}

function mixOklab(fromHex, toHex, t) {
    const from = rgbToOklab(hexToRgb(fromHex));
    const to   = rgbToOklab(hexToRgb(toHex));
    const lab  = from.map((value, i) => value + (to[i] - value) * t);
    return { lightness: lab[0], css: 'rgb(' + oklabToRgb(lab).map(channel => Math.round(channel * 255)).join(',') + ')' };
}

// Faster is blue and slower is red. The scale is logarithmic, so twice as fast and half as fast get the same strength.
// A difference inside the noise stays gray, and the curve lifts small differences, which are the usual ones.
function cellColor(speed) {
    const ratio     = 1 + speed / 100;
    const t         = Math.log(Math.max(ratio, 1e-6)) / Math.log(SATURATION);
    const noise     = Math.log(1 + NOISE_PCT / 100) / Math.log(SATURATION);
    const magnitude = Math.max(0, Math.min(1, (Math.abs(t) - noise) / (1 - noise)));
    const pole      = cssVar(t >= 0 ? '--diverging-high' : '--diverging-low');
    const strength  = Math.pow(magnitude, 0.7);
    return { ...mixOklab(cssVar('--diverging-mid'), pole, strength), strength };
}

// The number of a cell is tinted with its sign, so that the sign also shows in the gray cells inside the noise.
// The tint fades to plain ink as the cell takes its own color, where a tinted number would be hard to read.
const CELL_INK = {
    onLight: { plain: '#0b0b0b', slower: '#9c1c1c', faster: '#14479a' },
    onDark:  { plain: '#ffffff', slower: '#ffa399', faster: '#9cc6ff' },
};

function cellInk(speed, cell) {
    const ink     = cell.lightness < 0.62 ? CELL_INK.onDark : CELL_INK.onLight;
    const rounded = Math.round(speed);
    if (rounded === 0) {
        return ink.plain;
    }
    return mixOklab(rounded > 0 ? ink.faster : ink.slower, ink.plain, cell.strength).css;
}

//---------------------------------------------------------------------------------------------------------------------

function svgElement(tag, attributes, parent) {
    const element = document.createElementNS(SVG_NS, tag);
    for (const [name, value] of Object.entries(attributes)) {
        element.setAttribute(name, value);
    }
    if (parent) {
        parent.appendChild(element);
    }
    return element;
}

function svgText(text, attributes, parent) {
    const element = svgElement('text', attributes, parent);
    element.textContent = text;
    return element;
}

const tooltip = document.getElementById('tooltip');

function showTooltip(event, html) {
    tooltip.innerHTML = html;
    tooltip.style.display = 'block';
    const margin = 14;
    const box    = tooltip.getBoundingClientRect();
    let x = event.clientX + margin;
    let y = event.clientY + margin;
    if (x + box.width > window.innerWidth - 8) {
        x = event.clientX - box.width - margin;
    }
    if (y + box.height > window.innerHeight - 8) {
        y = event.clientY - box.height - margin;
    }
    tooltip.style.left = `${Math.max(8, x)}px`;
    tooltip.style.top  = `${Math.max(8, y)}px`;
}

function hideTooltip() {
    tooltip.style.display = 'none';
}

function attachTooltip(element, html) {
    element.addEventListener('mousemove', event => showTooltip(event, html));
    element.addEventListener('mouseleave', hideTooltip);
}

function rowTooltip(row) {
    return `<b>${row.variant}</b><br>` +
           `<span class="secondary">${row.scenario} · ${row.compiler}</span><br>` +
           `Mediana ${formatNumber(row.median, 1)} µs, mínimo ${formatNumber(row.min, 1)} µs<br>` +
           `<span class="${speedClass(row.speed)}">${speedText(row.speed)}</span><br>` +
           `<span class="secondary">Píxeles: ${pixelsText(row)}</span>`;
}

// Groups of consecutive scenarios with the same font and size, for the headers.
function scenarioGroups() {
    const groups = [];
    scenarios.forEach((scenario, index) => {
        const group = splitScenario(scenario).group;
        if (groups.length === 0 || groups[groups.length - 1].name !== group) {
            groups.push({ name: group, first: index, count: 0 });
        }
        groups[groups.length - 1].count++;
    });
    return groups;
}

//---------------------------------------------------------------------------------------------------------------------
// Heatmaps

function renderHeatmapLegend() {
    const legend = document.getElementById('heatmap-legend');
    legend.innerHTML = '';
    const width = 380, height = 34, margin = 24;
    const svg   = svgElement('svg', { width, height: height + 4, role: 'img', 'aria-label': 'Escala de color' }, legend);
    const steps = 64;
    const xOf   = t => margin + ((t + 1) / 2) * (width - 2 * margin);
    for (let i = 0; i < steps; i++) {
        const t     = -1 + (2 * (i + 0.5)) / steps;
        const speed = (Math.pow(SATURATION, t) - 1) * 100;
        svgElement('rect', { x: xOf(-1 + (2 * i) / steps), y: 2, width: (width - 2 * margin) / steps + 0.5, height: 10, fill: cellColor(speed).css }, svg);
    }
    for (const speed of [-50, -25, 0, 50, 100, 150]) {
        const x = xOf(Math.log(1 + speed / 100) / Math.log(SATURATION));
        svgElement('line', { x1: x, x2: x, y1: 12, y2: 16, stroke: cssVar('--axis') }, svg);
        svgText(speed === 0 ? '0 %' : `${shortSpeed(speed)} %`, { x, y: 28, 'text-anchor': 'middle', 'font-size': 10, fill: cssVar('--text-secondary') }, svg);
    }

    const note = document.createElement('span');
    note.textContent = 'Nombre en gris y cursiva, con ≠: en algún escenario no pinta exactamente igual que DrawText, o solo se ha comparado con otra variante';
    legend.appendChild(note);
}

// "Roboto 14 px" -> font "Roboto" and size "14 px".
function splitGroup(name) {
    const space = name.indexOf(' ');
    return space < 0 ? { font: name, size: '' } : { font: name.slice(0, space), size: name.slice(space + 1) };
}

function renderHeatmap(compiler, parent, showValues) {
    const groups = scenarioGroups();
    // With few scenarios per group, as with --scenario, the columns widen until the size fits above them.
    const CHARACTER_WIDTH = 6.5;
    const fitWidth = Math.max(...groups.map(group => Math.ceil((splitGroup(group.name).size.length * CHARACTER_WIDTH + 8) / group.count)));
    const LABEL_WIDTH = 210, CELL_WIDTH = Math.max(showValues ? 40 : 30, fitWidth), CELL_HEIGHT = 20, HEADER_HEIGHT = 126, GAP = 2;
    const width  = LABEL_WIDTH + scenarios.length * CELL_WIDTH + 8;
    const height = HEADER_HEIGHT + variants.length * CELL_HEIGHT + 4;

    const card = document.createElement('div');
    card.className = 'card scroll';
    card.style.marginBottom = '16px';
    const title = document.createElement('h3');
    title.textContent = compiler;
    title.style.marginTop = '0';
    card.appendChild(title);
    parent.appendChild(card);

    const svg = svgElement('svg', { width, height, role: 'img', 'aria-label': `Velocidad frente a Baseline, ${compiler}` }, card);
    const secondary = cssVar('--text-secondary');

    let previousFont = null;
    for (const group of groups) {
        const x0    = LABEL_WIDTH + group.first * CELL_WIDTH;
        const x1    = x0 + group.count * CELL_WIDTH - GAP;
        const parts = splitGroup(group.name);
        if (parts.font !== previousFont) {
            svgText(parts.font, { x: x0 + 2, y: 12, 'font-size': 11, 'font-weight': 600, fill: secondary }, svg);
            previousFont = parts.font;
        }
        svgText(parts.size, { x: (x0 + x1) / 2, y: 27, 'text-anchor': 'middle', 'font-size': 11, fill: secondary }, svg);
        svgElement('line', { x1: x0 + 2, x2: x1 - 2, y1: 33, y2: 33, stroke: cssVar('--axis') }, svg);
    }
    scenarios.forEach((scenario, column) => {
        const x = LABEL_WIDTH + column * CELL_WIDTH + (CELL_WIDTH - GAP) / 2 + 4;
        svgText(splitScenario(scenario).label, { x, y: HEADER_HEIGHT - 6, transform: `rotate(-90 ${x} ${HEADER_HEIGHT - 6})`, 'font-size': 11, fill: secondary }, svg);
    });

    variants.forEach((variant, line) => {
        const y     = HEADER_HEIGHT + line * CELL_HEIGHT;
        const label = svgText(variant, {
            x: LABEL_WIDTH - 10, y: y + CELL_HEIGHT / 2 + 4, 'text-anchor': 'end', 'font-size': 12,
            'font-weight': variant === 'Baseline' ? 700 : 400, fill: cssVar('--text-primary'), class: 'variant-label',
        }, svg);
        label.addEventListener('click', () => selectVariant(variant, true));
        const rowsOfVariant = scenarios.map(scenario => findRow(compiler, scenario, variant)).filter(row => row);
        if (rowsOfVariant.some(row => !paintsLikeDrawText(row))) {
            svgText('≠', { x: LABEL_WIDTH - 6, y: y + CELL_HEIGHT / 2 + 4, 'text-anchor': 'end', 'font-size': 11, fill: cssVar('--muted') }, svg);
            label.setAttribute('x', LABEL_WIDTH - 18);
            label.setAttribute('fill', cssVar('--muted'));
            label.setAttribute('font-style', 'italic');
        }

        scenarios.forEach((scenario, column) => {
            const row = findRow(compiler, scenario, variant);
            if (!row) {
                return;
            }
            const x     = LABEL_WIDTH + column * CELL_WIDTH;
            const color = cellColor(row.speed);
            const cell  = svgElement('g', {}, svg);
            svgElement('rect', { x, y, width: CELL_WIDTH - GAP, height: CELL_HEIGHT - GAP, rx: 2, fill: color.css }, cell);
            if (showValues) {
                svgText(shortSpeed(row.speed), { x: x + (CELL_WIDTH - GAP) / 2, y: y + CELL_HEIGHT / 2 + 3, 'text-anchor': 'middle', 'font-size': 10, fill: cellInk(row.speed, color) }, cell);
            }
            attachTooltip(cell, rowTooltip(row));
        });
    });
}

function renderHeatmaps() {
    const parent = document.getElementById('heatmaps');
    parent.innerHTML = '';
    const showValues = document.getElementById('show-values').checked;
    for (const compiler of compilers) {
        renderHeatmap(compiler, parent, showValues);
    }
    renderHeatmapLegend();
}

//---------------------------------------------------------------------------------------------------------------------
// Detail of one variant

function seriesColor(index) {
    return cssVar(`--series-${Math.min(index, 2) + 1}`);
}

function niceTicks(minRatio, maxRatio) {
    const candidates = [-90, -75, -50, -25, -10, 0, 10, 25, 50, 100, 150, 200, 300, 400, 600, 900];
    return candidates.filter(speed => {
        const ratio = 1 + speed / 100;
        return ratio >= minRatio && ratio <= maxRatio;
    });
}

function renderDetail(variant) {
    const container = document.getElementById('detail');
    container.innerHTML = '';
    document.getElementById('detail-title').textContent = `Detalle de ${variant}`;

    const legend = document.getElementById('detail-legend');
    legend.innerHTML = '';
    compilers.forEach((compiler, index) => {
        const item = document.createElement('span');
        item.innerHTML = `<span class="swatch" style="background:${seriesColor(index)}"></span>${compiler}`;
        legend.appendChild(item);
    });

    const rows = DATA.filter(row => row.variant === variant);
    const ratios = [1 / (1 + NOISE_PCT / 100), 1 + NOISE_PCT / 100];
    for (const row of rows) {
        ratios.push(1 + row.speed / 100);
        for (const sample of row.samples) {
            ratios.push(1 + sampleSpeed(row, sample) / 100);
        }
    }
    const logs   = ratios.filter(ratio => ratio > 0 && isFinite(ratio)).map(Math.log);
    const span   = Math.max(...logs) - Math.min(...logs);
    const logMin = Math.min(...logs) - span * 0.05;
    const logMax = Math.max(...logs) + span * 0.05;

    const LABEL_WIDTH = 250, ROW_HEIGHT = 26, TOP = 28, BOTTOM = 30;
    const width  = Math.max(560, container.clientWidth);
    const plotX0 = LABEL_WIDTH, plotX1 = width - 16;
    const height = TOP + scenarios.length * ROW_HEIGHT + BOTTOM;
    const xOf    = speed => plotX0 + ((Math.log(1 + speed / 100) - logMin) / (logMax - logMin)) * (plotX1 - plotX0);

    const svg = svgElement('svg', { width, height, role: 'img', 'aria-label': `Velocidad de ${variant} frente a Baseline` }, container);
    const secondary = cssVar('--text-secondary');
    const muted     = cssVar('--muted');

    const noiseLeft  = xOf((1 / (1 + NOISE_PCT / 100) - 1) * 100);
    const noiseRight = xOf(NOISE_PCT);
    svgElement('rect', { x: noiseLeft, y: TOP - 6, width: noiseRight - noiseLeft, height: scenarios.length * ROW_HEIGHT + 6, fill: cssVar('--noise') }, svg);
    svgText('ruido', { x: (noiseLeft + noiseRight) / 2, y: TOP - 10, 'text-anchor': 'middle', 'font-size': 10, fill: muted }, svg);

    for (const speed of niceTicks(Math.exp(logMin), Math.exp(logMax))) {
        const x = xOf(speed);
        svgElement('line', { x1: x, x2: x, y1: TOP - 4, y2: TOP + scenarios.length * ROW_HEIGHT, stroke: speed === 0 ? cssVar('--axis') : cssVar('--grid'), 'stroke-width': 1 }, svg);
        svgText(speed === 0 ? 'Baseline' : `${shortSpeed(speed)} %`, { x, y: height - 10, 'text-anchor': 'middle', 'font-size': 11, fill: secondary }, svg);
    }

    let previousGroup = null;
    scenarios.forEach((scenario, line) => {
        const y     = TOP + line * ROW_HEIGHT + ROW_HEIGHT / 2;
        const parts = splitScenario(scenario);
        if (parts.group !== previousGroup) {
            if (line > 0) {
                svgElement('line', { x1: 8, x2: plotX1, y1: y - ROW_HEIGHT / 2, y2: y - ROW_HEIGHT / 2, stroke: cssVar('--grid') }, svg);
            }
            svgText(parts.group, { x: 8, y: y + 4, 'font-size': 12, 'font-weight': 600, fill: cssVar('--text-primary') }, svg);
            previousGroup = parts.group;
        }
        svgText(parts.label, { x: LABEL_WIDTH - 12, y: y + 4, 'text-anchor': 'end', 'font-size': 12, fill: secondary }, svg);

        compilers.forEach((compiler, index) => {
            const row = findRow(compiler, scenario, variant);
            if (!row) {
                return;
            }
            const color = seriesColor(index);
            const dy    = (index - (compilers.length - 1) / 2) * 8;
            for (const sample of row.samples) {
                svgElement('circle', { cx: xOf(sampleSpeed(row, sample)), cy: y + dy, r: 2.5, fill: color, opacity: 0.35 }, svg);
            }
            const marker = svgElement('g', {}, svg);
            svgElement('circle', { cx: xOf(row.speed), cy: y + dy, r: 5, fill: color, stroke: cssVar('--surface'), 'stroke-width': 2 }, marker);
            svgElement('circle', { cx: xOf(row.speed), cy: y + dy, r: 11, fill: 'transparent' }, marker);
            attachTooltip(marker, rowTooltip(row));
        });
    });

    renderDetailTable(rows);
}

function renderDetailTable(rows) {
    const table = document.createElement('table');
    table.innerHTML = '<thead><tr><th>Escenario</th><th>Compilador</th><th>Mediana (µs)</th><th>Mínimo (µs)</th>' +
                      '<th>Frente a Baseline</th><th>Píxeles</th></tr></thead>';
    const body = document.createElement('tbody');
    for (const scenario of scenarios) {
        for (const compiler of compilers) {
            const row = findRow(compiler, scenario, rows[0] ? rows[0].variant : '');
            if (!row) {
                continue;
            }
            const line = document.createElement('tr');
            line.innerHTML = `<td>${scenario}</td><td>${compiler}</td><td class="number">${formatNumber(row.median, 1)}</td>` +
                             `<td class="number">${formatNumber(row.min, 1)}</td><td class="${speedClass(row.speed)}">${speedText(row.speed)}</td><td>${pixelsText(row)}</td>`;
            body.appendChild(line);
        }
    }
    table.appendChild(body);
    const parent = document.getElementById('detail-table');
    parent.innerHTML = '';
    parent.appendChild(table);
}

//---------------------------------------------------------------------------------------------------------------------

const variantSelect = document.getElementById('variant');
for (const variant of variants) {
    const option = document.createElement('option');
    option.value = option.textContent = variant;
    variantSelect.appendChild(option);
}

function selectVariant(variant, scroll) {
    variantSelect.value = variant;
    renderDetail(variant);
    history.replaceState(null, '', `#${encodeURIComponent(variant)}`);
    if (scroll) {
        document.getElementById('detail-title').scrollIntoView({ behavior: 'smooth' });
    }
}

function initialVariant() {
    const fromHash = decodeURIComponent(location.hash.slice(1));
    if (variants.includes(fromHash)) {
        return fromHash;
    }
    return variants.find(variant => variant !== 'Reference' && variant !== 'Baseline') || variants[0];
}

function renderAll() {
    renderHeatmaps();
    renderDetail(variantSelect.value);
}

variantSelect.addEventListener('change', () => selectVariant(variantSelect.value, false));
document.getElementById('show-values').addEventListener('change', renderHeatmaps);
window.addEventListener('resize', () => renderDetail(variantSelect.value));
window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', renderAll);

variantSelect.value = initialVariant();
renderAll();
</script>
</body>
</html>
'''


if __name__ == '__main__':
    main()
