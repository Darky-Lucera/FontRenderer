"""Builds an HTML page with the score of each technique of the benchmark on every machine, from the results of the
portable benchmark.

Usage:  python benchmarks/scripts/score_benchmark.py results_folder [-o score.html]

The CSV files must be named as the portable benchmark names them:
benchmark_<system>_<compiler>_<architecture>_<processor>[_scalar]_<run>.csv

A technique is a variant compared with its base, such as Sse2OverlapWidths against Sse2Overlap. Its score in a set of
scenarios is the geometric mean of the speed ratios of those scenarios, with the runs of each machine combined in the
same way.
"""

import argparse
import csv
import json
import math
import pathlib
import re
import sys

# (id, variant, base, kind of file, description)
TECHNIQUES = [
    ('widths-sse2',     'Sse2OverlapWidths',          'Sse2Overlap',    'simd',   'Casos de ancho en los glifos sin girar, con SSE2'),
    ('bandwidths-sse2', 'Sse2OverlapBandWidths',      'Sse2Overlap',    'simd',   'Casos de ancho en las bandas de los glifos girados en el atlas, con SSE2'),
    ('v2',              'X64v2',                      'Sse2Overlap',    'simd',   'Instrucciones de x86-64-v2 frente al nivel x86-64 por defecto, que ya tiene SSE y SSE2'),
    ('widths-v2',       'X64v2Widths',                'X64v2',          'simd',   'Casos de ancho en los glifos sin girar, con x86-64-v2'),
    ('bandwidths-v2',   'X64v2BandWidths',            'X64v2',          'simd',   'Casos de ancho en las bandas de los glifos girados en el atlas, con x86-64-v2'),
    ('noise',           'Sse2Overlap',                'Baseline',       'simd',   'Sse2Overlap frente a la librería: es el mismo código, así que mide el ruido'),
    ('simd',            'Baseline',                   'BaselineScalar', 'scalar', 'El código SIMD de la librería frente a su código escalar'),
    ('old-loop',        'RoundOnceSwarPremultiplied', 'BaselineScalar', 'scalar', 'El bucle escalar antiguo, sin una función por caso ni el atajo del texel 255'),
    ('uint64',          'RoundOnce64Premultiplied',   'BaselineScalar', 'scalar', 'Dos canales por uint64_t en vez de por uint32_t, en el escalar'),
    ('truncate',        'Truncate',                   'BaselineScalar', 'scalar', 'El Baseline antiguo, que trunca: pinta otros píxeles'),
]

FILE_NAME = re.compile(r'^benchmark_([^_]+)_([^_]+)_([^_]+)_(.+?)(_scalar)?_(\d+)$')
SCENARIO  = re.compile(r'^(\w+) (\d+) px[^,]*, (rotation|no rotation), (opaque|translucent)$')


def read_medians(path):
    with open(path, newline='', encoding='utf-8') as file:
        rows = list(csv.DictReader(file))
    if not rows:
        sys.exit(f'{path}: no rows')
    # Older results call the variants of x86-64-v2 X86v2 instead of X64v2.
    medians = {(row['scenario'], row['variant'].replace('X86v2', 'X64v2')): float(row['median_us']) for row in rows}
    formats = {row['scenario']: 'BGRA32' if row['format'].startswith('BGRA32') else row['format'] for row in rows}
    # "Clang 18.0.1 (https://...), 64 bits" -> "Clang 18.0.1"
    compiler = rows[0]['compiler'].split(',')[0].split(' (')[0]
    return medians, formats, compiler


def collect(folder):
    machines = {}
    for path in sorted(pathlib.Path(folder).glob('benchmark_*.csv')):
        match = FILE_NAME.match(path.stem)
        if not match:
            print(f'Skipped {path.name}: its name does not follow the pattern of the portable benchmark')
            continue
        system, compiler_id, arch, processor, scalar, run = match.groups()
        medians, formats, compiler = read_medians(path)
        key = f'{system}_{compiler_id}_{arch}_{processor}'
        machine = machines.setdefault(key, {
            'id':        key,
            'system':    system,
            'compiler':  compiler,
            'arch':      arch,
            'processor': processor.replace('_', ' · '),
            'runs':      {},
            'formats':   {},
        })
        machine['runs'][('scalar' if scalar else 'simd', run)] = medians
        machine['formats'].update(formats)

    records = []
    for machine in machines.values():
        for technique, variant, base, kind, _ in TECHNIQUES:
            for (run_kind, run), medians in sorted(machine['runs'].items()):
                if run_kind != kind:
                    continue
                for (scenario, name), base_median in medians.items():
                    if name != base or (scenario, variant) not in medians:
                        continue
                    parts = SCENARIO.match(scenario)
                    if not parts:
                        continue
                    records.append({
                        'technique': technique,
                        'machine':   machine['id'],
                        'run':       run,
                        'scenario':  scenario,
                        'size':      int(parts.group(2)),
                        'rotated':   parts.group(3) == 'rotation',
                        'opaque':    parts.group(4) == 'opaque',
                        'format':    machine['formats'][scenario],
                        'ratio':     base_median / medians[(scenario, variant)],
                    })

    machine_list = [{name: machine[name] for name in ('id', 'system', 'compiler', 'arch', 'processor')} for machine in machines.values()]
    return machine_list, records


def main():
    parser = argparse.ArgumentParser(description='Score page for the results of the portable benchmark.')
    parser.add_argument('folder', help='folder with the CSV files of the portable benchmark')
    parser.add_argument('-o', '--output', help='HTML file to write; by default, score.html next to the folder')
    args = parser.parse_args()

    machines, records = collect(args.folder)
    if not records:
        sys.exit(f'{args.folder}: no results to compare')

    techniques = [{'id': t[0], 'variant': t[1], 'base': t[2], 'description': t[4]} for t in TECHNIQUES]
    data = {'machines': machines, 'techniques': techniques, 'records': records}
    output = pathlib.Path(args.output) if args.output else pathlib.Path(args.folder).resolve().parent / 'score.html'
    # json.dumps escapes neither < nor >, so a name with </script> would end the script early.
    text = json.dumps(data).replace('<', '\\u003c')
    output.write_text(TEMPLATE.replace('__DATA__', text), encoding='utf-8')
    print(f'Wrote {output}: {len(machines)} machines, {len(records)} comparisons')


TEMPLATE = r'''<!doctype html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Puntuación del benchmark</title>
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
    --diverging-low:  #e34948;
    --diverging-mid:  #f0efec;
    --diverging-high: #2a78d6;
    --ink-slower:     #9c1c1c;
    --ink-faster:     #14479a;
    --selected:       rgba(42, 120, 214, 0.12);
}
@media (prefers-color-scheme: dark) {
    :root:not([data-theme="light"]) {
        color-scheme: dark;
        --page:           #0d0d0d;
        --surface:        #1a1a19;
        --text-primary:   #ffffff;
        --text-secondary: #c3c2b7;
        --muted:          #898781;
        --grid:           #2c2c2a;
        --axis:           #383835;
        --border:         rgba(255, 255, 255, 0.10);
        --diverging-low:  #e66767;
        --diverging-mid:  #383835;
        --diverging-high: #3987e5;
        --ink-slower:     #ffa399;
        --ink-faster:     #9cc6ff;
        --selected:       rgba(57, 135, 229, 0.18);
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
    --diverging-low:  #e66767;
    --diverging-mid:  #383835;
    --diverging-high: #3987e5;
    --ink-slower:     #ffa399;
    --ink-faster:     #9cc6ff;
    --selected:       rgba(57, 135, 229, 0.18);
}
body {
    margin: 0;
    background: var(--page);
    color: var(--text-primary);
    font: 14px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif;
}
main {
    max-width: 1320px;
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
p {
    margin: 4px 0 8px;
    color: var(--text-secondary);
    max-width: 95ch;
}
.card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 12px 16px 16px;
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
table {
    border-collapse: separate;
    border-spacing: 2px;
    font-size: 12px;
    font-variant-numeric: tabular-nums;
}
th {
    color: var(--text-secondary);
    font-weight: 600;
    padding: 2px 6px;
    white-space: nowrap;
}
th.group {
    border-bottom: 1px solid var(--axis);
}
th.name {
    text-align: left;
    font-weight: 400;
    color: var(--text-primary);
    padding-right: 12px;
}
th.name .secondary {
    color: var(--text-secondary);
}
td.cell {
    min-width: 44px;
    height: 22px;
    text-align: center;
    border-radius: 3px;
    position: relative;
    cursor: default;
}
td.cell.unsure::after {
    content: "";
    position: absolute;
    top: 3px;
    right: 3px;
    width: 5px;
    height: 5px;
    border-radius: 50%;
    background: currentColor;
}
td.empty {
    color: var(--muted);
    text-align: center;
}
tr.summary th.name {
    font-weight: 600;
}
tr.first-summary th, tr.first-summary td {
    border-top: 1px solid var(--axis);
    padding-top: 6px;
}
tr.technique {
    cursor: pointer;
}
tr.technique:hover th.name, tr.technique.selected th.name {
    background: var(--selected);
}
th.unreliable {
    color: var(--muted);
    font-style: italic;
}
.legend {
    display: flex;
    flex-wrap: wrap;
    gap: 6px 18px;
    align-items: center;
    margin: 4px 0 10px;
    color: var(--text-secondary);
    font-size: 12px;
}
.ramp {
    display: inline-flex;
    gap: 1px;
    vertical-align: middle;
    margin: 0 6px;
}
.ramp span {
    width: 26px;
    height: 12px;
    border-radius: 2px;
}
.dot {
    display: inline-block;
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: var(--text-secondary);
    margin-right: 4px;
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
    max-width: 420px;
    display: none;
}
#tooltip .secondary {
    color: var(--text-secondary);
}
#tooltip table {
    border-spacing: 0;
    margin-top: 4px;
}
#tooltip td {
    padding: 0 8px 0 0;
    white-space: nowrap;
}
.slower {
    color: var(--ink-slower);
}
.faster {
    color: var(--ink-faster);
}
</style>
</head>
<body>
<main>
    <h1>Puntuación del benchmark de DrawText</h1>
    <p>Cada técnica es una variante comparada con su base, dentro de la misma ejecución. Cada celda es la media geométrica de
       la velocidad de los escenarios de su columna, con las dos ejecuciones de cada máquina juntas. «+10» significa un 10 %
       más rápido que la base, y «−10», un 10 % más lento.</p>
    <p>«Girados en el atlas» no es texto girado en pantalla: el empaquetador guarda el glifo transpuesto para que quepa
       mejor. Viene activado por defecto, y en esos escenarios lo está más del 90 % de los glifos.</p>
    <div class="legend" id="legend"></div>
    <div class="controls">
        <label><input type="checkbox" id="include-96"> Contar «96 px sin girar» en las medias. El mismo código colocado en
               otro sitio del ejecutable ha cambiado allí hasta un 41 %.</label>
    </div>

    <h2>Resumen</h2>
    <p>La media de todas las máquinas que tienen la técnica. Pulsa una fila para ver su detalle más abajo.</p>
    <div class="card" id="summary"></div>

    <h2 id="detail-title">Detalle</h2>
    <p id="detail-description"></p>
    <div class="controls">
        <label>Técnica <select id="technique"></select></label>
    </div>
    <div class="card" id="detail"></div>
</main>
<div id="tooltip"></div>
<script>
const DATA = __DATA__;

// The scores are means over several scenarios and two runs, so they are steadier than one scenario: a smaller noise
// band than the 4 % of plot_benchmark.py.
const NOISE_PCT    = 2;
// The color saturates at 50 % faster or slower, so the gains of a few percent, the usual ones, still show.
const SATURATION   = 1.5;
// Two runs whose scores differ more than this get a mark in the cell.
const UNSURE_PCT   = 2;

const COLUMNS = [
    { id: 'all',         group: '',              label: 'Todos',       test: r => true },
    { id: 'opaque',      group: '',              label: 'Opaco',       test: r => r.opaque },
    { id: 'translucent', group: '',              label: 'Translúcido', test: r => !r.opaque },
    { id: 'bgra',        group: '',              label: 'BGRA32',      test: r => r.format === 'BGRA32' },
    { id: 'up14',        group: 'Sin girar',     label: '14 px',       test: r => r.format === 'Alpha8' && !r.rotated && r.size === 14 },
    { id: 'up24',        group: 'Sin girar',     label: '24 px',       test: r => r.format === 'Alpha8' && !r.rotated && r.size === 24 },
    { id: 'up40',        group: 'Sin girar',     label: '40 px',       test: r => r.format === 'Alpha8' && !r.rotated && r.size === 40 },
    { id: 'up96',        group: 'Sin girar',     label: '96 px',       test: r => r.format === 'Alpha8' && !r.rotated && r.size === 96, unreliable: true },
    { id: 'rot14',       group: 'Girados en el atlas', label: '14 px',       test: r => r.format === 'Alpha8' && r.rotated && r.size === 14 },
    { id: 'rot24',       group: 'Girados en el atlas', label: '24 px',       test: r => r.format === 'Alpha8' && r.rotated && r.size === 24 },
    { id: 'rot40',       group: 'Girados en el atlas', label: '40 px',       test: r => r.format === 'Alpha8' && r.rotated && r.size === 40 },
    { id: 'rot96',       group: 'Girados en el atlas', label: '96 px',       test: r => r.format === 'Alpha8' && r.rotated && r.size === 96 },
];

const SYSTEMS = { windows: 'Windows', linux: 'Linux', macos: 'macOS', android: 'Android' };

const machines = DATA.machines.slice().sort((a, b) =>
    a.processor.localeCompare(b.processor) || a.system.localeCompare(b.system) || a.compiler.localeCompare(b.compiler) || a.arch.localeCompare(b.arch));
const machineById = new Map(machines.map(m => [m.id, m]));

function isUnreliable(record) {
    return record.format === 'Alpha8' && !record.rotated && record.size === 96;
}

function includes96() {
    return document.getElementById('include-96').checked;
}

function geomean(values) {
    return values.length ? Math.exp(values.reduce((sum, v) => sum + Math.log(v), 0) / values.length) : NaN;
}

function toPct(ratio) {
    return (ratio - 1) * 100;
}

// The other columns leave out the unreliable scenario unless the reader asks for it; its own column always has it.
function recordsOf(technique, machine, column) {
    return DATA.records.filter(r => r.technique === technique && r.machine === machine && column.test(r) &&
                                    (column.unreliable || includes96() || !isUnreliable(r)));
}

function score(technique, machine, column) {
    const records = recordsOf(technique, machine, column);
    if (records.length === 0) {
        return null;
    }
    const byScenario = new Map();
    const byRun      = new Map();
    for (const r of records) {
        (byScenario.get(r.scenario) || byScenario.set(r.scenario, []).get(r.scenario)).push(r);
        (byRun.get(r.run) || byRun.set(r.run, []).get(r.run)).push(r.ratio);
    }
    const scenarios = [...byScenario.entries()].map(([name, list]) => ({
        name,
        ratio: geomean(list.map(r => r.ratio)),
        runs:  list.map(r => r.ratio),
    }));
    const runs = [...byRun.values()].map(geomean);
    const unsure = runs.length > 1 && Math.abs(Math.log(Math.max(...runs) / Math.min(...runs))) > Math.log(1 + UNSURE_PCT / 100);
    return { ratio: geomean(scenarios.map(s => s.ratio)), scenarios, runs, unsure };
}

//---------------------------------------------------------------------------------------------------------------------
// Colors are interpolated in OKLab, so the steps look even to the eye.

function cssVar(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

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

// Faster is blue and slower is red, on a logarithmic scale, so twice as fast and half as fast get the same strength.
function cellColor(ratio) {
    const t         = Math.log(Math.max(ratio, 1e-6)) / Math.log(SATURATION);
    const noise     = Math.log(1 + NOISE_PCT / 100) / Math.log(SATURATION);
    const magnitude = Math.max(0, Math.min(1, (Math.abs(t) - noise) / (1 - noise)));
    const pole      = cssVar(t >= 0 ? '--diverging-high' : '--diverging-low');
    const strength  = Math.pow(magnitude, 0.7);
    return { ...mixOklab(cssVar('--diverging-mid'), pole, strength), strength };
}

// The number is tinted with its sign, so the sign also shows in the gray cells; the tint fades as the cell takes color.
const CELL_INK = {
    onLight: { plain: '#0b0b0b', slower: '#9c1c1c', faster: '#14479a' },
    onDark:  { plain: '#ffffff', slower: '#ffa399', faster: '#9cc6ff' },
};

function cellInk(ratio, color) {
    const ink     = color.lightness < 0.62 ? CELL_INK.onDark : CELL_INK.onLight;
    const rounded = Math.round(toPct(ratio));
    if (rounded === 0) {
        return ink.plain;
    }
    return mixOklab(rounded > 0 ? ink.faster : ink.slower, ink.plain, color.strength).css;
}

//---------------------------------------------------------------------------------------------------------------------

function formatNumber(value, digits) {
    return value.toLocaleString('es-ES', { minimumFractionDigits: digits, maximumFractionDigits: digits });
}

function shortPct(ratio) {
    const rounded = Math.round(toPct(ratio));
    return rounded > 0 ? `+${rounded}` : rounded < 0 ? `−${-rounded}` : '0';
}

function speedText(ratio) {
    const pct = toPct(ratio);
    if (Math.abs(pct) < 0.05) {
        return 'igual';
    }
    return pct > 0 ? `un ${formatNumber(pct, 1)} % más rápido` : `un ${formatNumber(-pct, 1)} % más lento`;
}

function speedClass(ratio) {
    const pct = toPct(ratio);
    return Math.abs(pct) < 0.05 ? '' : pct > 0 ? 'faster' : 'slower';
}

function machineLabel(machine) {
    return `<b>${machine.processor}</b> <span class="secondary">· ${SYSTEMS[machine.system] || machine.system} · ${machine.compiler} · ${machine.arch}</span>`;
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

function attachTooltip(element, html) {
    element.addEventListener('mousemove', event => showTooltip(event, typeof html === 'function' ? html() : html));
    element.addEventListener('mouseleave', () => { tooltip.style.display = 'none'; });
}

function columnTitle(column) {
    return column.group ? `${column.group}, ${column.label}` : column.label;
}

function fillCell(td, ratio, unsure) {
    if (ratio === null || Number.isNaN(ratio)) {
        td.className = 'empty';
        td.textContent = '·';
        return;
    }
    const color = cellColor(ratio);
    td.className = 'cell' + (unsure ? ' unsure' : '');
    td.style.background = color.css;
    td.style.color = cellInk(ratio, color);
    td.textContent = shortPct(ratio);
}

function headerRows(table, firstLabel) {
    const top = table.insertRow();
    const corner = document.createElement('th');
    corner.rowSpan = 2;
    corner.textContent = firstLabel;
    corner.style.textAlign = 'left';
    top.appendChild(corner);
    const groups = [];
    for (const column of COLUMNS) {
        if (groups.length && groups[groups.length - 1].name === column.group) {
            groups[groups.length - 1].count++;
        }
        else {
            groups.push({ name: column.group, count: 1 });
        }
    }
    for (const group of groups) {
        const th = document.createElement('th');
        th.colSpan = group.count;
        th.textContent = group.name;
        if (group.name) {
            th.className = 'group';
        }
        top.appendChild(th);
    }
    const bottom = table.insertRow();
    for (const column of COLUMNS) {
        const th = document.createElement('th');
        th.textContent = column.label;
        if (column.unreliable) {
            th.className = 'unreliable';
            th.title = 'Escenario con mucho ruido de colocación del código';
        }
        bottom.appendChild(th);
    }
}

//---------------------------------------------------------------------------------------------------------------------
// Summary: one row per technique, the mean over the machines that have it.

function machinesOf(technique) {
    const ids = new Set(DATA.records.filter(r => r.technique === technique).map(r => r.machine));
    return machines.filter(m => ids.has(m.id));
}

function renderSummary() {
    const card = document.getElementById('summary');
    card.innerHTML = '';
    const table = document.createElement('table');
    headerRows(table, 'Técnica');
    for (const technique of DATA.techniques) {
        const list = machinesOf(technique.id);
        const tr = table.insertRow();
        tr.className = 'technique' + (technique.id === selectedTechnique ? ' selected' : '');
        tr.addEventListener('click', () => selectTechnique(technique.id, true));
        const th = document.createElement('th');
        th.className = 'name';
        th.innerHTML = `${technique.variant} <span class="secondary">frente a ${technique.base}</span>`;
        tr.appendChild(th);
        for (const column of COLUMNS) {
            const scores = list.map(m => ({ machine: m, value: score(technique.id, m.id, column) })).filter(s => s.value);
            const td = tr.insertCell();
            const mean = scores.length ? geomean(scores.map(s => s.value.ratio)) : null;
            fillCell(td, mean, false);
            if (mean !== null) {
                attachTooltip(td, () => summaryTooltip(technique, column, scores, mean));
            }
        }
    }
    card.appendChild(table);
}

function summaryTooltip(technique, column, scores, mean) {
    const sorted = scores.slice().sort((a, b) => a.value.ratio - b.value.ratio);
    const noise  = 1 + NOISE_PCT / 100;
    const faster = scores.filter(s => s.value.ratio > noise).length;
    const slower = scores.filter(s => s.value.ratio < 1 / noise).length;
    const worst  = sorted[0];
    const best   = sorted[sorted.length - 1];
    return `<b>${technique.variant}</b> <span class="secondary">frente a ${technique.base} · ${columnTitle(column)}</span><br>` +
           `Media de ${scores.length} máquinas: <span class="${speedClass(mean)}">${speedText(mean)}</span><br>` +
           `Más rápido en ${faster}, más lento en ${slower}, dentro del ±${NOISE_PCT} % en ${scores.length - faster - slower}<br>` +
           `<span class="secondary">Peor:</span> <span class="${speedClass(worst.value.ratio)}">${shortPct(worst.value.ratio)} %</span> ` +
           `<span class="secondary">${worst.machine.processor} · ${worst.machine.compiler}</span><br>` +
           `<span class="secondary">Mejor:</span> <span class="${speedClass(best.value.ratio)}">${shortPct(best.value.ratio)} %</span> ` +
           `<span class="secondary">${best.machine.processor} · ${best.machine.compiler}</span>`;
}

//---------------------------------------------------------------------------------------------------------------------
// Detail: one row per machine, then the mean and the worst case.

let selectedTechnique = DATA.techniques[0].id;

function renderDetail() {
    const technique = DATA.techniques.find(t => t.id === selectedTechnique);
    document.getElementById('detail-title').textContent = `Detalle: ${technique.variant} frente a ${technique.base}`;
    document.getElementById('detail-description').textContent = technique.description + '.';
    const card = document.getElementById('detail');
    card.innerHTML = '';
    const table = document.createElement('table');
    headerRows(table, 'Máquina');

    const list = machinesOf(technique.id);
    const columnScores = COLUMNS.map(() => []);
    for (const machine of list) {
        const tr = table.insertRow();
        const th = document.createElement('th');
        th.className = 'name';
        th.innerHTML = machineLabel(machine);
        tr.appendChild(th);
        COLUMNS.forEach((column, index) => {
            const value = score(technique.id, machine.id, column);
            const td = tr.insertCell();
            fillCell(td, value ? value.ratio : null, value && value.unsure);
            if (value) {
                columnScores[index].push(value.ratio);
                attachTooltip(td, () => cellTooltip(technique, machine, column, value));
            }
        });
    }

    const rows = [
        { label: 'Media', reduce: values => geomean(values) },
        { label: 'Peor', reduce: values => Math.min(...values) },
    ];
    rows.forEach((summary, index) => {
        const tr = table.insertRow();
        tr.className = 'summary' + (index === 0 ? ' first-summary' : '');
        const th = document.createElement('th');
        th.className = 'name';
        th.textContent = summary.label;
        tr.appendChild(th);
        COLUMNS.forEach((column, columnIndex) => {
            const values = columnScores[columnIndex];
            fillCell(tr.insertCell(), values.length ? summary.reduce(values) : null, false);
        });
    });
    card.appendChild(table);
}

function cellTooltip(technique, machine, column, value) {
    const runs = value.runs.map(ratio => `<span class="${speedClass(ratio)}">${shortPct(ratio)} %</span>`).join(' y ');
    let html = `<b>${technique.variant}</b> <span class="secondary">frente a ${technique.base} · ${columnTitle(column)}</span><br>` +
               `${machineLabel(machine)}<br>` +
               `<span class="${speedClass(value.ratio)}">${speedText(value.ratio)}</span>` +
               (value.runs.length > 1 ? ` <span class="secondary">(ejecuciones: ${runs})</span>` : '');
    if (value.unsure) {
        html += `<br><span class="secondary">Las dos ejecuciones difieren más de un ${UNSURE_PCT} %.</span>`;
    }
    html += '<table>';
    for (const scenario of value.scenarios) {
        const each = scenario.runs.map(ratio => `${shortPct(ratio)}`).join(' / ');
        html += `<tr><td class="secondary">${scenario.name}</td><td class="${speedClass(scenario.ratio)}">${shortPct(scenario.ratio)} %</td>` +
                `<td class="secondary">${each}</td></tr>`;
    }
    return html + '</table>';
}

function selectTechnique(id, scroll) {
    selectedTechnique = id;
    document.getElementById('technique').value = id;
    history.replaceState(null, '', '#' + id);
    renderSummary();
    renderDetail();
    if (scroll) {
        document.getElementById('detail-title').scrollIntoView({ behavior: 'smooth', block: 'start' });
    }
}

function renderLegend() {
    const legend = document.getElementById('legend');
    const steps = [0.6, 0.75, 0.9, 1, 1.1, 1.3, 1.6];
    const ramp = steps.map(ratio => `<span style="background:${cellColor(ratio).css}"></span>`).join('');
    legend.innerHTML = `<span>Más lento<span class="ramp">${ramp}</span>Más rápido</span>` +
                       `<span>Gris: dentro del ±${NOISE_PCT} %</span>` +
                       `<span><span class="dot"></span>Las dos ejecuciones difieren más de un ${UNSURE_PCT} %</span>` +
                       `<span>«·»: la máquina no tiene esa técnica o ese escenario</span>`;
}

function renderAll() {
    renderLegend();
    renderSummary();
    renderDetail();
}

const select = document.getElementById('technique');
for (const technique of DATA.techniques) {
    const option = document.createElement('option');
    option.value = technique.id;
    option.textContent = `${technique.variant} frente a ${technique.base}`;
    select.appendChild(option);
}
select.addEventListener('change', () => selectTechnique(select.value, false));
document.getElementById('include-96').addEventListener('change', renderAll);
window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', renderAll);
const linked = DATA.techniques.find(t => '#' + t.id === location.hash);
if (linked) {
    selectedTechnique = linked.id;
    select.value = linked.id;
}
renderAll();
</script>
</body>
</html>
'''

if __name__ == '__main__':
    main()
