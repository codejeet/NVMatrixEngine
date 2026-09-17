// Shared camera/photon/laser dielectric contract. Include after common.hlsli
// and a Hit declaration with p, n and material. Hardware surface tests use the
// same implementation, not a second interpretation of medium entry and exit.
bool glass(Hit h) {return h.material==1||h.material==2||h.material==8||h.material==10||h.material==12||h.material==13;}
void interfaceMedia(Hit h,float3 d,float nm,out uint next,out float ni,out float nt) {
    bool entering=dot(h.n,d)<0;
    uint material=(h.material==8||h.material==13)?8:(h.material==12?12:(h.material==10?10:1));
    uint outside=h.material==12?8:((h.material==8||h.material==13)?0:ambientMedium(h.p+h.n*EPS*4));
    ni=mediumIndex(entering?outside:material,nm);nt=mediumIndex(entering?material:outside,nm);
    next=entering?material:outside;
}
