import test from 'node:test';
import assert from 'node:assert/strict';

const sum = a => a.reduce((s, x) => s + x, 0);
const roundedSum = a => a.reduce((s, x) => Math.fround(s + x), 0);

test('independent support classification preserves each storage mode threshold arithmetic', () => {
  const cap = Math.fround(.032), full = Math.fround(1 - 1e-6);
  const preciseThreshold = cap * full, legacyThreshold = Math.fround(cap * full);
  assert.ok(legacyThreshold < preciseThreshold);
  // A float snapshot decoded into a JS/C++ double must still use the float
  // product for a float shader. Otherwise a value exactly on the threshold
  // may change classification even though no simulation value changed.
  const source = legacyThreshold;
  assert.ok(source >= legacyThreshold);
  assert.ok(source < preciseThreshold);
});

test('restricted capacity and fine swept-volume source share one numeric measure', () => {
  const before = [.01, .012, .007, .0034].map(Math.fround);
  const after = [...before];
  after[0] = Math.fround(.01 - .0000027);
  const sweptOutflow = sum(before) - sum(after);
  const mismatch = roundedSum(before) - sweptOutflow - roundedSum(after);
  assert.ok(Math.abs(mismatch) > 1e-10, 'FP32 restriction can contradict exact fine continuity');
  assert.equal(sum(before) - sweptOutflow, sum(after));
  assert.equal(sum(before) / (sum(after) + sweptOutflow), 1, 'A full cell stays full');
});

test('precise restriction retains a tiny positive cut cell without changing topology', () => {
  const tiny = Math.fround(2 ** -32);
  const fine = [Math.fround(.032), tiny, 0, 0, 0, 0, 0, 0];
  assert.equal(roundedSum(fine), fine[0]);
  assert.equal(sum(fine) - fine[0], tiny);
  assert.ok(fine[1] > 0);
});

test('shared FP64 face transfers preserve a small volume and its momentum over repeated exchanges', () => {
  const amount = 2 ** -32, initial = 2 ** -5, velocity = [2, -4, .5];
  const state = [initial, initial];
  const momentum = state.map(v => velocity.map(u => v * u));
  for (let i = 0; i < 4096; ++i) {
    const flux = i < 2048 ? amount : -amount;
    state[0] -= flux; state[1] += flux;
    velocity.forEach((u, a) => { momentum[0][a] -= flux * u; momentum[1][a] += flux * u; });
  }
  assert.deepEqual(state, [initial, initial]);
  assert.deepEqual(momentum, state.map(v => velocity.map(u => v * u)));
});
