import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync, statSync} from 'node:fs';
import {createHash} from 'node:crypto';

const root = new URL('./assets/neon-night/', import.meta.url);
const json = path => JSON.parse(readFileSync(new URL(path, root), 'utf8'));
const manifest = json('assets.json');
const scene = json('neon-night.gltf');

test('bundled CC0 assets match the pinned source downloads', () => {
  assert.equal(manifest.assets.length, 10);
  for (const asset of manifest.assets) {
    assert.equal(asset.license, 'CC0-1.0');
    assert.ok(asset.authors.length);
    assert.equal(new URL(asset.page).hostname, 'polyhaven.com');
  }
  for (const file of manifest.files) {
    assert.ok(file.path.startsWith('sources/') && !file.path.includes('..'));
    const data = readFileSync(new URL(file.path, root));
    assert.equal(data.byteLength, file.bytes, file.path);
    assert.equal(createHash('sha256').update(data).digest('hex'), file.sha256, file.path);
  }
});

test('the assembled scene has local, complete model and PBR dependencies', () => {
  assert.equal(scene.asset.version, '2.0');
  for (const item of [...scene.buffers, ...scene.images]) {
    assert.ok(!item.uri.includes('://') && !item.uri.includes('..'));
    const size = statSync(new URL(item.uri, root)).size;
    assert.ok(size > 0);
    if (item.byteLength) assert.equal(size, item.byteLength, item.uri);
  }
  for (const view of scene.bufferViews)
    assert.ok((view.byteOffset ?? 0) + view.byteLength <= scene.buffers[view.buffer].byteLength);
  for (const texture of scene.textures) assert.ok(scene.images[texture.source]);
  const visited = new Set();
  function visit(index, ancestors = new Set()) {
    assert.ok(!ancestors.has(index), 'node hierarchy must be acyclic');
    const node = scene.nodes[index];
    assert.ok(node);
    visited.add(index);
    if (node.mesh !== undefined) assert.ok(scene.meshes[node.mesh]);
    for (const child of node.children ?? []) visit(child, new Set([...ancestors, index]));
  }
  for (const node of scene.scenes[scene.scene].nodes) visit(node);
  assert.equal(visited.size, scene.nodes.length, 'no orphaned placed props');
  assert.ok(scene.materials.filter(m => m.normalTexture).length >= 10);
  assert.ok(scene.materials.filter(m => m.extensions?.KHR_materials_emissive_strength?.emissiveStrength > 1).length >= 5);
});
