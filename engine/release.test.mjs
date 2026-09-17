import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const read = p => readFileSync(new URL(p, import.meta.url), 'utf8');

test('standalone no-argument launch selects the water room and explicit normal lens wins', () => {
  const main = read('src/main.cpp');
  assert.match(main, /if \(argc == 1\)\s*\{\s*options\.fluidRoom = options\.fluid = true;/);
  assert.match(main, /arg == "--normal-lens"/);
  assert.match(main, /options\.fisheye = !normalLens &&/);
});

test('portable primary launcher is relative, selects baseline water and does not request elevation', () => {
  const cmd = read('../release/Play Water Lab.cmd');
  assert.match(cmd, /%~dp0NVMatrixFluidLab\.exe/);
  assert.match(cmd, /--fluid-room --normal-lens/);
  assert.doesNotMatch(cmd, /LOCALAPPDATA|RunAs|fluid-backend=cuda/);
});

test('shared audio and UI no longer include predecessor renderer or DLSS headers', () => {
  assert.doesNotMatch(read('../shared/src/audio.cpp'), /#include "dlss\.h"/);
  assert.doesNotMatch(read('../shared/src/ui_renderer.cpp'), /#include "renderer\.h"/);
});

test('recording tour drives real movement and jump inputs, with no fake splashes', () => {
  const main = read('src/main.cpp');
  const tour = main.slice(main.indexOf('if (demoTour) {'), main.indexOf('if (options.fluidTemporalTest) {', main.indexOf('if (demoTour) {')));
  assert.match(tour, /game->input\.forward = ahead/);
  assert.match(tour, /game->input\.jump = renderer\.frame % 180 == 90/);
  assert.doesNotMatch(tour, /game->place|setLinearVelocity|fluid->/);
  assert.match(main, /demoTour && \(!automation/);
});
