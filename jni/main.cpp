#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <GLES2/gl2.h>
#include <string.h>

MYMOD(net.ssao.brutal, SSAO_Brutal, 1.0, ntyzsky)
NEEDGAME(com.rockstargames.gtasa)

#define SHADER_LEN 65536

uintptr_t pGTASA = 0;
void* hGTASA = NULL;

uint32_t* m_snTimeInMilliseconds;
float* UnderWaterness;
uintptr_t TheCamera;
uint32_t* curShaderStateFlags;

typedef int (*glGetUniformLocation_t)(int, const char*);
typedef void (*glUniform1i_t)(int, int);
typedef void (*glUniform1fv_t)(int, int, const float*);
typedef void (*glUniform2fv_t)(int, int, const float*);
typedef void (*glUniform3fv_t)(int, int, const float*);

glGetUniformLocation_t _glGetUniformLocation;
glUniform1i_t _glUniform1i;
glUniform1fv_t _glUniform1fv;
glUniform2fv_t _glUniform2fv;
glUniform3fv_t _glUniform3fv;

struct CMatrix {
    float right[4];
    float forward[4];
    float up[4];
    float pos[4];
};

struct CCamera {
    char _pad1[0xB54];
    float vecFrustumNormals[4][3];
    char _pad2[0x60];
    CMatrix mCameraMatrix;
};

const char* ssaoFragmentShader = R"(
precision mediump float;
varying vec2 v_texCoord0;
uniform sampler2D uColorTex;
uniform vec2 uPixelSize;
uniform float uIntensity;
uniform float uTime;
uniform vec3 uCameraPos;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float getDepth(vec2 uv) {
    vec3 col = texture2D(uColorTex, uv).rgb;
    return dot(col, vec3(0.299, 0.587, 0.114));
}

void main() {
    vec3 color = texture2D(uColorTex, v_texCoord0).rgb;
    float depth = getDepth(v_texCoord0);
    
    float ao = 0.0;
    float radius = 5.0;
    float angleOffset = hash(v_texCoord0 + uTime * 0.001) * 6.28318;
    
    for(int i = 0; i < 16; i++) {
        float angle = (float(i) / 16.0) * 6.28318 + angleOffset;
        float dist = (float(i) / 16.0) * radius;
        vec2 offset = vec2(cos(angle), sin(angle)) * dist * uPixelSize;
        float sampleDepth = getDepth(v_texCoord0 + offset);
        float diff = depth - sampleDepth;
        float weight = smoothstep(0.0, radius * 0.5, dist);
        ao += max(0.0, diff) * weight;
    }
    
    ao = clamp(ao / 16.0, 0.0, 1.0);
    float distanceFactor = length(uCameraPos) * 0.005;
    float dynamicIntensity = uIntensity * (1.0 + distanceFactor);
    float occlusion = 1.0 - (ao * dynamicIntensity);
    
    vec2 cornerDist = abs(v_texCoord0 - 0.5) * 2.0;
    float vignette = 1.0 - pow(max(cornerDist.x, cornerDist.y), 2.5) * 0.2;
    
    gl_FragColor = vec4(color * occlusion * vignette, 1.0);
}
)";

char customFragShader[SHADER_LEN];
int injectionCount = 0;

struct ES2ShaderEx {
    char base[512];
    int uid_uPixelSize;
    int uid_uIntensity;
    int uid_uTime;
    int uid_uCameraPos;
};

// HOOK SEMUA SHADER - BRUTE FORCE MODE
DECL_HOOK(int, RQShaderBuildSource, int flags, char** pxlsrc, char** vtxsrc) {
    int ret = RQShaderBuildSource(flags, pxlsrc, vtxsrc);
    
    // LOG SEMUA FLAGS
    static int logCount = 0;
    if(logCount < 50) {
        logger->Info("SHADER FLAG: 0x%X", flags);
        logCount++;
    }
    
    // INJECT KE SEMUA SHADER (testing mode)
    if(injectionCount < 10) {
        logger->Info("BRUTAL INJECT to flag 0x%X (count: %d)", flags, injectionCount);
        strncpy(customFragShader, ssaoFragmentShader, SHADER_LEN - 1);
        customFragShader[SHADER_LEN - 1] = '\0';
        *pxlsrc = customFragShader;
        injectionCount++;
    }
    
    return ret;
}

DECL_HOOKv(ES2Shader_Select, ES2ShaderEx* self) {
    ES2Shader_Select(self);
    
    if(self->uid_uPixelSize == -1 && _glGetUniformLocation) {
        int shaderId = *(int*)self;
        self->uid_uPixelSize = _glGetUniformLocation(shaderId, "uPixelSize");
        self->uid_uIntensity = _glGetUniformLocation(shaderId, "uIntensity");
        self->uid_uTime = _glGetUniformLocation(shaderId, "uTime");
        self->uid_uCameraPos = _glGetUniformLocation(shaderId, "uCameraPos");
        
        if(self->uid_uPixelSize >= 0) {
            logger->Info("SSAO UNIFORMS BOUND! pix=%d int=%d time=%d cam=%d", 
                        self->uid_uPixelSize, self->uid_uIntensity, self->uid_uTime, self->uid_uCameraPos);
        }
    }
    
    if(self->uid_uPixelSize >= 0) {
        float pixelSize[2] = {1.0f / 1920.0f, 1.0f / 1080.0f};
        _glUniform2fv(self->uid_uPixelSize, 1, pixelSize);
        
        float intensity = 1.0f;
        _glUniform1fv(self->uid_uIntensity, 1, &intensity);
        
        if(m_snTimeInMilliseconds) {
            float time = (float)(*m_snTimeInMilliseconds);
            _glUniform1fv(self->uid_uTime, 1, &time);
        }
        
        if(TheCamera && self->uid_uCameraPos >= 0) {
            CCamera* cam = (CCamera*)TheCamera;
            float camPos[3] = {
                cam->mCameraMatrix.pos[0],
                cam->mCameraMatrix.pos[1],
                cam->mCameraMatrix.pos[2]
            };
            _glUniform3fv(self->uid_uCameraPos, 1, camPos);
        }
    }
}

DECL_HOOKv(ES2Shader_InitAfterCompile, ES2ShaderEx* self) {
    ES2Shader_InitAfterCompile(self);
    self->uid_uPixelSize = -1;
    self->uid_uIntensity = -1;
    self->uid_uTime = -1;
    self->uid_uCameraPos = -1;
}

DECL_HOOKv(CEntity_Render, void* self) {
    CEntity_Render(self);
}

extern "C" void OnModPreLoad() {
    logger->SetTag("SSAO_BRUTAL");
    logger->Info("BRUTAL SSAO PRELOAD");
}

extern "C" void OnModLoad() {
    logger->SetTag("SSAO_BRUTAL");
    logger->Info("=== BRUTAL SSAO LOADING ===");
    
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    
    if(!pGTASA || !hGTASA) {
        logger->Error("FATAL: GTA:SA not found!");
        return;
    }
    
    logger->Info("GTA:SA base: 0x%X", pGTASA);
    
    m_snTimeInMilliseconds = (uint32_t*)(pGTASA + 0x00953138);
    UnderWaterness = (float*)(pGTASA + 0x00a7d158);
    TheCamera = (uintptr_t)aml->GetSym(hGTASA, "TheCamera");
    curShaderStateFlags = (uint32_t*)(pGTASA + 0x006b7094);
    
    logger->Info("TheCamera: 0x%X", TheCamera);
    
    void* hGLES = aml->GetLibHandle("libGLESv2.so");
    if(hGLES) {
        _glGetUniformLocation = (glGetUniformLocation_t)aml->GetSym(hGLES, "glGetUniformLocation");
        _glUniform1i = (glUniform1i_t)aml->GetSym(hGLES, "glUniform1i");
        _glUniform1fv = (glUniform1fv_t)aml->GetSym(hGLES, "glUniform1fv");
        _glUniform2fv = (glUniform2fv_t)aml->GetSym(hGLES, "glUniform2fv");
        _glUniform3fv = (glUniform3fv_t)aml->GetSym(hGLES, "glUniform3fv");
        logger->Info("GLES2 functions loaded");
    }
    
    logger->Info("=== INSTALLING HOOKS ===");
    
    // Try symbol first
    void* addr1 = (void*)aml->GetSym(hGTASA, "_ZN8RQShader11BuildSourceEjPPKcS2_");
    if(addr1) {
        logger->Info("Found RQShader::BuildSource via symbol: %p", addr1);
    } else {
        addr1 = (void*)(pGTASA + 0x001CFA38);
        logger->Info("Using offset for RQShader::BuildSource: %p", addr1);
    }
    HOOK(RQShaderBuildSource, addr1);
    
    void* addr2 = (void*)aml->GetSym(hGTASA, "_ZN9ES2Shader6SelectEv");
    if(!addr2) addr2 = (void*)(pGTASA + 0x001CD368);
    logger->Info("ES2Shader::Select: %p", addr2);
    HOOK(ES2Shader_Select, addr2);
    
    void* addr3 = (void*)aml->GetSym(hGTASA, "_ZN9ES2Shader22InitializeAfterCompileEv");
    if(!addr3) addr3 = (void*)(pGTASA + 0x001CC7CC);
    logger->Info("ES2Shader::InitAfterCompile: %p", addr3);
    HOOK(ES2Shader_InitAfterCompile, addr3);
    
    void* addr4 = (void*)(pGTASA + 0x003ed20c);
    logger->Info("CEntity::Render: %p", addr4);
    HOOK(CEntity_Render, addr4);
    
    logger->Info("=== BRUTAL SSAO READY ===");
    logger->Info("Injecting to first 10 shaders for testing!");
}
