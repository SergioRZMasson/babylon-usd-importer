import { ArcRotateCamera } from '@babylonjs/core/Cameras/arcRotateCamera.js';
import { Engine } from '@babylonjs/core/Engines/engine.js';
import { HemisphericLight } from '@babylonjs/core/Lights/hemisphericLight.js';
import { Color4 } from '@babylonjs/core/Maths/math.color.js';
import { Vector3 } from '@babylonjs/core/Maths/math.vector.js';
import { CubeTexture } from '@babylonjs/core/Materials/Textures/cubeTexture.js';
import { Scene } from '@babylonjs/core/scene.js';
import { OpenUsdBabylonLoader } from 'babylon-usd-importer';

const els = {
    file: document.getElementById('file'),
    folder: document.getElementById('folder'),
    samples: document.getElementById('samples'),
    status: document.getElementById('status'),
    log: document.getElementById('log'),
    stats: document.getElementById('stats'),
    canvas: document.getElementById('canvas'),
    drop: document.getElementById('drop'),
    loading: document.getElementById('loading'),
    loadingMessage: document.getElementById('loadingMessage'),
};

const formatBytes = (value) =>
    value < 1024
        ? `${value} B`
        : value < 1048576
          ? `${(value / 1024).toFixed(1)} KB`
          : `${(value / 1048576).toFixed(2)} MB`;

function log(message, level = 'info') {
    const line = document.createElement('div');
    line.className = `line ${level}`;
    line.textContent = message;
    els.log.append(line);
    els.log.scrollTop = els.log.scrollHeight;
}

function setStatus(text, busy = false) {
    els.status.textContent = text;
    els.status.classList.toggle('busy', busy);
}

function showLoading(message) {
    els.loadingMessage.textContent = message;
    els.loading.setAttribute('aria-busy', 'true');
    els.loading.classList.add('active');
}

function hideLoading() {
    els.loading.classList.remove('active');
    els.loading.setAttribute('aria-busy', 'false');
}

function setBusy(busy) {
    els.file.disabled = busy;
    els.folder.disabled = busy;
    for (const button of els.samples.querySelectorAll('button')) {
        button.disabled = busy;
    }
}

function renderStats(rows) {
    els.stats.replaceChildren(
        ...rows.flatMap(([label, value]) => {
            const key = document.createElement('dt');
            key.textContent = label;
            const result = document.createElement('dd');
            result.textContent = value;
            return [key, result];
        }),
    );
}

const engine = new Engine(els.canvas, true, {
    preserveDrawingBuffer: true,
    stencil: true,
});
const scene = new Scene(engine);
scene.clearColor = new Color4(0.09, 0.09, 0.11, 1);

const camera = new ArcRotateCamera(
    'camera',
    -Math.PI / 2.2,
    Math.PI / 2.8,
    6,
    Vector3.Zero(),
    scene,
);
camera.attachControl(els.canvas, true);
camera.wheelDeltaPercentage = 0.02;
camera.pinchDeltaPercentage = 0.02;
camera.minZ = 0.01;

const fill = new HemisphericLight('fill', new Vector3(0.3, 1, 0.2), scene);
fill.intensity = 0.9;
scene.environmentTexture = CubeTexture.CreateFromPrefilteredData(
    'https://assets.babylonjs.com/environments/environmentSpecular.env',
    scene,
);
scene.environmentIntensity = 1.0;

engine.runRenderLoop(() => scene.render());
addEventListener('resize', () => engine.resize());

const loader = new OpenUsdBabylonLoader();
let loadedContainer = null;
let loadGeneration = 0;
let pendingLoad = Promise.resolve();

globalThis.__demo = {
    engine,
    scene,
    camera,
    loader,
    getLoaded: () => loadedContainer,
};

function disposeLoadedAsset() {
    if (!loadedContainer) return;
    for (const group of loadedContainer.animationGroups) {
        group.stop();
    }
    if (loadedContainer.cameras.includes(scene.activeCamera)) {
        scene.activeCamera = camera;
        camera.attachControl(els.canvas, true);
    }
    loadedContainer.dispose();
    loadedContainer = null;
}

function frameScene(meshes) {
    if (meshes.length === 0) return;

    let min = null;
    let max = null;
    for (const mesh of meshes) {
        mesh.computeWorldMatrix(true);
        const { minimumWorld, maximumWorld } = mesh.getBoundingInfo().boundingBox;
        min = min ? Vector3.Minimize(min, minimumWorld) : minimumWorld.clone();
        max = max ? Vector3.Maximize(max, maximumWorld) : maximumWorld.clone();
    }

    const size = max.subtract(min);
    const center = min.add(max).scale(0.5);
    const boundingRadius = Math.max(size.length() * 0.5, 1e-4);
    const aspect = engine.getAspectRatio(camera);
    const vertical = camera.fov;
    const horizontal = 2 * Math.atan(Math.tan(vertical / 2) * aspect);
    const fitting = Math.min(vertical, horizontal);

    camera.setTarget(center);
    camera.radius = (boundingRadius / Math.sin(fitting / 2)) * 1.15;
    camera.lowerRadiusLimit = boundingRadius * 0.05;
    camera.upperRadiusLimit = boundingRadius * 40;
    camera.minZ = boundingRadius / 500;
    camera.maxZ = boundingRadius * 200;
    camera.wheelPrecision = 60 / boundingRadius;
    camera.alpha = -Math.PI / 2.2;
    camera.beta = Math.PI / 3;
}

async function loadUsd(bytes, fileName, files = {}) {
    const generation = ++loadGeneration;
    const sourceByteLength =
        bytes.byteLength +
        Object.values(files).reduce((total, file) => total + file.byteLength, 0);
    const run = pendingLoad.then(
        () => runLoad(generation, bytes, fileName, files, sourceByteLength),
        () => runLoad(generation, bytes, fileName, files, sourceByteLength),
    );
    pendingLoad = run.catch(() => undefined);
    return run;
}

async function runLoad(generation, bytes, fileName, files, sourceByteLength) {
    if (generation !== loadGeneration) return;

    els.log.replaceChildren();
    els.stats.replaceChildren();
    setStatus(`Loading ${fileName}…`, true);
    showLoading(`Staging ${fileName}...`);
    setBusy(true);

    try {
        const started = performance.now();
        const result = await loader.loadAsync(scene, bytes, {
            fileName,
            files,
            addToScene: false,
            onProgress: ({ message }) => {
                els.loadingMessage.textContent = message;
            },
            onLog: (level, message) => log(message, level),
        });
        const totalMs = performance.now() - started;

        if (generation !== loadGeneration) {
            result.container.dispose();
            return;
        }

        if (result.missingAssets.length > 0) {
            log(
                `${result.missingAssets.length} referenced file(s) were not supplied: ` +
                    result.missingAssets.join(', '),
                'warning',
            );
        }

        disposeLoadedAsset();
        result.container.addAllToScene();
        loadedContainer = result.container;

        const meshes = result.container.meshes.filter((mesh) => mesh.getTotalVertices() > 0);
        frameScene(meshes);

        const runtimeVertices = meshes.reduce(
            (total, mesh) => total + mesh.getTotalVertices(),
            0,
        );
        const runtimeTriangles = meshes.reduce(
            (total, mesh) => total + (mesh.getTotalIndices() / 3 || 0),
            0,
        );
        const { timings, statistics } = result;
        const commandBytes = statistics.commandBytes + statistics.dataBytes;

        renderStats([
            ['Source', `${fileName} · ${formatBytes(sourceByteLength)}`],
            ['Command + data', formatBytes(commandBytes)],
            ['OpenUSD open', `${timings.stageOpenMs.toFixed(0)} ms`],
            ['Stage traversal', `${timings.stageReadMs.toFixed(0)} ms`],
            ['Vertex preparation', `${timings.preparationMs.toFixed(0)} ms`],
            ['Command packing', `${timings.packingMs.toFixed(0)} ms`],
            ['Heap → JavaScript', `${timings.heapCopyMs.toFixed(0)} ms`],
            ['Babylon materialize', `${timings.materializeMs.toFixed(0)} ms`],
            ['Total', `${totalMs.toFixed(0)} ms`],
            ['Source meshes', statistics.meshes.toLocaleString()],
            ['Native instances', statistics.instances.toLocaleString()],
            ['Unique vertices', statistics.vertices.toLocaleString()],
            ['Unique triangles', statistics.triangles.toLocaleString()],
            ['Runtime meshes', meshes.length.toLocaleString()],
            ['Runtime vertices', runtimeVertices.toLocaleString()],
            ['Runtime triangles', Math.round(runtimeTriangles).toLocaleString()],
            ['Materials', result.container.materials.length.toLocaleString()],
            ['Animations', result.container.animationGroups.length.toLocaleString()],
            ['Skeletons', result.container.skeletons.length.toLocaleString()],
        ]);

        for (const group of result.container.animationGroups) {
            group.play(true);
        }

        log(
            `Extracted ${formatBytes(statistics.commandBytes)} of commands and ` +
                `${formatBytes(statistics.dataBytes)} of raw data.`,
        );
        setStatus(
            `${fileName} — ${meshes.length} meshes, ${runtimeVertices.toLocaleString()} vertices`,
        );
    } catch (error) {
        if (generation !== loadGeneration) return;
        setStatus('USD loading failed.');
        log(String(error?.message ?? error), 'error');
        console.error(error);
    } finally {
        if (generation === loadGeneration) {
            hideLoading();
            setBusy(false);
        }
    }
}

const USD_EXTENSIONS = ['usd', 'usda', 'usdc', 'usdz'];
const extensionOf = (name) => name.slice(name.lastIndexOf('.') + 1).toLowerCase();
const isUsd = (name) => USD_EXTENSIONS.includes(extensionOf(name));

async function loadFileSet(files) {
    if (files.length === 0) return;

    const entries = files.map((file) => ({
        file,
        path: (file.webkitRelativePath || file.name).replace(/\\/g, '/'),
    }));
    const segments = entries[0].path.split('/');
    const commonRoot = entries.length > 1 && segments.length > 1 ? `${segments[0]}/` : '';
    for (const entry of entries) {
        if (commonRoot && entry.path.startsWith(commonRoot)) {
            entry.path = entry.path.slice(commonRoot.length);
        }
    }

    const candidates = entries.filter((entry) => isUsd(entry.path));
    if (candidates.length === 0) {
        setStatus('No .usd/.usda/.usdc/.usdz file in that selection.');
        return;
    }

    const requestedRoot = new URLSearchParams(location.search).get('root');
    candidates.sort(
        (left, right) =>
            Number(!(left.path === requestedRoot || left.path.endsWith(`/${requestedRoot}`))) -
                Number(!(right.path === requestedRoot || right.path.endsWith(`/${requestedRoot}`))) ||
            left.path.split('/').length - right.path.split('/').length ||
            left.path.localeCompare(right.path),
    );
    const root = candidates[0];

    const supportingFiles = {};
    for (const entry of entries) {
        if (entry === root) continue;
        supportingFiles[entry.path] = new Uint8Array(await entry.file.arrayBuffer());
    }
    await loadUsd(
        new Uint8Array(await root.file.arrayBuffer()),
        root.path,
        supportingFiles,
    );
}

async function collectEntry(entry, prefix = '') {
    if (entry.isFile) {
        const file = await new Promise((resolve, reject) => entry.file(resolve, reject));
        Object.defineProperty(file, 'webkitRelativePath', {
            value: prefix + file.name,
            configurable: true,
        });
        return [file];
    }
    if (!entry.isDirectory) return [];

    const reader = entry.createReader();
    const children = [];
    for (;;) {
        const batch = await new Promise((resolve, reject) =>
            reader.readEntries(resolve, reject),
        );
        if (batch.length === 0) break;
        children.push(...batch);
    }
    const nested = await Promise.all(
        children.map((child) => collectEntry(child, `${prefix}${entry.name}/`)),
    );
    return nested.flat();
}

async function handleInput(event) {
    const input = event.target;
    const files = [...input.files];
    input.value = '';
    await loadFileSet(files);
}

els.file.addEventListener('change', handleInput);
els.folder.addEventListener('change', handleInput);

for (const type of ['dragenter', 'dragover']) {
    document.addEventListener(type, (event) => {
        event.preventDefault();
        els.drop.classList.add('active');
    });
}
for (const type of ['dragleave', 'drop']) {
    document.addEventListener(type, (event) => {
        event.preventDefault();
        els.drop.classList.remove('active');
    });
}

document.addEventListener('drop', async (event) => {
    try {
        const items = [...(event.dataTransfer?.items ?? [])];
        const entries = items
            .map((item) => item.webkitGetAsEntry?.())
            .filter((entry) => entry != null);
        if (entries.length > 0) {
            const collected = await Promise.all(
                entries.map((entry) => collectEntry(entry)),
            );
            await loadFileSet(collected.flat());
            return;
        }
        await loadFileSet(
            [...(event.dataTransfer?.files ?? [])].filter(
                (file) => file.size > 0 || isUsd(file.name),
            ),
        );
    } catch (error) {
        setStatus('Could not read the dropped files.');
        log(String(error?.message ?? error), 'error');
        setBusy(false);
    }
});

async function loadSample(name) {
    setStatus(`Fetching ${name}…`, true);
    try {
        const response = await fetch(`./assets/${name}`);
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }
        await loadUsd(new Uint8Array(await response.arrayBuffer()), name);
    } catch (error) {
        setStatus(`Could not fetch ${name}.`);
        log(String(error?.message ?? error), 'error');
        setBusy(false);
    }
}

els.samples.addEventListener('click', async (event) => {
    const name = event.target?.dataset?.sample;
    if (name) {
        await loadSample(name);
    }
});

setBusy(false);
hideLoading();
setStatus('Ready — choose a USD file or sample.');

const requestedSample = new URLSearchParams(location.search).get('sample');
if (
    requestedSample &&
    [...els.samples.querySelectorAll('button')].some(
        (button) => button.dataset.sample === requestedSample,
    )
) {
    await loadSample(requestedSample);
}
