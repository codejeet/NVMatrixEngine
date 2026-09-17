struct WhitewaterParticle {float4 positionRadius,velocityLife,previousType,surfaceAge;};
StructuredBuffer<WhitewaterParticle> WhitewaterParticles:register(t6);
[shader("intersection")]
void WhitewaterIntersection() {
    WhitewaterParticle p=WhitewaterParticles[PrimitiveIndex()];
    if(p.velocityLife.w<=0||uint(p.previousType.w)==1)return; // Foam belongs to the water coating.
    float3 q=ObjectRayOrigin()-p.positionRadius.xyz,d=ObjectRayDirection();
    float a=dot(d,d),center=-dot(q,d)/a;
    // Closest-approach form avoids subtracting two large squared distances for
    // millimetre bubbles viewed across the room.
    float3 perpendicular=q+d*center;
    float disc=(p.positionRadius.w*p.positionRadius.w-dot(perpendicular,perpendicular))/a;
    if(disc<0)return;
    float root=sqrt(disc),t=center-root;if(t<RayTMin())t=center+root;
    if(t>=RayTMin()&&t<=RayTCurrent()){FluidAttributes attrs;attrs.unused=0;ReportHit(t,0,attrs);}
}
