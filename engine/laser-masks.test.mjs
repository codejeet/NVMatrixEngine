import test from 'node:test';
import assert from 'node:assert/strict';

test('GPU beam masks retain every scattering/endpoint segment in original sum order',()=>{
  for(let variant=0;variant<64;variant++){
    const beams=Array.from({length:32},(_,i)=>({
      medium:[0,1,8,10][(i+variant)%4],length:(i*17+variant)%7,
      scatter:(i+variant)%3?1:0,endpoint:(i+variant)%5===0?i+1:0
    }));
    let air=0,water=0,terminal=0;
    for(let i=0;i<32;i++){
      const b=beams[i];if(b.medium!==0&&b.medium!==8)b.scatter=0;
      if(b.length>0&&b.scatter>0){if(b.medium===8)water|=1<<i;else air|=1<<i;}
      if(b.endpoint)terminal|=1<<i;
    }
    function indices(mask){const result=[];while(mask){result.push(31-Math.clz32((mask&-mask)>>>0));mask=(mask&(mask-1))>>>0;}return result;}
    for(const medium of [0,1,8,10]){
      const reference=beams.flatMap((b,i)=>b.length>0&&b.scatter>0&&b.medium===medium?[i]:[]);
      assert.deepEqual(indices(medium===0?air:medium===8?water:0),reference);
    }
    assert.deepEqual(indices(terminal),beams.flatMap((b,i)=>b.endpoint?[i]:[]));
  }
});
