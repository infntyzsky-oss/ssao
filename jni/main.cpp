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

// Game vars
uint32_t* m_snTimeInMilliseconds;
float* UnderWaterness;
uintptr_t TheCamera;
uint32_t* curShaderStateFlags;
void** blurPShader;
void** gradingPShader;
void** shadowResolvePShader;
void** contrastVShader;
void** contrastPShader;

// GL functions
typedef int (*glGetUniformLocation_t)(int, const char*);
typedef void (*glUniform1i_t)(int, int);
typedef void (*glUniform1fv_t)(int, int, const float*);
typedef void (*glUniform2fv_t)(int, int, const float*);

glGetUniformLocation_t _glGetUniformLocation;
glUniform1i_t _glUniform1i;
glUniform1fv_t _glUniform1fv;
glUniform2fv_t _glUniform2fv;

// Camera struct (simplified)
struct CCamera {
    char _pad[0x10];
    float matrix[16];
    float position[3];
    char _pad2[0x500];
};

// SSAO shader - GLES2 only
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
    float radius = 4.0;
    
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
    
    float distanceFactor = length(uCameraPos) * 0.01;
    float dynamicIntensity = uIntensity * (1.0 + distanceFactor * 0.5);
    
    float occlusion = 1.0 - (ao * dynamicIntensity);
    
    vec2 cornerDist = abs(v_texCoord0 - 0.5) * 2.0;
    float vignette = 1.0 - pow(max(cornerDist.x, cornerDist.y), 2.0) * 0.15;
    
    gl_FragColor = vec4(color * occlusion * vignette, 1.0);
}
)";

char customFragShader[SHADER_LEN];
bool bBuildingShaderInjected = false;

struct ES2ShaderEx {
    char base[512];
    int uid_uPixelSize;
    int uid_uIntensity;
    int uid_uTime;
    int uid_uCameraPos;
};

// Hook postprocess buffer
DECL_HOOKv(postprocess_buffer, void* buffer, int width, int height) {
    postprocess_buffer(buffer, width, height);
}

// Hook shader building
DECL_HOOK(int, RQShaderBuildSource, int flags, char** pxlsrc, char** vtxsrc) {
    int ret = RQShaderBuildSource(flags, pxlsrc, vtxsrc);
    
    bool isBuilding = (flags == 0x10020430 || flags == 0x12020430 ||   
                       flags == 0x10220432 || flags == 0x10222432 ||  
                       flags == 0x10100430 || flags == 0x10120430 ||  
                       flags == 0x10110430 || flags == 0x10130430 ||  
                       flags == 0x1010042A || flags == 0x1012042A ||  
                       flags == 0x1013042A || flags == 0x1011042A ||  
                       flags == 0x1092042A);
    
    if(isBuilding && !bBuildingShaderInjected) {
        logger->Info("INJECTING SSAO to building shader: 0x%X", flags);
        strncpy(customFragShader, ssaoFragmentShader, SHADER_LEN - 1);
        customFragShader[SHADER_LEN - 1] = '\0';
        *pxlsrc = customFragShader;
        bBuildingShaderInjected = true;
    }
    
    return ret;
}

// Hook shader selection
DECL_HOOKv(ES2Shader_Select, ES2ShaderEx* self) {
    ES2Shader_Select(self);
    
    if(self->uid_uPixelSize == -1 && _glGetUniformLocation) {
        int shaderId = *(int*)self;
        self->uid_uPixelSize = _glGetUniformLocation(shaderId, "uPixelSize");
        self->uid_uIntensity = _glGetUniformLocation(shaderId, "uIntensity");
        self->uid_uTime = _glGetUniformLocation(shaderId, "uTime");
        self->uid_uCameraPos = _glGetUniformLocation(shaderId, "uCameraPos");
        
        if(self->uid_uPixelSize >= 0) {
            logger->Info("SSAO uniforms bound: pix=%d, int=%d, time=%d, cam=%d", 
                        self->uid_uPixelSize, self->uid_uIntensity, self->uid_uTime, self->uid_uCameraPos);
        }
    }
    
    if(self->uid_uPixelSize >= 0) {
        float pixelSize[2] = {1.0f / 1920.0f, 1.0f / 1080.0f};
        _glUniform2fv(self->uid_uPixelSize, 1, pixelSize);
        
        float intensity = 0.9f;
        _glUniform1fv(self->uid_uIntensity, 1, &intensity);
        
        if(m_snTimeInMilliseconds) {
            float time = (float)(*m_snTimeInMilliseconds);
            _glUniform1fv(self->uid_uTime, 1, &time);
        }
        
        if(TheCamera && self->uid_uCameraPos >= 0) {
            CCamera* cam = (CCamera*)TheCamera;
            _glUniform1fv(self->uid_uCameraPos, 3, cam->position);
        }
    }
}

// Hook shader init
DECL_HOOKv(ES2Shader_InitAfterCompile, ES2ShaderEx* self) {
    ES2Shader_InitAfterCompile(self);
    
    self->uid_uPixelSize = -1;
    self->uid_uIntensity = -1;
    self->uid_uTime = -1;
    self->uid_uCameraPos = -1;
}

// Hook entity render
DECL_HOOKv(CEntity_Render, void* self) {
    CEntity_Render(self);
}

extern "C" void OnModPreLoad() {
    logger->SetTag("SSAO_BRUTAL");
    logger->Info("Brutal SSAO initializing...");
}

extern "C" void OnModLoad() {
    logger->SetTag("SSAO_BRUTAL");
    
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    
    if(!pGTASA || !hGTASA) {
        logger->Error("GTA:SA library not found");
        return;
    }
    
    // Game vars
    m_snTimeInMilliseconds = (uint32_t*)(pGTASA + 0x00953138);
    UnderWaterness = (float*)(pGTASA + 0x00a7d158);
    TheCamera = (uintptr_t)aml->GetSym(hGTASA, "TheCamera");
    
    // Shader state vars
    curShaderStateFlags = (uint32_t*)(pGTASA + 0x006b7094);
    blurPShader = (void**)(pGTASA + 0x0067a264);
    gradingPShader = (void**)(pGTASA + 0x0067a260);
    shadowResolvePShader = (void**)(pGTASA + 0x0067a268);
    contrastVShader = (void**)(pGTASA + 0x0067a258);
    contrastPShader = (void**)(pGTASA + 0x0067a25c);
    
    logger->Info("Game vars: Camera=0x%X, ShaderFlags=%p", TheCamera, curShaderStateFlags);
    
    // GL functions
    void* hGLES = aml->GetLibHandle("libGLESv2.so");
    if(hGLES) {
        _glGetUniformLocation = (glGetUniformLocation_t)aml->GetSym(hGLES, "glGetUniformLocation");
        _glUniform1i = (glUniform1i_t)aml->GetSym(hGLES, "glUniform1i");
        _glUniform1fv = (glUniform1fv_t)aml->GetSym(hGLES, "glUniform1fv");
        _glUniform2fv = (glUniform2fv_t)aml->GetSym(hGLES, "glUniform2fv");
        logger->Info("GLES2 functions loaded");
    }
    
    logger->Info("Installing hooks...");
    
    void* addr_pp = (void*)(pGTASA + 0x00225222);
    HOOK(postprocess_buffer, addr_pp);
    
    void* addr1 = (void*)aml->GetSym(hGTASA, "_ZN8RQShader11BuildSourceEjPPKcS2_");
    if(!addr1) addr1 = (void*)(pGTASA + 0x001CFA38);
    HOOK(RQShaderBuildSource, addr1);
    
    void* addr2 = (void*)aml->GetSym(hGTASA, "_ZN9ES2Shader6SelectEv");
    if(!addr2) addr2 = (void*)(pGTASA + 0x001CD368);
    HOOK(ES2Shader_Select, addr2);
    
    void* addr3 = (void*)aml->GetSym(hGTASA, "_ZN9ES2Shader22InitializeAfterCompileEv");
    if(!addr3) addr3 = (void*)(pGTASA + 0x001CC7CC);
    HOOK(ES2Shader_InitAfterCompile, addr3);
    
    void* addr4 = (void*)(pGTASA + 0x003ed20c);
    HOOK(CEntity_Render, addr4);
    
    logger->Info("BRUTAL SSAO LOADED - Camera integration active");
}
