/* ============================================================================
   twin.js  -  the wallet in 3D, drawn from the firmware's own outputs
   ----------------------------------------------------------------------------
   Nothing here decides what the wallet does. Every frame it reads what the
   firmware did to its (virtual) hardware - the servo angle, the LED pin, the
   buzzer, the advertising interval - and moves the concept model to match:

     servo angle      slides the latch and fans the cards out of the top
     LED pin          lights the status LED
     buzzer           sends sound rings out from the wallet
     advertising      radio pulses, four times faster in the 20 ms alarm beacon
     alarm            a red edge; shaking jolts the model

   Press and hold the sensor to touch it with your finger.
   ========================================================================= */

import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { RoomEnvironment } from 'three/addons/environments/RoomEnvironment.js';

const SERVO_LOCKED = 20, SERVO_RELEASED = 95;      // the firmware's defaults (GET servolock / servorel)
const SLIDER_TRAVEL = 0.004;                       // m

export async function createTwin(canvas, { url, onPress, onRelease }) {
  const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: true });
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
  renderer.toneMapping = THREE.AgXToneMapping;
  renderer.toneMappingExposure = 0.95;

  const scene = new THREE.Scene();
  const pmrem = new THREE.PMREMGenerator(renderer);
  scene.environment = pmrem.fromScene(new RoomEnvironment(), 0.04).texture;
  scene.environmentIntensity = 0.55;          // PVD black should read black, not studio grey
  const key = new THREE.DirectionalLight(0xffffff, 1.6);
  key.position.set(0.35, 0.55, 0.45);
  const rim = new THREE.DirectionalLight(0xffffff, 1.1);
  rim.position.set(-0.5, 0.25, -0.35);
  scene.add(key, rim);

  const camera = new THREE.PerspectiveCamera(28, 1, 0.005, 5);
  camera.position.set(0.155, 0.1, 0.255);      // three-quarter view, so it reads as an object
  const controls = new OrbitControls(camera, canvas);
  controls.target.set(0, 0.05, 0);
  controls.enableDamping = true;
  controls.enablePan = false;
  controls.minDistance = 0.15;
  controls.maxDistance = 0.55;
  controls.minPolarAngle = 0.25;
  controls.maxPolarAngle = Math.PI / 2 + 0.1;

  const gltf = await new GLTFLoader().loadAsync(url);
  const wallet = gltf.scene;
  scene.add(wallet);
  const part = name => wallet.getObjectByName(name);

  // The concept shows its cards fanned out; closed, they sit inside the shell.
  const cards = [1, 2, 3, 4].map(i => part(`card_${i}`)).filter(Boolean);
  const fanned = cards.map(c => c.position.clone());
  const tucked = cards.map(c => new THREE.Vector3(c.position.x, 0, c.position.z));
  const slider = part('latch_slider');
  const sliderOpen = slider?.position.clone();

  const ledMesh = meshOf(part('status_led'));
  const led = ledMesh && (ledMesh.material = ledMesh.material.clone());
  const sensorMeshes = ['sensor_pad', 'sensor_well'].map(n => meshOf(part(n))).filter(Boolean);
  const pad = sensorMeshes[0] && (sensorMeshes[0].material = sensorMeshes[0].material.clone());

  // a pool of rings for sound and radio
  const rings = [];
  const ringGeo = new THREE.RingGeometry(0.98, 1.0, 96);
  function emit(kind) {
    const m = new THREE.Mesh(ringGeo, new THREE.MeshBasicMaterial({
      color: kind === 'sound' ? 0xffffff : kind === 'alarm' ? 0xff5a4e : 0x9fb6ff,
      transparent: true, opacity: kind === 'sound' ? 0.55 : 0.28, side: THREE.DoubleSide, depthWrite: false }));
    m.position.copy(controls.target);
    m.userData = { kind, born: performance.now(), life: kind === 'sound' ? 700 : 1600,
                   from: kind === 'sound' ? 0.05 : 0.06, to: kind === 'sound' ? 0.13 : 0.24 };
    scene.add(m);
    rings.push(m);
  }

  // ---- what the firmware says, and where the model is heading ----------
  let want = { servo: SERVO_LOCKED, led: 0, buzz: 0, alarm: 0, shaking: 0, touched: false, adv: 244, advOn: 1, conn: 0 };
  let open = 0, glow = 0, lastBuzz = 0, lastPulse = 0, lastSound = 0;

  function update(p) {
    want = {
      servo: p.servo >= 0 ? p.servo : SERVO_LOCKED, led: p.led, buzz: p.buzz, alarm: p.alarm,
      shaking: p.shaking, touched: p.platen !== 'empty', adv: p.adv || 244, advOn: p.adv_on, conn: p.conn,
    };
  }

  // ---- press the sensor -------------------------------------------------
  const ray = new THREE.Raycaster(), ndc = new THREE.Vector2();
  let pressing = false;
  const overSensor = e => {
    const r = canvas.getBoundingClientRect();
    ndc.set(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
    ray.setFromCamera(ndc, camera);
    return ray.intersectObjects(sensorMeshes, false).length > 0;
  };
  canvas.addEventListener('pointermove', e => canvas.classList.toggle('over-sensor', !pressing && overSensor(e)));
  canvas.addEventListener('pointerdown', e => {
    if (e.button !== 0 || !overSensor(e)) return;
    pressing = true;
    controls.enabled = false;
    try { canvas.setPointerCapture(e.pointerId); } catch { /* fine */ }
    onPress?.();
  });
  const release = () => { if (!pressing) return; pressing = false; controls.enabled = true; onRelease?.(); };
  for (const ev of ['pointerup', 'pointercancel', 'lostpointercapture']) canvas.addEventListener(ev, release);

  // ---- the loop ---------------------------------------------------------
  let visible = true, raf = 0, last = performance.now();
  new IntersectionObserver(([e]) => { visible = e.isIntersecting; if (visible && !raf) raf = requestAnimationFrame(frame); })
    .observe(canvas);
  new ResizeObserver(() => {
    const { clientWidth: w, clientHeight: h } = canvas;
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
  }).observe(canvas);

  function frame(now) {
    raf = 0;
    const dt = Math.min(0.05, (now - last) / 1000);
    last = now;

    // latch and cards follow the servo
    const t = THREE.MathUtils.clamp((want.servo - SERVO_LOCKED) / (SERVO_RELEASED - SERVO_LOCKED), 0, 1);
    open += (t - open) * Math.min(1, dt * 7);
    cards.forEach((c, i) => {
      const k = THREE.MathUtils.clamp(open * 1.25 - i * 0.08, 0, 1);
      c.position.lerpVectors(tucked[i], fanned[i], easeOut(k));
    });
    if (slider) slider.position.y = sliderOpen.y - SLIDER_TRAVEL * (1 - open);

    // the LED, and the finger on the sensor
    if (led) {
      led.emissive.setRGB(0.25, 1.0, 0.45);
      led.emissiveIntensity += ((want.led ? 2.2 : 0) - led.emissiveIntensity) * Math.min(1, dt * 25);
    }
    glow += ((want.touched ? 1 : 0) - glow) * Math.min(1, dt * 12);
    if (pad) { pad.emissive.setRGB(0.6, 0.7, 1.0); pad.emissiveIntensity = glow * 0.35; }

    // shaking: the bench is moving it, so move it
    if (want.shaking) {
      wallet.rotation.set((Math.random() - .5) * .06, (Math.random() - .5) * .08, (Math.random() - .5) * .05);
      wallet.position.set((Math.random() - .5) * .002, (Math.random() - .5) * .002, 0);
    } else {
      wallet.rotation.x *= .8; wallet.rotation.y *= .8; wallet.rotation.z *= .8;
      wallet.position.multiplyScalar(.8);
    }

    // sound and radio
    if (want.buzz && now - lastSound > 260) { emit('sound'); lastSound = now; }
    if (want.buzz && !lastBuzz) emit('sound');
    lastBuzz = want.buzz;
    const every = want.adv <= 40 ? 380 : 1500;      // shown slower than real, at the right ratio
    if (want.advOn && !want.conn && now - lastPulse > every) { emit(want.alarm ? 'alarm' : 'radio'); lastPulse = now; }
    for (let i = rings.length - 1; i >= 0; i--) {
      const r = rings[i], u = (now - r.userData.born) / r.userData.life;
      if (u >= 1) { scene.remove(r); r.material.dispose(); rings.splice(i, 1); continue; }
      const s = r.userData.from + (r.userData.to - r.userData.from) * easeOut(u);
      r.scale.setScalar(s);
      r.quaternion.copy(camera.quaternion);
      r.material.opacity = (r.userData.kind === 'sound' ? 0.55 : 0.3) * (1 - u);
    }

    controls.update();
    renderer.render(scene, camera);
    if (visible) raf = requestAnimationFrame(frame);
  }
  raf = requestAnimationFrame(frame);

  return { update };
}

function meshOf(obj) {
  let found = null;
  obj?.traverse(o => { if (!found && o.isMesh) found = o; });
  return found;
}

function easeOut(x) { return 1 - Math.pow(1 - x, 3); }
