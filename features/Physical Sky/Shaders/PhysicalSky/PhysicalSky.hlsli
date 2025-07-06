#include "Common/Color.hlsli"

namespace PhysicalSky
{
const static float rEarth = 6371; // in km
// effective ratio of rEarth / layer height
const static float rOzoneRel = 125; 
const static float rRayleighRel = 500;
const static float rMieRel = 1500;
// max relative air mass cap at twilight
const static float maOzoneCap = 25; 
const static float maRayleighCap= 75;
const static float maMieCap = 200;
// extincition coeffs per rel air mass
const static float3 rouOzonePerDu = float3(5e-5, 5e-5, 5e-6);
const static float3 rouRayleigh = float3(0.04, 0.09, 0.25);
const static float rouMieGreen = 0.06;

float RelativeAirMass(float theta, float rRel){
    float rTemp = rRel * theta;
    return sqrt(rTemp * rTemp + 2 * rRel + 1) - rTemp;
}

float3 DirlightOzlem(float3 viewDir, SharedData::PhysicalSkyPerDirLight lightData)
{
    SharedData::PhysicalSkyData data = SharedData::PhysicalSkyData;

    // Zenith angles
    float cosView = viewDir.z;
    float sinView = sqrt(1 - cosView * cosView);
    // float cosHorDownshift = rEarth / (rEarth + altitude);
    if (cosView < 0 && sinView < data.cosHorDownshift)
        return 0;
    float azimuthView = atan2(viewDir.y, viewDir.x);

    float cosSun = lightData.lightAngles.y;
    float sinSun = sqrt(1 - cosSun * cosSun);
    float azimuthSun = lightData.lightAngles.x;

    // tangent height
    float hTanView = data.altitude + (cosView > 0 ? 0 : rEarth - rEarth / sinView);
    // float hTanSun = altitude + (cosSun > 0 ? 0 : rEarth - rEarth / sinSun);

    // altitudal decay of participants
    // float altDecayOzone = exp(-altitude / hScaleOzone);
    // float altDecayRayleigh = exp(-altitude / hScaleRayleigh);
    // float altDecayMie = exp(-altitude / hScaleMie);
    
    // relative air masses
    float amViewRayleigh = min(RelativeAirMass(cosView, rRayleighRel), maRayleighCap) * data.altDecayRayleigh;
    float amViewMie = min(RelativeAirMass(cosView, rMieRel), maMieCap) * data.altDecayMie;
    // float amSunOzone = min(RelativeAirMass(cosSun, rOzoneRel), maOzoneCap) * altDecayOzone;
    // float amSunRayleigh = min(RelativeAirMass(cosSun, rRayleighRel), maRayleighCap) * altDecayRayleigh;
    // float amSunMie = min(RelativeAirMass(cosSun, rMieRel), maMieCap) * altDecayMie;

    // refraction corrected incidence
    float refrZenithView = acos(cosView) + radians(amViewRayleigh / 57);
    float2 refrSinCos;
    sincos(refrZenithView, refrSinCos.x, refrSinCos.y);
    float refrU = sinSun * refrSinCos.x * cos(azimuthView - azimuthSun) + cosSun * refrSinCos.y;

    // extinction coeffs
    // float3 rouOzone = data.ozoneDu * rouOzonePerDu;
    // float3 rouMie = rouMieGreen * (data.turbidity - 2 + 1 / data.turbidity);
    // float rouMieAltCorrected = rouMie.g * altDecayMie;
    // float msDegrader = 0.94 * exp(-rouMieAltCorrected);
    // float rouMieSkew = 0.9 - 0.1 * msDegrader;
    // rouMie *= float3(rouMieSkew, 1, rcp(rouMieSkew));

    // optical depth
    float3 tauViewRayleigh = rouRayleigh * amViewRayleigh;
    float3 tauViewMie = data.rouMie * amViewMie;
    // float3 tauSunOzone = rouOzone * amSunOzone;
    // float3 tauSunRayleigh = rouRayleigh * amSunRayleigh;
    // float3 tauSunMie = rouMie * amSunMie;

    // multi scatter corrected transmittance
    float3 trRayleigh = 2 / (2 + sqrt(tauViewRayleigh * lightData.tauSunRayleigh));
    float3 trMie = exp(-sqrt(tauViewMie * lightData.tauSunMie) / 6);
    // single scatter transmittance around sun disc
    float3 trAureole = exp(-lightData.tauSunOzone - tauViewRayleigh - tauViewMie);
    float3 trSun =  exp(-lightData.tauSunOzone - lightData.tauSunRayleigh - lightData.tauSunMie);

    // twilight coeffs
    // float twilightVertScale = 0.03 * max(-hTanSun, 0);
    // float twilightDarkness = 0.03 * twilightVertScale * twilightVertScale;
    // // float twilightLum = exp(0.3 - 0.05 * amSunRayleigh - 1.7 * twilightVertScale) + 1e-5; // for absolute luminance
    float3 twilightScatterRatio = (1 - exp(-tauViewRayleigh - tauViewMie)) / (7 * tauViewRayleigh + tauViewMie) * 
        exp(-lightData.tauSunOzone - lightData.twilightVertScale / amViewRayleigh - lightData.twilightDarkness);
    float deepTwilightCutoff = 0.002 * data.altDecayRayleigh;

    // venus belt
    float zenithSun = acos(cosSun);
    // float horDownshift = acos(cosHorDownshift);
    float venusBeltShadowThres = radians(89) - data.horDownshift - zenithSun;
    float venusBeltHorScaleCoeff = 1;
    if (venusBeltShadowThres < 0)
    {
        float venusBeltAltCorrection = (radians(2) + data.horDownshift) / radians(120);
        float venusBeltVertScaleCoeff = venusBeltAltCorrection * sin(radians(91) + data.horDownshift - acos(cosView)) / (1 - cos(venusBeltShadowThres));
        venusBeltHorScaleCoeff = 1 + 1 / max(venusBeltVertScaleCoeff + refrU, 0);
    }
    
    // indicatrix
    // float anisoMie = msDegrader * exp(-0.02 * amSunMie);
    float phaseCommon = 1 + (refrU >= 0 ? data.msDegrader : lightData.anisoMie) * refrU * refrU;

    // float cRayleigh = 3 / (3 + msDegrader);
    float3 fRayleigh = 7 * data.cRayleigh * tauViewRayleigh * trRayleigh * twilightScatterRatio;

    // float g2 = anisoMie * anisoMie;
    // float cMie = 3 * (1 - g2) / (3 + msDegrader + 2 * msDegrader * g2);
    float pMie = pow(1 + lightData.g2 - 2 * lightData.anisoMie * refrU, -1.5); // multi scatter corrected cornette-shanks
    float3 fMie = 0.9 * lightData.cMie * pMie * tauViewMie * trMie * twilightScatterRatio;
    
    float3 fAureole = trAureole * (refrU <= 0.99999 ? data.rouMieAltCorrected * lightData.anisoMie / 10 / (1 - refrU) : 7.2e9 * refrU - 7.19988e9);

    const static float3 fRayleighZenith = 0; //  TODO
    float3 f = phaseCommon * (max(max(fRayleigh/ venusBeltHorScaleCoeff, fRayleighZenith), deepTwilightCutoff) + fMie + fAureole) ;
    f *= float3((f.r / f.g - 1) * data.vividness + 1, 1, (f.b / f.g - 1) * data.vividness + 1);
    f *= lightData.lightColor;

    return f;
}

float3 SkyOzlem(float3 viewDir)
{
    SharedData::PhysicalSkyData data = SharedData::PhysicalSkyData;
    float altitude = data.altitude;

    // Many of the commented lines below are ones that can be calculated on CPU
    // They were left here to make it easier to understand the algorithm
    float3 fSun = DirlightOzlem(viewDir, data.sunData);
    
    float3 f = fSun;
    if (data.tonemapper == 1)
        f = Color::LinearToGamma(f);
    else
        f = f / (1 + f);
    
    return f;
}
}
