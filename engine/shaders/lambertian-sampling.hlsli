// Scalar estimator policy, also compiled directly by tests/lambertian.cpp.
// Inclusion is decided before visibility. Never cull a nonzero contribution:
// even an occluded dominant light cannot starve a weaker visible light.
float lambertianProbability(float weight,float total,uint candidates,float budget,uint reference) {
    if(weight<=0)return 0;
    if(reference!=0||candidates<=budget)return 1;
    float p=(weight/total)*float(budget);
    return p<.5f?.5f:(p>1?1:p);
}
// Systematic sampling has the same marginal inclusion probabilities as
// independent roulette, but traces floor(sum(p)) or ceil(sum(p)) shadow rays.
bool lambertianSelected(float probability,float offset) { return offset<probability; }
float lambertianNextOffset(float probability,float offset) {
    float next=offset-probability;
    return next<0?next+1:next;
}
