/* ============================================================================
   cadview.js  -  the generator's assemblies, exploded on demand
   ----------------------------------------------------------------------------
   Loads cad/assembly/model_{a,b}.glb, written by cad/mono_wallet_cad.py, and
   the JSON part list next to each: every node knows how far it travels when
   the model is pulled apart, so the slider reveals the stack-up in order.
   Hover a part to name it.
   ========================================================================= */

import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { RoomEnvironment } from 'three/addons/environments/RoomEnvironment.js';

const NAMES = {
  B1: 'B1 - card sleeve, back half', B2: 'B2 - electronics shell, front half', B3: 'B3 - component tray',
  A1: 'A1 - front half', A2: 'A2 - back half', A3: 'A3 - spring clip',
  card: 'card', xiao_usbc: 'USB-C receptacle', xiao: 'Seeed XIAO nRF52840 Sense', r502: 'GROW R502-A fingerprint sensor',
  lipo: 'LiPo cell, 402030 class', servo: '1.7 g micro servo', led: 'status LED', buzzer: '9 mm magnetic buzzer',
  screw: 'M2 x 12 self-tapping screw',
};
const label = name => {
  const k = Object.keys(NAMES).sort((a, b) => b.length - a.length).find(p => name.startsWith(p));
  return k ? NAMES[k] : name;
};

export async function createCadViewer(canvas, { base, slider, onHover }) {
  const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: true });
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
  renderer.toneMapping = THREE.AgXToneMapping;
  renderer.toneMappingExposure = 1.0;
  const scene = new THREE.Scene();
  scene.environment = new THREE.PMREMGenerator(renderer).fromScene(new RoomEnvironment(), 0.04).texture;
  scene.environmentIntensity = 0.6;
  const sun = new THREE.DirectionalLight(0xffffff, 1.4);
  sun.position.set(0.3, 0.6, 0.4);
  scene.add(sun);

  const camera = new THREE.PerspectiveCamera(30, 16 / 10, 0.005, 5);
  const controls = new OrbitControls(camera, canvas);
  controls.enableDamping = true;
  controls.enablePan = false;
  controls.autoRotate = true;
  controls.autoRotateSpeed = 0.6;
  canvas.addEventListener('pointerdown', () => { controls.autoRotate = false; });

  const loader = new GLTFLoader();
  let model = null, parts = [], hovered = null;

  async function show(which) {
    const [gltf, meta] = await Promise.all([
      loader.loadAsync(`${base}/model_${which}.glb`),
      fetch(`${base}/model_${which}.json`).then(r => r.json()),
    ]);
    if (model) scene.remove(model);
    model = gltf.scene;
    const byName = Object.fromEntries(meta.parts.map(p => [p.name, p]));
    parts = [];
    model.traverse(o => {
      if (!o.isMesh) return;
      const node = o.parent && byName[o.parent.name] ? o.parent : o;
      const info = byName[node.name];
      if (!info) return;
      o.material = o.material.clone();
      parts.push({ mesh: o, node, info, y0: node.position.y });
    });
    scene.add(model);
    // frame it: centred, seen from the front and above
    const box = new THREE.Box3().setFromObject(model);
    const c = box.getCenter(new THREE.Vector3()), size = box.getSize(new THREE.Vector3()).length();
    controls.target.copy(c);
    camera.position.copy(c).add(new THREE.Vector3(0.55, 0.62, 0.95).normalize().multiplyScalar(size * 1.9));
    explode(Number(slider.value));
  }

  function explode(t) {
    for (const p of parts) p.node.position.y = p.y0 + (p.info.explode_z / 1000) * t;
  }
  slider.addEventListener('input', () => explode(Number(slider.value)));

  const ray = new THREE.Raycaster(), ndc = new THREE.Vector2();
  canvas.addEventListener('pointermove', e => {
    const r = canvas.getBoundingClientRect();
    ndc.set(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
    ray.setFromCamera(ndc, camera);
    const hit = ray.intersectObjects(parts.map(p => p.mesh), false)[0];
    const p = hit && parts.find(x => x.mesh === hit.object);
    if (p === hovered) return;
    if (hovered) hovered.mesh.material.emissive?.setRGB(0, 0, 0);
    hovered = p ?? null;
    if (hovered) hovered.mesh.material.emissive?.setRGB(0.12, 0.12, 0.14);
    const s = hovered?.info.size_mm;
    onHover?.(hovered ? `${label(hovered.info.name)} · ${s[0]} x ${s[1]} x ${s[2]} mm` : 'Hover a part');
  });

  let visible = false;
  new IntersectionObserver(([e]) => { visible = e.isIntersecting; if (visible) requestAnimationFrame(frame); }).observe(canvas);
  new ResizeObserver(() => {
    const { clientWidth: w, clientHeight: h } = canvas;
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
  }).observe(canvas);
  function frame() {
    controls.update();
    renderer.render(scene, camera);
    if (visible) requestAnimationFrame(frame);
  }

  await show('b');
  return { show };
}
