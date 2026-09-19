// Independent implementation of Wang et al. (2026), equations 6-19, 21, 26-29.
// Complex spectra use a 2N-square even extension of the N-square physical domain.
// The strict 2/3 Galerkin cutoff is the unpadded alternative to 3/2 padding;
// each quadratic product is projected before it participates in another product.
#define WAVE_DATA_REGISTER u1
#include "hamiltonian-shared.hlsli"
cbuffer WavePass : register(b4) { uint Dst,A,B,Op; float X,Y; uint Reserved0,Reserved1; };
RWStructuredBuffer<float2> Pool : register(u0);
RWStructuredBuffer<uint> Stats : register(u2);
#ifndef WAVE_FFT_SIZE
#define WAVE_FFT_SIZE 64
#define WAVE_FFT_LOG2 6
#endif
static const uint N=WAVE_FFT_SIZE, NN=N*N;
static const uint Eta=0,Psi=1,OldH=2,OldP=3,V=4,Px=5,Pz=6,Hx=7,Hz=8,EtaV=9,G1=10,G2=11,Dno=12;
static const uint NLH=18,NLP=19,RealH=22,RealP=23,RealPx=24,RealPz=25,RealHx=26,RealHz=27,RealV=28;
float2 at(uint slot,uint i){return Pool[slot*NN+i];}
float2 timesI(float2 v){return float2(-v.y,v.x);}
int2 mode(uint i){int2 m=int2(i%N,i/N);return int2(m.x>=int(N/2)?m.x-int(N):m.x,m.y>=int(N/2)?m.y-int(N):m.y);}
float2 waveK(uint i){return 3.141592653589793*float2(mode(i))/WaveDomain.zw;}
float g0(float k){return k*tanh(k*WavePhysics.x);}
float cutoff(uint i){return all(abs(mode(i))<int(N/3))?1:0;}
float stateFilter(uint i) {
    float2 r=abs(float2(mode(i)))/(N*.5);
    float2 f=(5+4*cos(3.141592653589793*r)-cos(6.283185307179586*r))*.125;
    return f.x*f.y*cutoff(i);
}
[numthreads(64,1,1)]void Init(uint i:SV_DispatchThreadID) {
    if(i>=NN)return;
    for(uint s=0;s<35;++s)Pool[s*NN+i]=0;
    float2 uv=(float2(i%N,i/N)+.5)/float(N/2);
    // Two reflecting standing modes; amplitude is their combined peak bound.
    Pool[RealH*NN+i]=float2(WaveCoupling.w*(.7*cos(2*3.141592653589793*uv.x)+.3*cos(3*3.141592653589793*uv.y)),0);
    if(WaveSpectrum.x>0) {
        float eta=0,psi=0,energy=0,L=WaveSpectrum.x*WaveSpectrum.x/WavePhysics.z;
        for(uint j=0;j<64;++j) {
            // Deterministic irregular mode pairs, with independent temporal
            // phases. Energy normalization keeps extra modes from flattening
            // the sea as a sum-of-amplitudes normalization would.
            uint seed=(j+1)*747796405u+2891336453u;
            seed=((seed>>((seed>>28)+4))^seed)*277803737u;seed=(seed>>22)^seed;
            float2 m=float2(1+seed%24,1+(seed>>8)%23),k=3.141592653589793*m/WaveDomain.zw;
            float km=length(k),alignment=dot(k/km,normalize(float2(.9,.4)));
            float a=exp(-.5/pow(km*L,2))*abs(alignment)/(km*km)*exp(-.5*km*km*.01);
            if(WaveSpectrum.z>0) {
                // Remove short initial waves, tapering over 1..1.5 times the
                // minimum wavelength. Normalize the retained energy below so
                // selecting coarse swells does not flatten their height.
                float cutoffK=6.283185307179586/max(.001,WaveSpectrum.z);
                a*=1-smoothstep(cutoffK/1.5,cutoffK,km);
            }
            float phase=float(seed>>16)*(6.283185307179586/65536);
            float spatial=cos(m.x*3.141592653589793*uv.x)*cos(m.y*3.141592653589793*uv.y);
            float omega=sqrt(WavePhysics.z*g0(km));
            eta+=a*cos(phase)*spatial;psi-=a*WavePhysics.z/omega*sin(phase)*spatial;energy+=a*a;
        }
        float normalization=sqrt(max(1e-12,energy*.5));
        Pool[RealH*NN+i]=float2(WaveCoupling.w*eta/normalization,0);
        Pool[RealP*NN+i]=float2(WaveCoupling.w*psi/normalization,0);
    }
    if(i<24)Stats[i]=0;
    if(i<(N/2)*(N/2)) {
        for(uint p=0;p<=WaveGrid.z+2;++p)WaveData[p*(N/2)*(N/2)+i]=0;
        Stats[64+(N/2)*(N/2)*4+i]=0;
    }
}
groupshared float2 FftValues[WAVE_FFT_SIZE];
[numthreads(WAVE_FFT_SIZE,1,1)]void FFT(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    bool inverse=(Op&1)!=0,vertical=(Op&2)!=0;
    uint row=group.x%N,component=group.x/N;
    uint index=vertical?lane*N+row:row*N+lane;
    float2 input=at(A+((Op&8)?component:0),index);
    if(Op&4) {
        uint layer=component/3,axis=component%3;
        float2 k=waveK(index);float km=length(k);
        float depth=WavePhysics.x*(1-float(layer)/float(WaveGrid.z-1));
        float e0=exp(-km*depth),e1=exp(-km*(2*WavePhysics.x-depth)),den=1+exp(-2*km*WavePhysics.x);
        input=axis==1?input*(km*(e0-e1)/den):timesI(input)*(axis==0?k.x:k.y)*(e0+e1)/den;
    }
    FftValues[reversebits(lane)>>(32-WAVE_FFT_LOG2)]=input;
    GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint length=2;length<=N;length*=2) {
        uint j=lane%(length/2),base=(lane/length)*length;
        float phase=(inverse?1:-1)*6.283185307179586*float(j)/length;
        float2 twiddle=float2(cos(phase),sin(phase));
        float2 a=FftValues[base+j],b=FftValues[base+j+length/2];
        float2 wb=float2(twiddle.x*b.x-twiddle.y*b.y,twiddle.x*b.y+twiddle.y*b.x);
        float2 value=a+((lane%length)<length/2?wb:-wb);
        GroupMemoryBarrierWithGroupSync();FftValues[lane]=value;GroupMemoryBarrierWithGroupSync();
    }
    Pool[(Dst+component)*NN+index]=FftValues[lane]*(inverse?1.0/N:1);
}
[numthreads(64,1,1)]void Operator(uint i:SV_DispatchThreadID) {
    if(i>=NN)return;float2 a=at(A,i),value=a;float2 k=waveK(i);float km=length(k),G=g0(km);
    if(Op==0)value=X*a+Y*at(B,i);
    else if(Op==1)value=a*(G*X);
    else if(Op==2)value=a*(km*km*X);
    else if(Op==3)value=timesI(a)*(k.x*X);
    else if(Op==4)value=timesI(a)*(k.y*X);
    else if(Op==5)value=G>0?a*(X/G):0; // potential gauge: psi_hat(0)=0
    else if(Op==6)value=a*cutoff(i);
    else if(Op==7)value=i!=0?a*cutoff(i):float2((int(Stats[13])-WaveMass.y)*WaveMass.x/(WaveDomain.z*WaveDomain.w)*NN,0);
    else if(Op==8) {
        float2 r=abs(float2(mode(i)))/(N*.25);
        value=a*exp(-pow(r.x,8)-pow(r.y,8))*cutoff(i);
    }else if(Op==12)value=km>0?-a/(km*km):0;
    else if(Op>=9) {
        float depth=WavePhysics.x*(1-X/float(WaveGrid.z-1));
        float e0=exp(-km*depth),e1=exp(-km*(2*WavePhysics.x-depth)),den=1+exp(-2*km*WavePhysics.x);
        float C=(e0+e1)/den,S=km*(e0-e1)/den;
        value=Op==10?a*S:timesI(a)*(Op==9?k.x:k.y)*C;
    }
    Pool[Dst*NN+i]=value;
}
[numthreads(64,1,1)]void Product(uint i:SV_DispatchThreadID) {
    if(i<NN)Pool[Dst*NN+i]=float2(at(A,i).x*at(B,i).x,0);
}
[numthreads(64,1,1)]void Bernoulli(uint i:SV_DispatchThreadID) {
    if(i>=NN)return;float e=WavePhysics.w;
    float2 dh=float2(at(RealHx,i).x,at(RealHz,i).x),dp=float2(at(RealPx,i).x,at(RealPz,i).x);
    float den=1+e*e*dot(dh,dh),W=(at(RealV,i).x+e*dot(dh,dp))/den;
    Pool[Dst*NN+i]=float2(e*(-.5*dot(dp,dp)+.5*den*W*W),0);
}
[numthreads(64,1,1)]void Integrate(uint i:SV_DispatchThreadID) {
    if(i>=NN)return;float G=g0(length(waveK(i))),g=WavePhysics.z,dt=WaveCoupling.z;
    float w=sqrt(g*G),theta=w*dt,c=cos(theta),s=w>0?sin(theta)/w:dt;
    // 1-cos(theta) evaluated without cancellation for low-frequency float32 modes.
    float omc=2*pow(sin(.5*theta),2),response=G>0?omc/G:.5*g*dt*dt;
    float2 h=at(Eta,i),p=at(Psi,i),fh=at(NLH,i),fp=at(NLP,i);
    float2 fhs=Op!=0?1.5*fh-.5*at(OldH,i):fh,fps=Op!=0?1.5*fp-.5*at(OldP,i):fp;
    float filter=stateFilter(i);
    Pool[Eta*NN+i]=(c*h+G*s*p+s*fhs+omc/g*fps)*filter;
    if(i==0)Pool[Eta*NN+i]=h;
    Pool[Psi*NN+i]=(-g*s*h+c*p-response*fhs+s*fps)*filter;
    Pool[OldH*NN+i]=fh;Pool[OldP*NN+i]=fp;
}
[numthreads(64,1,1)]void Publish(uint i:SV_DispatchThreadID) {
    uint n=WaveGrid.x;
    if(Op==2) {
        if(i>=n*n*WaveGrid.z)return;
        uint layer=i/(n*n),q=i%(n*n),j=(q/n)*N+q%n;
        WaveData[(layer+1)*n*n+q]=float4(at(A+layer*3,j).x,at(A+layer*3+1,j).x,at(A+layer*3+2,j).x,0);
        return;
    }
    if(i>=n*n)return;uint j=(i/n)*N+i%n;
    if(Op==0) {
        float4 old=WaveData[i];old.x=WavePhysics.y+at(A,j).x;old.z=at(B,j).x;WaveData[i]=old;
    }else {
        uint index=Dst*n*n+i;float4 v=WaveData[index];v[B]=at(A,j).x;WaveData[index]=v;
    }
}
[numthreads(64,1,1)]void Snapshot(uint i:SV_DispatchThreadID) {
    if(i<WaveGrid.x*WaveGrid.x){float4 v=WaveData[i];v.y=v.x;WaveData[i]=v;}
}
[numthreads(64,1,1)]void Smooth(uint i:SV_DispatchThreadID) {
    if(i>=NN)return;uint2 p=uint2(i%N,i/N);
    float2 v=at(A,i)*.5;
    v+=.125*(at(A,p.y*N+(p.x+1)%N)+at(A,p.y*N+(p.x+N-1)%N)+
             at(A,((p.y+1)%N)*N+p.x)+at(A,((p.y+N-1)%N)*N+p.x));
    Pool[Dst*NN+i]=v;
}
[numthreads(64,1,1)]void Relax(uint i:SV_DispatchThreadID) {
    if(i>=NN)return;uint2 q=uint2(i%N,i/N);q=min(q,N-1-q);
    float2 xz=WaveDomain.xy+(float2(q)+.5)/WaveGrid.x*WaveDomain.zw;
    float2 target=at(A,i);float h=Pool[Dst*NN+i].x;
    float sigma=WaveCoupling.x*waveInterior(xz)*saturate(target.y);
    float desired=target.y>1e-6?target.x/target.y:h;
    Pool[Dst*NN+i]=float2(desired+(h-desired)*exp(-sigma*WaveCoupling.z),0);
}
[numthreads(64,1,1)]void ClearFree(uint i:SV_DispatchThreadID) {
    if(i<3)Stats[i]=0; // free count, seed attempts, retired count; errors remain sticky
    if(i==7||i==8||i==9||i==11||i==16)Stats[i]=0;
    if(i==18||i==19)Stats[i]=0;
    if(i<WaveGrid.x*WaveGrid.x*4)Stats[64+i]=0;
}
[numthreads(64,1,1)]void Transfers(uint i:SV_DispatchThreadID) {
    if(i>=NN)return;
    uint n=WaveGrid.x;int2 q=int2(i%N,i/N);q=min(q,int(N)-1-q);
    // A conservative binomial footprint removes unresolved source frequencies.
    // Reflection folds edge weights back into the basin, preserving volume.
    float volume=0;float3 impulse=0;
    const float weights[5]={1,4,6,4,1};
    for(int z=-2;z<=2;++z)for(int x=-2;x<=2;++x) {
        int2 p=q+int2(x,z);p=abs(p+(p<0));p=min(p,2*int(n)-1-p);
        uint base=64+(p.y*n+p.x)*4;
        float w=weights[x+2]*weights[z+2]/256;
        volume+=w*int(Stats[base]);
        impulse+=w*float3(int(Stats[base+1]),int(Stats[base+2]),int(Stats[base+3]))*.0001;
    }
    float area=WaveDomain.z*WaveDomain.w/(n*n);
    Pool[Dst*NN+i]=float2(volume*WaveMass.x/area,0);
    Pool[A*NN+i]=float2(impulse.y*WaveMass.x/area,0);
    float2 tangent=impulse.xz*WaveMass.x/(area*WavePhysics.x);
    if(i%N>=n)tangent.x=-tangent.x;
    if(i/N>=n)tangent.y=-tangent.y;
    Pool[RealPx*NN+i]=float2(tangent.x,0);
    Pool[RealPz*NN+i]=float2(tangent.y,0);
}
groupshared float MeanHeights[64];
[numthreads(64,1,1)]void Validate(uint i:SV_DispatchThreadID) {
    if(i<64) {
        float sum=0;
        for(uint j=i;j<WaveGrid.x*WaveGrid.x;j+=64)sum+=WaveData[j].x;
        MeanHeights[i]=sum;GroupMemoryBarrierWithGroupSync();
        for(uint stride=32;stride>0;stride/=2) {
            if(i<stride)MeanHeights[i]+=MeanHeights[i+stride];
            GroupMemoryBarrierWithGroupSync();
        }
        if(i==0)Stats[10]=asuint(MeanHeights[0]/(WaveGrid.x*WaveGrid.x));
    }
    if(i<NN) {
        if(any(!isfinite(at(Eta,i)))||any(!isfinite(at(Psi,i))))InterlockedAdd(Stats[4],1);
        if(Op==1) {
            // The zero mode of surface potential is a gauge; project the
            // kinematic RHS onto the mean-free inverse-DNO range.
            if(i!=0){InterlockedMax(Stats[7],asuint(length(at(Dno,i)-at(20,i))));
                InterlockedMax(Stats[8],asuint(length(at(20,i))));}
        }
    }
    if(i<WaveGrid.x*WaveGrid.x) {
        InterlockedMax(Stats[5],asuint(abs(WaveData[i].x-WavePhysics.y)));
        for(uint layer=0;layer<WaveGrid.z;++layer) {
            float3 v=WaveData[(layer+1)*WaveGrid.x*WaveGrid.x+i].xyz;
            if(any(!isfinite(v)))InterlockedAdd(Stats[4],1);
            InterlockedMax(Stats[6],asuint(length(v)));
        }
    }
}
