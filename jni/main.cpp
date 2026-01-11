#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <string.h>

MYMOD(net.ssao.brutal3, SSAO_GLES3_Brutal, 1.0, ntyzsky)
NEEDGAME(com.rockstargames.gtasa)

#define SHADER_LEN 131072

uintptr_t pGTASA = 0;
void* hGTASA = NULL;

uint32_t* m_snTimeInMilliseconds;
uint32_t* curShaderStateFlags;
float* UnderWaterness;

typedef int (*glGetUniformLocation_t)(int, const char*);
typedef void (*glUniform1i_t)(int, int);
typedef void (*glUniform1fv_t)(int, int, const float*);
typedef void (*glUniform2fv_t)(int, int, const float*);

glGetUniformLocation_t _glGetUniformLocation;
glUniform1i_t _glUniform1i;
glUniform1fv_t _glUniform1fv;
glUniform2fv_t _glUniform2fv;

const char* ssaoVertexShader = R"(
#version 300 es
precision highp float;

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aTexCoord;

out vec2 vTexCoord;
out vec2 vPixPos;

void main() {
    gl_Position = vec4(aPosition.xy, 0.0, 1.0);
    vTexCoord = aTexCoord;
    vPixPos = aPosition.xy * 0.5 + 0.5;
}
)";

const char* ssaoFragmentShader = R"(
#version 300 es
precision highp float;

in vec2 vTexCoord;
in vec2 vPixPos;

uniform sampler2D uSceneTex;
uniform vec2 uPixelSize;
uniform float uSampleRadius;
uniform int uSampleCount;
uniform float uIntensity;
uniform float uNormalBias;
uniform float uTime;
uniform float uFadeoutStart;
uniform float uFadeoutEnd;

out vec4 fragColor;

const float PI = 3.14159265359;
const float TAU = 6.28318530718;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float getBayerDither(vec2 pixelPos, int level) {
    float finalBayer = 0.0;
    
    for(int i = 1 - level; i <= 0; i++) {
        float bayerSize = exp2(float(i));
        vec2 bayerCoord = mod(floor(pixelPos * bayerSize), 2.0);
        float bayer = 2.0 * bayerCoord.x - 4.0 * bayerCoord.x * bayerCoord.y + 3.0 * bayerCoord.y;
        finalBayer += exp2(2.0 * float(i + level)) * bayer;
    }
    
    float finalDivisor = 4.0 * exp2(2.0 * float(level)) - 4.0;
    return finalBayer / finalDivisor + 1.0 / exp2(2.0 * float(level));
}

float getDepth(vec2 uv) {
    vec3 col = texture(uSceneTex, uv).rgb;
    float luma = dot(col, vec3(0.299, 0.587, 0.114));
    
    vec3 colR = texture(uSceneTex, uv + vec2(uPixelSize.x, 0.0)).rgb;
    vec3 colL = texture(uSceneTex, uv - vec2(uPixelSize.x, 0.0)).rgb;
    vec3 colU = texture(uSceneTex, uv + vec2(0.0, uPixelSize.y)).rgb;
    vec3 colD = texture(uSceneTex, uv - vec2(0.0, uPixelSize.y)).rgb;
    
    float lumaR = dot(colR, vec3(0.299, 0.587, 0.114));
    float lumaL = dot(colL, vec3(0.299, 0.587, 0.114));
    float lumaU = dot(colU, vec3(0.299, 0.587, 0.114));
    float lumaD = dot(colD, vec3(0.299, 0.587, 0.114));
    
    float edgeH = abs(lumaR - lumaL);
    float edgeV = abs(lumaU - lumaD);
    float edge = max(edgeH, edgeV);
    
    return luma * (1.0 + edge * 2.0);
}

vec3 getNormal(vec2 uv, float centerDepth) {
    vec3 offs = vec3(uPixelSize, 0.0);
    
    float depthR = getDepth(uv + offs.xz);
    float depthL = getDepth(uv - offs.xz);
    float depthU = getDepth(uv + offs.zy);
    float depthD = getDepth(uv - offs.zy);
    
    vec3 dx = vec3(2.0 * offs.x, 0.0, depthR - depthL);
    vec3 dy = vec3(0.0, 2.0 * offs.y, depthU - depthD);
    
    return normalize(cross(dx, dy));
}

void main() {
    vec3 sceneColor = texture(uSceneTex, vTexCoord).rgb;
    float centerDepth = getDepth(vTexCoord);
    
    vec3 normal = getNormal(vTexCoord, centerDepth);
    
    float radiusJitter = getBayerDither(vPixPos * vec2(1920.0, 1080.0), 2);
    float angleOffset = radiusJitter * TAU + uTime * 0.0001;
    
    float ao = 0.0;
    float radius = uSampleRadius;
    int samples = uSampleCount;
    
    const float goldenAngle = 2.399963;
    vec2 currentVector;
    currentVector.x = cos(angleOffset);
    currentVector.y = sin(angleOffset);
    
    float negInvR2 = -1.0 / (radius * radius);
    
    for(int i = 0; i < samples; i++) {
        float angle = float(i) * goldenAngle + angleOffset;
        float dist = sqrt(float(i) / float(samples)) * radius;
        
        vec2 offset = vec2(cos(angle), sin(angle)) * dist * uPixelSize;
        vec2 sampleUV = vTexCoord + offset;
        
        float sampleDepth = getDepth(sampleUV);
        
        float depthDiff = centerDepth - sampleDepth;
        
        float distWeight = 1.0 - smoothstep(0.0, radius, length(offset / uPixelSize));
        
        vec3 sampleDir = normalize(vec3(offset, depthDiff));
        float normalWeight = max(0.0, dot(normal, sampleDir) - uNormalBias);
        
        float occlusion = max(0.0, depthDiff) * distWeight * normalWeight;
        occlusion *= clamp(1.0 + negInvR2 / max(0.001, abs(depthDiff)), 0.0, 1.0);
        
        ao += occlusion;
    }
    
    ao = clamp(ao / (0.4 * (1.0 - uNormalBias) * float(samples) * sqrt(radius)), 0.0, 1.0);
    
    ao = pow(ao, 1.0 / 2.2);
    
    ao = 1.0 - pow(1.0 - ao, uIntensity * 4.0);
    
    float fadeout = smoothstep(uFadeoutStart, uFadeoutEnd, centerDepth);
    ao = mix(ao, 0.0, fadeout);
    
    float occlusion = 1.0 - ao;
    
    vec2 vignette = abs(vTexCoord - 0.5) * 2.0;
    float vignetteAmount = 1.0 - pow(max(vignette.x, vignette.y), 3.0) * 0.3;
    
    fragColor = vec4(sceneColor * occlusion * vignetteAmount, 1.0);
}
)";

char customFragShader[SHADER_LEN];
char customVertShader[SHADER_LEN];
bool bInjected = false;

struct ES2ShaderEx {
    char base[512];
    int uid_uSceneTex;
    int uid_uPixelSize;
    int uid_uSampleRadius;
    int uid_uSampleCount;
    int uid_uIntensity;
    int uid_uNormalBias;
    int uid_uTime;
    int uid_uFadeoutStart;
    int uid_uFadeoutEnd;
};

DECL_HOOK(int, RQShaderBuildSource, int flags, char** pxlsrc, char** vtxsrc) {
    int ret = RQShaderBuildSource(flags, pxlsrc, vtxsrc);
    
    static int logCount = 0;
    static bool loggedFlags[4096] = {false};
    
    if(logCount < 50 && !loggedFlags[flags % 4096]) {
        logger->Info("Shader flag: 0x%X", flags);
        loggedFlags[flags % 4096] = true;
        logCount++;
    }
    
    bool isBuilding = (flags == 0x10020430 || flags == 0x12020430 || 
                       flags == 0x10220432 || flags == 0x10222432 ||
                       flags == 0x10100430 || flags == 0x10120430 ||
                       flags == 0x10110430 || flags == 0x10130430 ||
                       flags == 0x1010042A || flags == 0x1012042A ||
                       flags == 0x1013042A || flags == 0x1011042A ||
                       flags == 0x1092042A);
    
    if(!bInjected && isBuilding) {
        logger->Info("BRUTAL SSAO INJECTION at building shader flag 0x%X", flags);
        
        strncpy(customFragShader, ssaoFragmentShader, SHADER_LEN - 1);
        customFragShader[SHADER_LEN - 1] = '\0';
        *pxlsrc = customFragShader;
        
        strncpy(customVertShader, ssaoVertexShader, SHADER_LEN - 1);
        customVertShader[SHADER_LEN - 1] = '\0';
        *vtxsrc = customVertShader;
        
        bInjected = true;
    }
    
    return ret;
}

DECL_HOOKv(ES2Shader_Select, ES2ShaderEx* self) {
    ES2Shader_Select(self);
    
    if(self->uid_uPixelSize == -1 && _glGetUniformLocation) {
        int shaderId = *(int*)self;
        
        self->uid_uSceneTex = _glGetUniformLocation(shaderId, "uSceneTex");
        self->uid_uPixelSize = _glGetUniformLocation(shaderId, "uPixelSize");
        self->uid_uSampleRadius = _glGetUniformLocation(shaderId, "uSampleRadius");
        self->uid_uSampleCount = _glGetUniformLocation(shaderId, "uSampleCount");
        self->uid_uIntensity = _glGetUniformLocation(shaderId, "uIntensity");
        self->uid_uNormalBias = _glGetUniformLocation(shaderId, "uNormalBias");
        self->uid_uTime = _glGetUniformLocation(shaderId, "uTime");
        self->uid_uFadeoutStart = _glGetUniformLocation(shaderId, "uFadeoutStart");
        self->uid_uFadeoutEnd = _glGetUniformLocation(shaderId, "uFadeoutEnd");
        
        if(self->uid_uPixelSize >= 0) {
            logger->Info("BRUTAL SSAO ACTIVE - All uniforms loaded");
        }
    }
    
    if(self->uid_uPixelSize >= 0) {
        if(self->uid_uSceneTex >= 0) _glUniform1i(self->uid_uSceneTex, 0);
        
        float pixelSize[2] = {1.0f / 1920.0f, 1.0f / 1080.0f};
        _glUniform2fv(self->uid_uPixelSize, 1, pixelSize);
        
        if(self->uid_uSampleRadius >= 0) {
            float sampleRadius = 2.5f;
            _glUniform1fv(self->uid_uSampleRadius, 1, &sampleRadius);
        }
        
        if(self->uid_uSampleCount >= 0) {
            int sampleCount = 16;
            _glUniform1i(self->uid_uSampleCount, sampleCount);
        }
        
        if(self->uid_uIntensity >= 0) {
            float intensity = 0.75f;
            _glUniform1fv(self->uid_uIntensity, 1, &intensity);
        }
        
        if(self->uid_uNormalBias >= 0) {
            float normalBias = 0.1f;
            _glUniform1fv(self->uid_uNormalBias, 1, &normalBias);
        }
        
        if(self->uid_uTime >= 0 && m_snTimeInMilliseconds) {
            float time = (float)(*m_snTimeInMilliseconds);
            _glUniform1fv(self->uid_uTime, 1, &time);
        }
        
        if(self->uid_uFadeoutStart >= 0) {
            float fadeoutStart = 0.85f;
            _glUniform1fv(self->uid_uFadeoutStart, 1, &fadeoutStart);
        }
        
        if(self->uid_uFadeoutEnd >= 0) {
            float fadeoutEnd = 1.0f;
            _glUniform1fv(self->uid_uFadeoutEnd, 1, &fadeoutEnd);
        }
    }
}

DECL_HOOKv(ES2Shader_InitAfterCompile, ES2ShaderEx* self) {
    ES2Shader_InitAfterCompile(self);
    
    self->uid_uSceneTex = -1;
    self->uid_uPixelSize = -1;
    self->uid_uSampleRadius = -1;
    self->uid_uSampleCount = -1;
    self->uid_uIntensity = -1;
    self->uid_uNormalBias = -1;
    self->uid_uTime = -1;
    self->uid_uFadeoutStart = -1;
    self->uid_uFadeoutEnd = -1;
}

extern "C" void OnModPreLoad() {
    logger->SetTag("SSAO_BRUTAL3");
    logger->Info("GLES3 BRUTAL SSAO - PREPARING");
}

extern "C" void OnModLoad() {
    logger->SetTag("SSAO_BRUTAL3");
    
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    
    if(!pGTASA || !hGTASA) {
        logger->Error("GTA:SA not found");
        return;
    }
    
    m_snTimeInMilliseconds = (uint32_t*)(pGTASA + 0x00953138);
    curShaderStateFlags = (uint32_t*)(pGTASA + 0x006b7094);
    UnderWaterness = (float*)(pGTASA + 0x00a7d158);
    
    void* hGLES3 = aml->GetLibHandle("libGLESv3.so");
    if(!hGLES3) hGLES3 = aml->GetLibHandle("libGLESv2.so");
    
    if(hGLES3) {
        _glGetUniformLocation = (glGetUniformLocation_t)aml->GetSym(hGLES3, "glGetUniformLocation");
        _glUniform1i = (glUniform1i_t)aml->GetSym(hGLES3, "glUniform1i");
        _glUniform1fv = (glUniform1fv_t)aml->GetSym(hGLES3, "glUniform1fv");
        _glUniform2fv = (glUniform2fv_t)aml->GetSym(hGLES3, "glUniform2fv");
        logger->Info("GLES3 functions loaded");
    }
    
    void* addr1 = (void*)aml->GetSym(hGTASA, "_ZN8RQShader11BuildSourceEjPPKcS2_");
    if(!addr1) addr1 = (void*)(pGTASA + 0x001CFA38);
    HOOK(RQShaderBuildSource, addr1);
    
    void* addr2 = (void*)aml->GetSym(hGTASA, "_ZN9ES2Shader6SelectEv");
    if(!addr2) addr2 = (void*)(pGTASA + 0x001CD368);
    HOOK(ES2Shader_Select, addr2);
    
    void* addr3 = (void*)aml->GetSym(hGTASA, "_ZN9ES2Shader22InitializeAfterCompileEv");
    if(!addr3) addr3 = (void*)(pGTASA + 0x001CC7CC);
    HOOK(ES2Shader_InitAfterCompile, addr3);
    
    logger->Info("GLES3 BRUTAL SSAO LOADED");
    logger->Info("16 SAMPLES | SPIRAL PATTERN | BAYER DITHER");
    logger->Info("NORMAL RECONSTRUCTION | EDGE DETECTION");
    logger->Info("VIGNETTE | FADEOUT | MTA-INSPIRED");
}
