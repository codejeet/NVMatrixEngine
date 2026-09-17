// Time integration of the existing piecewise-linear spatial face model.
// Temporal SDF interpolation is an approximation to rigid motion, not an exact
// swept solid. Split at vertex sign changes before four-point Gauss quadrature.
float cutTimeFace(float4 before,float4 after){
    if(all(before>=0)&&all(after>=0))return 1;
    if(all(before<0)&&all(after<0))return 0;
    if(all(before==after))return .5*(cutTriangle(before.xyw)+cutTriangle(before.xzw));
    float points[6];points[0]=0;points[1]=1;uint count=2;
    [unroll]for(uint i=0;i<4;++i){
        if((before[i]<0)!=(after[i]<0)){
            float t=before[i]/(before[i]-after[i]);
            if(t>0&&t<1)points[count++]=t;
        }
    }
    for(uint i=1;i<count;++i){float t=points[i];uint j=i;while(j&&points[j-1]>t){points[j]=points[j-1];--j;}points[j]=t;}
    static const float nodes[4]={.0694318442,.3300094782,.6699905218,.9305681558};
    static const float weights[4]={.1739274226,.3260725774,.3260725774,.1739274226};
    float area=0;
    for(uint s=1;s<count;++s){float dt=points[s]-points[s-1];
        [unroll]for(uint k=0;k<4;++k){float4 p=lerp(before,after,points[s-1]+dt*nodes[k]);
            area+=dt*weights[k]*.5*(cutTriangle(p.xyw)+cutTriangle(p.xzw));}
    }
    return area;
}
