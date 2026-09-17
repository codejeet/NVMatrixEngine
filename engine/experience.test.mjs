import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const read=p=>readFileSync(new URL(p,import.meta.url),'utf8');
test('default lens and UI use a 90 degree diagonal field of view in both lens modes',()=>{
  const lens=read('src/experience.h');
  assert.match(lens,/diagonalDegrees = 90/);
  assert.match(read('ui/lab.rml'),/id="lens-fov"[^>]*value="90"/);
  const projection=lens.slice(lens.indexOf('float tanHalfVertical'),lens.indexOf('void rectilinear'));
  assert.doesNotMatch(projection,/return fisheye \?|\.577350/);
  assert.match(projection,/std::clamp\(diagonalDegrees/);
});
test('ball density control connects the menu to live physics without resetting water',()=>{
  assert.match(read('ui/lab.rml'),/id="ball-density" data-action="ball-density"/);
  assert.match(read('src/experience.h'),/ballFloats = false/);
  assert.match(read('src/hud.cpp'),/action == "ball-density" && fluidRoom && game.boatBody >= 0/);
  assert.match(read('src/main.cpp'),/game->setBallFloating\(renderer.experience.ballFloats\)/);
  const game=read('../shared/src/game.cpp');
  const setter=game.slice(game.indexOf('void Game::setBallFloating'),game.indexOf('bool Game::nearBoat'));
  assert.match(setter,/setMassProps\(mass, inertia\)/);
  assert.match(setter,/updateInertiaTensor/);
  assert.doesNotMatch(setter,/setWorldTransform|setLinearVelocity|setAngularVelocity|resetHistory =/);
  assert.match(game,/boat->applyCentralForce\(\{0, -ballMass\(\) \* waterGravity, 0\}\)/);
});
test('flashlight cone integrates to its radiant flux and photon partitions preserve budget',()=>{
  const cosine=Math.cos(24*Math.PI/180), power=8;
  const intensity=power/(2*Math.PI*(1-cosine));
  assert.ok(Math.abs(intensity*2*Math.PI*(1-cosine)-power)<1e-12);
  for(const budget of [1024,65536]) for(const environment of [0,1,2,3]) for(const flashlight of [false,true]) {
    const n4=flashlight?(environment===3?budget:budget/4):0, left=budget-n4;
    const lasers=environment===0;
    const n1=lasers?left/8:0,n2=n1,n3=environment?left:left/(lasers?4:2),n0=left-n1-n2-n3;
    assert.ok([n0,n1,n2,n3,n4].every(n=>n>=0));
    assert.equal(n0+n1+n2+n3+n4,budget);
  }
});
test('lens presentation, picking, temporal projection and FG gate share the model',()=>{
  const renderer=read('src/renderer.cpp'),present=read('shaders/present.hlsl'), lens=read('src/experience.h');
  assert.match(present,/fisheyeToRectilinearUv/);assert.match(lens,/lensMapping::fisheyeToRectilinearScale/);
  assert.match(renderer,/displayedLens.rectilinear/);
  assert.match(renderer,/lens.fisheye \? fgDistortion.Get\(\) : nullptr/);
  assert.match(read('src/streamline.cpp'),/cam.projection._22/);
  assert.match(read('src/main.cpp'),/activeRenderer->displayRay/);
});
test('water quality and body coupling use existing GPU residency/fence boundaries',()=>{
  const renderer=read('src/renderer.cpp'),buoyancy=read('src/fluid/fluid_buoyancy.cpp');
  assert.match(renderer,/fluidDesc.gridCellSize = options.fluidCellSize/);
  assert.match(renderer,/fluidDesc.simulationRate = options.fluidSimulationHz/);
  assert.match(renderer,/buoyancy->record/);assert.match(renderer,/game->receiveWater/);
  assert.ok(!buoyancy.includes('WaitFor'));assert.ok(!buoyancy.includes('particles.resource->Map'));
  assert.match(read('shaders/fluid/buoyancy.hlsl'),/liquidSample/);
  assert.match(read('shaders/fluid/buoyancy.hlsl'),/Faces\[/);
  assert.match(read('shaders/fluid/buoyancy.hlsl'),/bool connected=false/);
  assert.match(read('../shared/src/game.cpp'),/getInvInertiaTensorWorld\(\) \* lever/);
});
test('hydrostatic head ignores a detached splash above the pool',()=>{
  const phi=y=>Math.min(Math.max(.04-y,y-.34),Math.max(1.6-y,y-1.85));
  const step=.02;let connected=false,height=-1e6,previous=.02;
  for(let y=.04;y<=3;y+=step){
    if(!connected){if(y>.14)break;if(phi(y)<=0&&phi(y+step)<=0)connected=true;}
    else if(phi(y)>0){let wet=previous,air=y;for(let i=0;i<6;i++){const mid=(wet+air)/2;if(phi(mid)<=0)wet=mid;else air=mid;}height=(wet+air)/2;break;}
    previous=y;
  }
  assert.ok(Math.abs(height-.34)<.001);
});
