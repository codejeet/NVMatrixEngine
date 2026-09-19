#!/usr/bin/env python3
"""Rebuild the authored NOCTURNE scene. Python 3 stdlib; ImageMagick for sign artwork.
Downloaded source assets are unmodified. fetch_neon_assets.py restores them.
Geometry, layout and sign designs here are original NVMatrixEngine content.
"""
from pathlib import Path
import copy
import json
import math
import random
import struct
import zlib
import subprocess

ROOT = Path(__file__).resolve().parents[1] / 'assets/neon-night'
ROOT.mkdir(parents=True, exist_ok=True)
D = {'asset': {'version': '2.0', 'generator': 'NVMatrixEngine NOCTURNE scene builder'},
     'extensionsUsed': ['KHR_materials_emissive_strength'],
     'scene': 0, 'scenes': [{'name': 'NOCTURNE / After the rain', 'nodes': []}]}
for key in ('buffers', 'bufferViews', 'accessors', 'images', 'samplers', 'textures', 'materials', 'meshes', 'nodes'):
    D[key] = []
D['samplers'].append({'magFilter': 9729, 'minFilter': 9987, 'wrapS': 10497, 'wrapT': 10497})
BLOB = bytearray()
D['buffers'].append({'uri': 'architecture.bin', 'byteLength': 0})
GROUPS = {}
RNG = random.Random(7319)


def add(a, b): return tuple(x + y for x, y in zip(a, b))
def mul(a, s): return tuple(x * s for x in a)
def sub(a, b): return add(a, mul(b, -1))
def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def length(a): return math.sqrt(sum(x*x for x in a))
def unit(a): return mul(a, 1/length(a))


def texture(path):
    D['images'].append({'uri': path})
    D['textures'].append({'source': len(D['images'])-1, 'sampler': 0})
    return {'index': len(D['textures'])-1}


def material(name, color=(1, 1, 1), rough=.6, metal=0, source=None, normal=1, emission=None, strength=1, artwork=None):
    pbr = {'baseColorFactor': [*color, 1], 'metallicFactor': metal, 'roughnessFactor': rough}
    mat = {'name': name, 'pbrMetallicRoughness': pbr}
    if source:
        folder = ROOT / 'sources' / source
        diff = next(p for p in folder.glob('*.jpg') if '_diff' in p.name)
        pbr['baseColorTexture'] = texture(diff.relative_to(ROOT).as_posix())
        arm = texture(f'sources/{source}/{source}_arm_2k.jpg')
        pbr['metallicRoughnessTexture'] = arm
        mat['occlusionTexture'] = {**arm, 'strength': .75}
        mat['normalTexture'] = {**texture(f'sources/{source}/{source}_nor_gl_2k.jpg'), 'scale': normal}
    if emission:
        mat['emissiveFactor'] = list(emission)
        mat['extensions'] = {'KHR_materials_emissive_strength': {'emissiveStrength': strength}}
        if artwork:
            mat['emissiveTexture'] = texture(artwork)
    D['materials'].append(mat)
    return len(D['materials'])-1


ASPHALT = material('Rain-darkened scanned asphalt', (.28,.31,.34), 1, source='asphalt_02', normal=.22)
BRICK = material('Weathered brick masonry', (.53,.43,.39), .86, source='brick_wall_001', normal=.8)
CONCRETE = material('Worn damp concrete', (.45,.48,.48), .65, source='concrete_floor_worn_001', normal=.55)
PAINT = material('Midnight blue painted steel', (.025,.049,.055), .28, .55)
IRON = material('Oxidised charcoal metal', (.075,.083,.085), .38, .85)
CHROME = material('Brushed aluminium', (.5,.53,.55), .24, .95)
WOOD = material('Dark stained wood', (.075,.035,.021), .54)
GLASS = material('Dark reflective shop glass', (.012,.022,.028), .075, .55)
WATER = material('Thin still rain puddles', (.018,.027,.032), .065)
WHITE = material('Worn painted road marking', (.58,.53,.39), .56)
PINK = material('Hot pink neon tube', (.1,.005,.02), .24, emission=(1,.013,.19), strength=55)
CYAN = material('Ice cyan neon tube', (.002,.04,.05), .24, emission=(.015,.7,1), strength=40)
AMBER = material('Warm tungsten practical', (.09,.035,.009), .35, emission=(1,.39,.12), strength=2.2)
DIM = material('Distant occupied windows', (.09,.04,.012), .45, emission=(1,.49,.22), strength=.45)


def triangle(mat, pts, uv=None, normals=None):
    n = unit(cross(sub(pts[1], pts[0]), sub(pts[2], pts[0])))
    if uv is None: uv = [(0,0), (1,0), (0,1)]
    vertices = GROUPS.setdefault(mat, [])
    for i in range(3):
        p=pts[i]
        vertices.append((*p, *(normals[i] if normals else n), *uv[i], (p[0]+3.7)/7.4, (p[2]+11)/38))


def quad(mat, center, u, v, tile=(1,1)):
    # u is picture-right, v picture-up. glTF images have their origin at top-left.
    pts = [sub(sub(center,u),v), add(sub(center,v),u), add(add(center,u),v), add(sub(center,u),v)]
    uv = [(0,tile[1]),(tile[0],tile[1]),(tile[0],0),(0,0)]
    for ids in ((0,1,2),(0,2,3)): triangle(mat, [pts[i] for i in ids], [uv[i] for i in ids])


def box(mat, center, size, repeat=1):
    x,y,z = (s/2 for s in size)
    for axis, u, v in [((x,0,0),(0,0,-z),(0,y,0)),((-x,0,0),(0,0,z),(0,y,0)),
                       ((0,y,0),(x,0,0),(0,0,-z)),((0,-y,0),(x,0,0),(0,0,z)),
                       ((0,0,z),(x,0,0),(0,y,0)),((0,0,-z),(-x,0,0),(0,y,0))]:
        quad(mat,add(center,axis),u,v,(length(u)*2/repeat,length(v)*2/repeat))


def pipe(mat, a, b, radius=.025, sides=10):
    axis=unit(sub(b,a));t=unit(cross(axis,(0,1,0) if abs(axis[1])<.9 else (1,0,0)));s=cross(axis,t)
    for i in range(sides):
        n0=add(mul(t,math.cos(i*math.tau/sides)),mul(s,math.sin(i*math.tau/sides)))
        n1=add(mul(t,math.cos((i+1)*math.tau/sides)),mul(s,math.sin((i+1)*math.tau/sides)))
        p0,p1,p2,p3=add(a,mul(n0,radius)),add(a,mul(n1,radius)),add(b,mul(n1,radius)),add(b,mul(n0,radius))
        triangle(mat,[p0,p1,p2],normals=[n0,n1,n1]);triangle(mat,[p0,p2,p3],normals=[n0,n1,n0])


def sign(name, text, center, width, height, color, strength=18, vertical=False):
    artwork=f'signs/{name}.png';path=ROOT/artwork;path.parent.mkdir(exist_ok=True)
    # Render fresh high-resolution glyphs before downsampling to 4K artwork.
    # The sign itself is real emissive geometry, with a complete mip chain at import.
    label='\n'.join(text) if vertical else text
    subprocess.run(['convert','-background','black','-fill','white','-font','DejaVu-Sans',
                    '-gravity','center','-pointsize','1024','label:'+label,'-trim','+repage',
                    '-resize',f'{720 if vertical else 3800}x{3800 if vertical else 800}',
                    '-gravity','center','-extent',f'{1024 if vertical else 4096}x{4096 if vertical else 1024}',
                    str(path)],check=True)
    mat=material(name+' luminous lettering',(.009,.013,.017),.32,emission=color,strength=strength,artwork=artwork)
    x,y,z=center
    box(PAINT,(x,y,z+.065),(width+.16,height+.16,.14))
    quad(mat,(x,y,z-.011),(-width/2,0,0),(0,height/2,0))
    tube=CYAN if color[2]>color[0] else PINK
    for yy in (-height/2,height/2):pipe(tube,(x-width/2,y+yy,z-.025),(x+width/2,y+yy,z-.025),.012)
    for xx in (-width/2,width/2):pipe(tube,(x+xx,y-height/2,z-.025),(x+xx,y+height/2,z-.025),.012)


def merge_asset(ident):
    source=ROOT/'sources'/ident/(ident+'_1k.gltf');a=json.loads(source.read_text())
    offsets={key:len(D[key]) for key in ('buffers','bufferViews','accessors','images','samplers','textures','materials','meshes','nodes')}
    for b in a.get('buffers',[]):
        b['uri']='sources/'+ident+'/'+b['uri'];D['buffers'].append(b)
    for view in a.get('bufferViews',[]):view['buffer']+=offsets['buffers'];D['bufferViews'].append(view)
    for accessor in a.get('accessors',[]):accessor['bufferView']+=offsets['bufferViews'];D['accessors'].append(accessor)
    for image in a.get('images',[]):image['uri']='sources/'+ident+'/'+image['uri'];D['images'].append(image)
    D['samplers'].extend(a.get('samplers',[]))
    for tex in a.get('textures',[]):
        tex['source']+=offsets['images']
        if 'sampler' in tex:tex['sampler']+=offsets['samplers']
        D['textures'].append(tex)
    def texture_indices(obj):
        if isinstance(obj,dict):
            for key,value in obj.items():
                if key.endswith('Texture') and isinstance(value,dict) and 'index' in value:value['index']+=offsets['textures']
                else:texture_indices(value)
        elif isinstance(obj,list):
            for value in obj:texture_indices(value)
    for mat in a.get('materials',[]):texture_indices(mat);D['materials'].append(mat)
    for mesh in a.get('meshes',[]):
        for primitive in mesh['primitives']:
            primitive['attributes']={k:v+offsets['accessors'] for k,v in primitive['attributes'].items()}
            if 'indices' in primitive:primitive['indices']+=offsets['accessors']
            if 'material' in primitive:primitive['material']+=offsets['materials']
        D['meshes'].append(mesh)
    # Retain the source node transforms. Each placed prop receives fresh nodes.
    return a['nodes'], offsets['meshes']


ASSETS={ident:merge_asset(ident) for ident in ('utility_box_01','metal_trash_can','fire_hydrant',
    'exterior_aircon_unit','outdoor_table_chair_set_01','trashbag','wooden_crate_01')}


def prop(ident, pos, yaw=0, scale=1, select=None, recenter=(0,0,0)):
    source,offset=ASSETS[ident];children=[]
    for index,n in enumerate(source):
        if select is not None and index not in select:continue
        node=copy.deepcopy(n);node['mesh']+=offset
        node['translation']=list(add(node.get('translation',(0,0,0)),recenter))
        children.append(len(D['nodes']));D['nodes'].append(node)
    radians=math.radians(yaw)
    D['scenes'][0]['nodes'].append(len(D['nodes']))
    D['nodes'].append({'name':ident+' placed','children':children,'translation':list(pos),
                       'rotation':[0,math.sin(radians/2),0,math.cos(radians/2)],'scale':[scale]*3})


# A narrow service street with raised footpaths, recessed shop bays and upper floors.
box(ASPHALT,(0,-.13,8),(7.4,.26,38),3.2)
for x in (-3.18,3.18):
    box(CONCRETE,(x,.065,8),(.72,.13,38),2)
    for z in range(-10,28,2):box(IRON,(x,.135,z),(.72,.005,.014))
for side in (-1,1):
    x=side*3.65
    box(BRICK,(x,7.6,9),(.4,9.0,38),2.5)
    for z in (-10,-5,0,5,10,15,20,25):
        box(BRICK,(x,1.55,z),(.42,3.1,.6),2.5)
    for y in (.3,3.15,6.4,9.65,12):box(CONCRETE,(side*3.43,y,9),(.13,.12,38),2)
    for z in (-7.5,-2.5,2.5,7.5,12.5,17.5,22.5):
        # The original masonry is behind a reflective storefront insert.
        box(PAINT,(side*3.57,1.55,z),(.18,2.85,4.3))
        box(GLASS,(side*3.455,1.56,z),(.022,2.48,3.94))
        for dz in (-1.98,-.65,.65,1.98):box(IRON,(side*3.427,1.53,z+dz),(.07,2.72,.055))
        for yy in (.31,2.05,2.82):box(IRON,(side*3.423,yy,z),(.07,.055,4))
        # Warm translucent shade panels in selected shop windows.
        if (int(z*2)+side)%3:
            box(AMBER,(side*3.412,2.38,z),(.012,.65,3.84))
            for dz in (-1.3,0,1.3):box(IRON,(side*3.39,2.38,z+dz),(.035,.68,.045))
        # Upper-floor sash windows, lintels, sill drainage and mullions.
        for floor in range(2):
            yy=4.75+floor*3.2
            box(CONCRETE,(side*3.42,yy-.95,z),(.23,.15,1.75),1.5)
            box(PAINT,(side*3.40,yy,z),(.13,1.95,1.55))
            lit=RNG.random()<.28
            box(DIM if lit else GLASS,(side*3.317,yy,z),(.02,1.7,1.32))
            box(IRON,(side*3.29,yy,z),(.055,1.8,.035))
            box(IRON,(side*3.29,yy,z),(.055,.045,1.4))
            if lit:
                for slat in range(8):box(WOOD,(side*3.283,yy-.73+slat*.205,z),(.035,.045,1.32))
    for z in (-5,8,20):
        pipe(IRON,(side*3.28,.15,z),(side*3.28,11.8,z),.065)
        for yy in (1,3,5,7,9,11):box(CHROME,(side*3.275,yy,z),(.14,.06,.15))
    for z,yy in ((-2,3.65),(7,3.6),(15,7)):
        # Scanned metal fins, dirt, fasteners and alpha-cut fan guards.
        prop('exterior_aircon_unit',(side*3.10,yy,z),-side*90,select=[1],recenter=(-.5,0,0))
        box(IRON,(side*3.05,yy-.37,z),(.7,.05,.96))

# Focal bar at the end of the alley. The lit frontage keeps depth readable.
box(BRICK,(0,6.1,24),(7.5,12.2,.5),2.5)
box(PAINT,(0,1.7,23.65),(6.0,3.4,.20))
box(GLASS,(0,1.55,23.52),(5.7,2.95,.025))
for x in (-2.9,-1,1,2.9):box(CHROME,(x,1.55,23.47),(.045,3,.055))
box(AMBER,(0,2.6,23.45),(5.6,.67,.02))
for x in (-2.5,0,2.5):
    for y in (5.6,8.8):
        box(PAINT,(x,y,23.65),(1.5,1.9,.15))
        box(DIM if x!=0 else GLASS,(x,y,23.55),(1.25,1.65,.025))
        box(IRON,(x,y,23.51),(.05,1.72,.04))
        if x!=0:
            for slat in range(8):box(WOOD,(x,y-.72+slat*.20,23.50),(1.25,.04,.03))
sign('nocturne','NOCTURNE',(0,3.65,23.38),5.7,.85,(.02,.65,1),26)
sign('motel','MOTEL',(2.55,4.25,1.2),.78,3.2,(.02,.65,1),24,True)
sign('ramen','RAMEN',(-2.58,3.8,6.6),.74,2.75,(1,.01,.15),30,True)
sign('bar','BAR',(2.45,3.35,14.3),1.45,.64,(1,.03,.23),20)
sign('open','OPEN',(-2.32,1.78,9.2),1.10,.38,(1,.04,.12),12)
for side,z,top in ((1,1.2,5.9),(-1,6.6,5.25),(1,14.3,3.8)):
    pipe(IRON,(side*3.5,top,z+.09),(side*2.25,top,z+.09),.035)
    pipe(IRON,(side*3.5,top+.5,z+.09),(side*2.8,top,z+.09),.026)
# Cyan and magenta strips under two storefront awnings.
for side,z,mat in ((1,1,CYAN),(-1,7,PINK),(-1,18,CYAN)):
    box(PAINT,(side*3.12,2.96,z),(.87,.13,3.7))
    pipe(mat,(side*2.73,2.88,z-1.78),(side*2.73,2.88,z+1.78),.018)

# Street clutter, with measured model scale and the source variants selected explicitly.
prop('outdoor_table_chair_set_01',(2.47,.15,5.1),5,.97)
prop('outdoor_table_chair_set_01',(-2.45,.15,11.8),-9,.97)
prop('metal_trash_can',(-2.55,.14,1.4),15,select=[4,5,6,7],recenter=(.5,0,0))
prop('metal_trash_can',(2.64,.14,16.7),-22,select=[0,1,2,3],recenter=(-.5,0,0))
prop('fire_hydrant',(2.47,.14,-1.9),-20,select=[5,6,7,8,9],recenter=(-.3,0,0))
prop('utility_box_01',(-3.08,.16,3.4),90)
prop('utility_box_01',(3.05,.16,11.3),-90)
for pos,yaw in [((-2.59,.15,2.15),34),((-2.10,.02,1.9),-41),((2.48,.15,17.5),-5),((2.7,.15,18),32)]:
    prop('trashbag',pos,yaw,1.1)
for pos,yaw in [((2.55,.15,8),-4),((2.53,.49,8),9),((-2.5,.15,14.7),17)]:
    prop('wooden_crate_01',pos,yaw)
# Bollards, a drain grating, worn lane paint, overhead service cables.
for x,z in [(-2.25,-3),(2.25,-3),(-2.25,20),(2.25,20)]:
    pipe(IRON,(x,.0,z),(x,.73,z),.058)
    pipe(CHROME,(x,.58,z),(x,.64,z),.060)
for z in (-3,8,18):
    box(IRON,(-2.72,.012,z),(.33,.024,1.0))
    for dz in range(10):box(CONCRETE,(-2.72,.027,z-.44+dz*.098),(.29,.008,.032))
for z in (-5,0,5,10,15,20):
    for x in (-2.62,2.62):box(WHITE,(x,.005,z),(.035,.008,1.8))
for z in (3,11,20):
    points=[(-3.5+i*.35,6.7-.6*math.sin(i/20*math.pi),z+.12*math.sin(i*.35)) for i in range(21)]
    for a,b in zip(points,points[1:]):pipe(IRON,a,b,.012,6)
    for i in (4,9,14,18):
        a=points[i];pipe(IRON,a,add(a,(0,-.16,0)),.018)
        box(AMBER,add(a,(0,-.20,0)),(.055,.08,.055))

# A continuous wetness/roughness field avoids disconnected, black puddle edges.
# UV1 covers the entire road; the scanned albedo and normal still tile on UV0.
def wet_roughness():
    width,height=512,2048
    pools=[(-.6,-3.2,1.3,1.65),(1.25,0,1.,2.3),(-1.3,4.9,1.1,2.),(.1,9.5,1.6,1.8),
           (1.6,13,.6,2.8),(-1.5,17,.8,2.),(0,21,1.4,1.6)]
    raw=bytearray()
    for j in range(height):
        raw.append(0)
        z=j/(height-1)*38-11
        for i in range(width):
            x=i/(width-1)*7.4-3.7
            warp=.11*math.sin(x*9+z*2)+.08*math.sin(z*7-x*3)
            wet=0
            for px,pz,rx,rz in pools:
                if abs(z-pz)>rz*1.4:continue
                r=math.sqrt(((x-px)/rx)**2+((z-pz)/rz)**2)+warp
                t=max(0,min(1,(1.15-r)/.40))
                wet=max(wet,t*t*(3-2*t))
            grain=.018*math.sin(x*280+z*173)*math.sin(z*213-x*139)
            rough=.44+.10*math.sin(x*2+z*.7)*math.sin(z*3-x*.3)
            rough=rough*(1-wet)+.105*wet+grain
            raw.extend((255,round(max(.06,min(1,rough))*255),0))
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    encoded=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(bytes(raw),9))+chunk(b'IEND',b'')
    (ROOT/'wet-asphalt-arm.png').write_bytes(encoded)
    D['materials'][ASPHALT]['pbrMetallicRoughness']['metallicRoughnessTexture']={**texture('wet-asphalt-arm.png'),'texCoord':1}
wet_roughness()

# Original mesh buffers are deterministic, grouped by material to keep BLAS count low.
def accessor(values, width, bounds=False):
    while len(BLOB)%4:BLOB.append(0)
    offset=len(BLOB)
    for value in values:BLOB.extend(struct.pack('<'+'f'*width,*value))
    view=len(D['bufferViews']);D['bufferViews'].append({'buffer':0,'byteOffset':offset,'byteLength':len(BLOB)-offset,'target':34962})
    item={'bufferView':view,'componentType':5126,'count':len(values),'type':{2:'VEC2',3:'VEC3'}[width]}
    if bounds:item.update(min=[min(v[i] for v in values) for i in range(width)],max=[max(v[i] for v in values) for i in range(width)])
    D['accessors'].append(item);return len(D['accessors'])-1

for mat,verts in GROUPS.items():
    attrs={'POSITION':accessor([v[:3] for v in verts],3,True),
           'NORMAL':accessor([v[3:6] for v in verts],3),'TEXCOORD_0':accessor([v[6:8] for v in verts],2),
           'TEXCOORD_1':accessor([v[8:10] for v in verts],2)}
    mesh=len(D['meshes']);D['meshes'].append({'name':D['materials'][mat]['name'],'primitives':[{'attributes':attrs,'material':mat}]})
    D['scenes'][0]['nodes'].append(len(D['nodes']));D['nodes'].append({'mesh':mesh,'name':D['materials'][mat]['name']})
D['buffers'][0]['byteLength']=len(BLOB)
(ROOT/'architecture.bin').write_bytes(BLOB)
(ROOT/'neon-night.gltf').write_text(json.dumps(D,separators=(',',':'))+'\n')
print(f'NOCTURNE: {len(D["nodes"])} nodes, {len(D["materials"])} materials, {len(D["images"])} images; {len(BLOB):,} authored geometry bytes')
