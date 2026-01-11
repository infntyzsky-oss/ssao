#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <string.h>

MYMOD(net.ssao.brutal3.depthpass, SSAO_DepthPass, 1.0, ntyzsky)
NEEDGAME(com.rockstargames.gtasa)

#define SHADER_LEN 131072

uintptr_t pGTASA = 0;
void* hGTASA = NULL;

uint32_t* m_snTimeInMilliseconds;
uint32_t* curShaderStateFlags;
float* UnderWaterness;

struct RwV3d { float x, y, z; };
struct RwMatrix {
    RwV3d right, up, at, pos;
    uint32_t flags;
};

struct RwCamera {
    char pad[0x60];
    RwMatrix* viewMatrix;
    float viewWindow[2];
    float recipViewWindow[2];
    float viewOffset[2];
    float nearPlane, farPlane;
    float fogPlane;
};

typedef int (*glGetUniformLocation_t)(int, const char*);
typedef void (*glUniform1i_t)(int, int);
typedef void (*glUniform1fv_t)(int, int, const float*);
typedef void (*glUniform2fv_t)(int, int, const float*);
typedef void (*glUniformMatrix4fv_t)(int, int, uint8_t, const float*);
typedef void (*glUniformMatrix3fv_t)(int, int, uint8_t, const float*);
typedef void (*glGenFramebuffers_t)(int, uint32_t*);
typedef void (*glBindFramebuffer_t)(uint32_t, uint32_t);
typedef void (*glFramebufferTexture2D_t)(uint32_t, uint32_t, uint32_t, uint32_t, int);
typedef void (*glGenTextures_t)(int, uint32_t*);
typedef void (*glBindTexture_t)(uint32_t, uint32_t);
typedef void (*glTexImage2D_t)(uint32_t, int, int, int, int, int, uint32_t, uint32_t, const void*);
typedef void (*glTexParameteri_t)(uint32_t, uint32_t, int);
typedef void (*glClear_t)(uint32_t);
typedef void (*glViewport_t)(int, int, int, int);
typedef uint32_t (*glCheckFramebufferStatus_t)(uint32_t);
typedef void (*glActiveTexture_t)(uint32_t);

glGetUniformLocation_t _glGetUniformLocation;
glUniform1i_t _glUniform1i;
glUniform1fv_t _glUniform1fv;
glUniform2fv_t _glUniform2fv;
glUniformMatrix4fv_t _glUniformMatrix4fv_real;
glUniformMatrix3fv_t _glUniformMatrix3fv_real;
glGenFramebuffers_t _glGenFramebuffers;
glBindFramebuffer_t _glBindFramebuffer;
glFramebufferTexture2D_t _glFramebufferTexture2D;
glGenTextures_t _glGenTextures;
glBindTexture_t _glBindTexture;
glTexImage2D_t _glTexImage2D;
glTexParameteri_t _glTexParameteri;
glClear_t _glClear;
glViewport_t _glViewport;
glCheckFramebufferStatus_t _glCheckFramebufferStatus;
glActiveTexture_t _glActiveTexture;

float cachedMVP[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
float cachedModelView[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
bool matrixCaptured = false;

GLuint depthTexture = 0;
GLuint depthFBO = 0;
bool depthTargetReady = false;
int screenWidth = 1920;
int screenHeight = 1080;

void CreateDepthTarget() {
    if(depthTargetReady) return;
    
    _glGenTextures(1, &depthTexture);
    _glBindTexture(GL_TEXTURE_2D, depthTexture);
    _glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, screenWidth, screenHeight, 
                  0, GL_RED, GL_FLOAT, NULL);
    _glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    _glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    _glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    _glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    _glGenFramebuffers(1, &depthFBO);
    _glBindFramebuffer(GL_FRAMEBUFFER, depthFBO);
    _glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 
                           GL_TEXTURE_2D, depthTexture, 0);
    
    uint32_t status = _glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if(status == GL_FRAMEBUFFER_COMPLETE) {
        depthTargetReady = true;
        logger->Info("Depth target created: %dx%d", screenWidth, screenHeight);
    } else {
        logger->Error("Depth FBO failed: 0x%X", status);
    }
    
    _glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

const char* depthVertexShader = R"(
#version 300 es
precision highp float;

layout(location = 0) in vec3 aPosition;

uniform mat4 uMVP;
out float vDepth;

void main() {
    vec4 pos = uMVP * vec4(aPosition, 1.0);
    gl_Position = pos;
    vDepth = (pos.z / pos.w) * 0.5 + 0.5;
}
)";

const char* depthFragmentShader = R"(
#version 300 es
precision highp float;

in float vDepth;
out vec4 fragColor;

void main() {
    fragColor = vec4(vDepth, vDepth, vDepth, 1.0);
}
)";

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
uniform sampler2D uDepthTex;
uniform vec2 uPixelSize;
uniform float uSampleRadius;
uniform int uSampleCount;
uniform float uIntensity;
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

float getDepth(vec2 uv) {
    return texture(uDepthTex, uv).r;
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
    
    if(centerDepth > 0.999) {
        fragColor = vec4(sceneColor, 1.0);
        return;
    }
    
    vec3 normal = getNormal(vTexCoord, centerDepth);
    
    float ao = 0.0;
    float radius = uSampleRadius;
    int samples = uSampleCount;
    
    const float goldenAngle = 2.399963;
    float angleOffset = hash12(vPixPos * 1000.0) * TAU + uTime * 0.0001;
    
    for(int i = 0; i < samples; i++) {
        float angle = float(i) * goldenAngle + angleOffset;
        float dist = sqrt(float(i) / float(samples)) * radius;
        
        vec2 offset = vec2(cos(angle), sin(angle)) * dist * uPixelSize;
        vec2 sampleUV = vTexCoord + offset;
        
        float sampleDepth = getDepth(sampleUV);
        float depthDiff = centerDepth - sampleDepth;
        
        float distWeight = 1.0 - smoothstep(0.0, radius, length(offset / uPixelSize));
        
        vec3 sampleDir = normalize(vec3(offset, depthDiff));
        float normalWeight = max(0.0, dot(normal, sampleDir));
        
        float occlusion = step(0.0, depthDiff) * distWeight * normalWeight;
        occlusion *= smoothstep(0.0, 0.01, depthDiff);
        
        ao += occlusion;
    }
    
    ao = clamp(ao / float(samples), 0.0, 1.0);
    ao = pow(ao, 1.0 / 2.2);
    ao = 1.0 - pow(1.0 - ao, uIntensity * 3.0);
    
    float fadeout = smoothstep(uFadeoutStart, uFadeoutEnd, centerDepth);
    ao = mix(ao, 0.0, fadeout);
    
    float occlusion = 1.0 - ao;
    
    fragColor = vec4(sceneColor * occlusion, 1.0);
}
)";

char depthVertShaderBuf[SHADER_LEN];
char depthFragShaderBuf[SHADER_LEN];
char ssaoVertShaderBuf[SHADER_LEN];
char ssaoFragShaderBuf[SHADER_LEN];

bool depthShaderInjected = false;
bool ssaoShaderInjected = false;
bool isDepthPass = false;

struct DepthShaderEx {
    char base[512];
    int uid_uMVP;
};

struct SSAOShaderEx {
    char base[512];
    int uid_uSceneTex;
    int uid_uDepthTex;
    int uid_uPixelSize;
    int uid_uSampleRadius;
    int uid_uSampleCount;
    int uid_uIntensity;
    int uid_uTime;
    int uid_uFadeoutStart;
    int uid_uFadeoutEnd;
};

DECL_HOOKv(glUniformMatrix4fv_Hook, int location, int count, uint8_t transpose, const float* value) {
    if(!isDepthPass && count == 1 && value) {
        memcpy(cachedMVP, value, 64);
        matrixCaptured = true;
    }
    
    _glUniformMatrix4fv_real(location, count, transpose, value);
}

DECL_HOOKv(glUniformMatrix3fv_Hook, int location, int count, uint8_t transpose, const float* value) {
    if(!isDepthPass && count == 1 && value) {
        cachedModelView[0] = value[0]; cachedModelView[1] = value[1]; cachedModelView[2] = value[2];
        cachedModelView[4] = value[3]; cachedModelView[5] = value[4]; cachedModelView[6] = value[5];
        cachedModelView[8] = value[6]; cachedModelView[9] = value[7]; cachedModelView[10] = value[8];
    }
    
    _glUniformMatrix3fv_real(location, count, transpose, value);
}

DECL_HOOK(int, RQShaderBuildSource, int flags, char** pxlsrc, char** vtxsrc) {
    int ret = RQShaderBuildSource(flags, pxlsrc, vtxsrc);
    
    bool isBuilding = (flags == 0x10020430 || flags == 0x12020430 ||
                       flags == 0x10220432 || flags == 0x10222432 ||
                       flags == 0x10100430 || flags == 0x10120430 ||
                       flags == 0x10110430 || flags == 0x10130430 ||
                       flags == 0x1010042A || flags == 0x1012042A ||
                       flags == 0x1013042A || flags == 0x1011042A ||
                       flags == 0x1092042A);
    
    if(isDepthPass && !depthShaderInjected && isBuilding) {
        logger->Info("DEPTH SHADER INJECT at 0x%X", flags);
        
        strncpy(depthFragShaderBuf, depthFragmentShader, SHADER_LEN - 1);
        depthFragShaderBuf[SHADER_LEN - 1] = '\0';
        *pxlsrc = depthFragShaderBuf;
        
        strncpy(depthVertShaderBuf, depthVertexShader, SHADER_LEN - 1);
        depthVertShaderBuf[SHADER_LEN - 1] = '\0';
        *vtxsrc = depthVertShaderBuf;
        
        depthShaderInjected = true;
    }
    else if(!isDepthPass && !ssaoShaderInjected && isBuilding) {
        logger->Info("SSAO SHADER INJECT at 0x%X", flags);
        
        strncpy(ssaoFragShaderBuf, ssaoFragmentShader, SHADER_LEN - 1);
        ssaoFragShaderBuf[SHADER_LEN - 1] = '\0';
        *pxlsrc = ssaoFragShaderBuf;
        
        strncpy(ssaoVertShaderBuf, ssaoVertexShader, SHADER_LEN - 1);
        ssaoVertShaderBuf[SHADER_LEN - 1] = '\0';
        *vtxsrc = ssaoVertShaderBuf;
        
        ssaoShaderInjected = true;
    }
    
    return ret;
}

DECL_HOOKv(ES2Shader_Select, SSAOShaderEx* self) {
    ES2Shader_Select(self);
    
    if(!_glGetUniformLocation) return;
    
    int shaderId = *(int*)self;
    
    if(isDepthPass && depthShaderInjected) {
        DepthShaderEx* depthShader = (DepthShaderEx*)self;
        if(depthShader->uid_uMVP == -1) {
            depthShader->uid_uMVP = _glGetUniformLocation(shaderId, "uMVP");
            logger->Info("Depth shader uniforms loaded");
        }
        
        if(depthShader->uid_uMVP >= 0 && matrixCaptured) {
            _glUniformMatrix4fv_real(depthShader->uid_uMVP, 1, 0, cachedMVP);
        }
    }
    else if(!isDepthPass && ssaoShaderInjected) {
        if(self->uid_uDepthTex == -1) {
            self->uid_uSceneTex = _glGetUniformLocation(shaderId, "uSceneTex");
            self->uid_uDepthTex = _glGetUniformLocation(shaderId, "uDepthTex");
            self->uid_uPixelSize = _glGetUniformLocation(shaderId, "uPixelSize");
            self->uid_uSampleRadius = _glGetUniformLocation(shaderId, "uSampleRadius");
            self->uid_uSampleCount = _glGetUniformLocation(shaderId, "uSampleCount");
            self->uid_uIntensity = _glGetUniformLocation(shaderId, "uIntensity");
            self->uid_uTime = _glGetUniformLocation(shaderId, "uTime");
            self->uid_uFadeoutStart = _glGetUniformLocation(shaderId, "uFadeoutStart");
            self->uid_uFadeoutEnd = _glGetUniformLocation(shaderId, "uFadeoutEnd");
            
            logger->Info("SSAO shader uniforms loaded");
        }
        
        if(self->uid_uDepthTex >= 0) {
            _glActiveTexture(GL_TEXTURE0);
            if(self->uid_uSceneTex >= 0) _glUniform1i(self->uid_uSceneTex, 0);
            
            _glActiveTexture(GL_TEXTURE1);
            _glBindTexture(GL_TEXTURE_2D, depthTexture);
            _glUniform1i(self->uid_uDepthTex, 1);
            
            _glActiveTexture(GL_TEXTURE0);
            
            float pixelSize[2] = {1.0f / screenWidth, 1.0f / screenHeight};
            _glUniform2fv(self->uid_uPixelSize, 1, pixelSize);
            
            float sampleRadius = 3.0f;
            _glUniform1fv(self->uid_uSampleRadius, 1, &sampleRadius);
            
            int sampleCount = 16;
            _glUniform1i(self->uid_uSampleCount, sampleCount);
            
            float intensity = 1.0f;
            _glUniform1fv(self->uid_uIntensity, 1, &intensity);
            
            if(m_snTimeInMilliseconds) {
                float time = (float)(*m_snTimeInMilliseconds);
                _glUniform1fv(self->uid_uTime, 1, &time);
            }
            
            float fadeoutStart = 0.8f;
            _glUniform1fv(self->uid_uFadeoutStart, 1, &fadeoutStart);
            
            float fadeoutEnd = 1.0f;
            _glUniform1fv(self->uid_uFadeoutEnd, 1, &fadeoutEnd);
        }
    }
}

DECL_HOOKv(ES2Shader_InitAfterCompile, SSAOShaderEx* self) {
    ES2Shader_InitAfterCompile(self);
    
    self->uid_uSceneTex = -1;
    self->uid_uDepthTex = -1;
    self->uid_uPixelSize = -1;
    self->uid_uSampleRadius = -1;
    self->uid_uSampleCount = -1;
    self->uid_uIntensity = -1;
    self->uid_uTime = -1;
    self->uid_uFadeoutStart = -1;
    self->uid_uFadeoutEnd = -1;
}

typedef void (*RwCameraForAllClumpsInFrustum_t)(RwCamera*, void*);
RwCameraForAllClumpsInFrustum_t _RwCameraForAllClumpsInFrustum;

DECL_HOOKv(RwCameraForAllClumpsInFrustum_Hook, RwCamera* camera, void* callback) {
    if(!depthTargetReady) {
        CreateDepthTarget();
    }
    
    if(depthTargetReady) {
        isDepthPass = true;
        depthShaderInjected = false;
        
        _glBindFramebuffer(GL_FRAMEBUFFER, depthFBO);
        _glViewport(0, 0, screenWidth, screenHeight);
        _glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        
        _RwCameraForAllClumpsInFrustum(camera, callback);
        
        _glBindFramebuffer(GL_FRAMEBUFFER, 0);
        isDepthPass = false;
    }
    
    ssaoShaderInjected = false;
    _RwCameraForAllClumpsInFrustum(camera, callback);
}

void LoadGLFunctions() {
    void* hGLES = aml->GetLibHandle("libGLESv3.so");
    if(!hGLES) hGLES = aml->GetLibHandle("libGLESv2.so");
    
    if(!hGLES) {
        logger->Error("OpenGL library not found");
        return;
    }
    
    _glGetUniformLocation = (glGetUniformLocation_t)aml->GetSym(hGLES, "glGetUniformLocation");
    _glUniform1i = (glUniform1i_t)aml->GetSym(hGLES, "glUniform1i");
    _glUniform1fv = (glUniform1fv_t)aml->GetSym(hGLES, "glUniform1fv");
    _glUniform2fv = (glUniform2fv_t)aml->GetSym(hGLES, "glUniform2fv");
    _glGenFramebuffers = (glGenFramebuffers_t)aml->GetSym(hGLES, "glGenFramebuffers");
    _glBindFramebuffer = (glBindFramebuffer_t)aml->GetSym(hGLES, "glBindFramebuffer");
    _glFramebufferTexture2D = (glFramebufferTexture2D_t)aml->GetSym(hGLES, "glFramebufferTexture2D");
    _glGenTextures = (glGenTextures_t)aml->GetSym(hGLES, "glGenTextures");
    _glBindTexture = (glBindTexture_t)aml->GetSym(hGLES, "glBindTexture");
    _glTexImage2D = (glTexImage2D_t)aml->GetSym(hGLES, "glTexImage2D");
    _glTexParameteri = (glTexParameteri_t)aml->GetSym(hGLES, "glTexParameteri");
    _glClear = (glClear_t)aml->GetSym(hGLES, "glClear");
    _glViewport = (glViewport_t)aml->GetSym(hGLES, "glViewport");
    _glCheckFramebufferStatus = (glCheckFramebufferStatus_t)aml->GetSym(hGLES, "glCheckFramebufferStatus");
    _glActiveTexture = (glActiveTexture_t)aml->GetSym(hGLES, "glActiveTexture");
    
    uintptr_t matrix4fv = aml->GetSym(hGLES, "glUniformMatrix4fv");
    uintptr_t matrix3fv = aml->GetSym(hGLES, "glUniformMatrix3fv");
    
    if(matrix4fv) {
        HOOK(glUniformMatrix4fv_Hook, (void*)matrix4fv);
        _glUniformMatrix4fv_real = (glUniformMatrix4fv_t)matrix4fv;
        logger->Info("Hooked glUniformMatrix4fv");
    }
    
    if(matrix3fv) {
        HOOK(glUniformMatrix3fv_Hook, (void*)matrix3fv);
        _glUniformMatrix3fv_real = (glUniformMatrix3fv_t)matrix3fv;
        logger->Info("Hooked glUniformMatrix3fv");
    }
    
    logger->Info("GL functions loaded");
}

extern "C" void OnModPreLoad() {
    logger->SetTag("SSAO_DEPTH");
    logger->Info("DEPTH PREPASS SSAO PREPARING");
}

extern "C" void OnModLoad() {
    logger->SetTag("SSAO_DEPTH");
    
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    
    if(!pGTASA || !hGTASA) {
        logger->Error("GTA:SA not found");
        return;
    }
    
    m_snTimeInMilliseconds = (uint32_t*)(pGTASA + 0x00953138);
    curShaderStateFlags = (uint32_t*)(pGTASA + 0x006b7094);
    UnderWaterness = (float*)(pGTASA + 0x00a7d158);
    
    LoadGLFunctions();
    
    void* addr1 = (void*)aml->GetSym(hGTASA, "_ZN8RQShader11BuildSourceEjPPKcS2_");
    if(!addr1) addr1 = (void*)(pGTASA + 0x001CFA38);
    HOOK(RQShaderBuildSource, addr1);
    
    void* addr2 = (void*)aml->GetSym(hGTASA, "_ZN9ES2Shader6SelectEv");
    if(!addr2) addr2 = (void*)(pGTASA + 0x001CD368);
    HOOK(ES2Shader_Select, addr2);
    
    void* addr3 = (void*)aml->GetSym(hGTASA, "_ZN9ES2Shader22InitializeAfterCompileEv");
    if(!addr3) addr3 = (void*)(pGTASA + 0x001CC7CC);
    HOOK(ES2Shader_InitAfterCompile, addr3);
    
    void* addr4 = (void*)aml->GetSym(hGTASA, "_Z29RwCameraForAllClumpsInFrustumP8RwCameraPv");
    if(!addr4) addr4 = (void*)(pGTASA + 0x0021e690);
    HOOK(RwCameraForAllClumpsInFrustum_Hook, addr4);
    _RwCameraForAllClumpsInFrustum = (RwCameraForAllClumpsInFrustum_t)addr4;
    
    logger->Info("DEPTH PREPASS SSAO LOADED");
    logger->Info("REAL DEPTH BUFFER | 16 SAMPLES");
    logger->Info("SPIRAL PATTERN | NORMAL RECONSTRUCTION");
}
