#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <GLES2/gl2.h>
#include <string.h>
#include <stdio.h>

MYMOD(net.ssao.brutal, SSAO_Brutal, 1.0, ntyzsky)
NEEDGAME(com.rockstargames.gtasa)

#define SHADER_LEN 65536

uintptr_t pGTASA = 0;
void* hGTASA = NULL;

uint32_t* m_snTimeInMilliseconds;
uintptr_t TheCamera;

typedef int (*glGetUniformLocation_t)(int, const char*);
typedef void (*glUniform1fv_t)(int, int, const float*);
typedef void (*glUniform2fv_t)(int, int, const float*);
typedef void (*glUniform3fv_t)(int, int, const float*);

glGetUniformLocation_t _glGetUniformLocation;
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

struct ES2ShaderEx {
    char base[512];
    int uid_uPixelSize;
    int uid_uIntensity;
    int uid_uTime;
    int uid_uCameraPos;
};

char customFragShader[SHADER_LEN];

// Read shader file
inline void freadfull(char* buf, size_t maxlen, FILE* f) {
    size_t i = 0;
    --maxlen;
    while(!feof(f) && i < maxlen) {
        buf[i] = fgetc(f);
        ++i;
    }
    buf[i-1] = 0;
}

// Map building flags to SSAO shader
inline const char* FlagsToSSAOShader(int flags) {
    switch(flags) {
        // Building shaders
        case 0x10020430:
        case 0x12020430:
        case 0x10220432:
        case 0x10222432:
        case 0x10100430:
        case 0x10120430:
        case 0x10110430:
        case 0x10130430:
        case 0x1010042A:
        case 0x1012042A:
        case 0x1013042A:
        case 0x1011042A:
        case 0x1092042A:
            return "ssao";
        
        default:
            return NULL;
    }
}

DECL_HOOK(int, RQShaderBuildSource, int flags, char** pxlsrc, char** vtxsrc) {
    int ret = RQShaderBuildSource(flags, pxlsrc, vtxsrc);
    
    // Log flags (first 100)
    static int logCount = 0;
    if(logCount < 100) {
        logger->Info("Shader flag: 0x%X", flags);
        logCount++;
    }
    
    // Check if this flag needs SSAO
    const char* shaderName = FlagsToSSAOShader(flags);
    if(shaderName) {
        char shaderPath[512];
        sprintf(shaderPath, "%s/ssao/%s.glsl", aml->GetAndroidDataPath(), shaderName);
        
        FILE* pFile = fopen(shaderPath, "r");
        if(pFile != NULL) {
            logger->Info("LOADING SSAO for flag 0x%X from %s", flags, shaderPath);
            freadfull(customFragShader, SHADER_LEN, pFile);
            *pxlsrc = customFragShader;
            fclose(pFile);
        } else {
            logger->Error("FAILED to load %s", shaderPath);
        }
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
            logger->Info("SSAO uniforms bound!");
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

extern "C" void OnModPreLoad() {
    logger->SetTag("SSAO_BRUTAL");
}

extern "C" void OnModLoad() {
    logger->SetTag("SSAO_BRUTAL");
    
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    
    if(!pGTASA || !hGTASA) {
        logger->Error("GTA:SA not found");
        return;
    }
    
    m_snTimeInMilliseconds = (uint32_t*)(pGTASA + 0x00953138);
    TheCamera = (uintptr_t)aml->GetSym(hGTASA, "TheCamera");
    
    void* hGLES = aml->GetLibHandle("libGLESv2.so");
    if(hGLES) {
        _glGetUniformLocation = (glGetUniformLocation_t)aml->GetSym(hGLES, "glGetUniformLocation");
        _glUniform1fv = (glUniform1fv_t)aml->GetSym(hGLES, "glUniform1fv");
        _glUniform2fv = (glUniform2fv_t)aml->GetSym(hGLES, "glUniform2fv");
        _glUniform3fv = (glUniform3fv_t)aml->GetSym(hGLES, "glUniform3fv");
    }
    
    HOOKPLT(RQShaderBuildSource, pGTASA + 0x6720F8);
    
    void* addr2 = (void*)aml->GetSym(hGTASA, "_ZN9ES2Shader6SelectEv");
    if(!addr2) addr2 = (void*)(pGTASA + 0x001CD368);
    HOOK(ES2Shader_Select, addr2);
    
    void* addr3 = (void*)aml->GetSym(hGTASA, "_ZN9ES2Shader22InitializeAfterCompileEv");
    if(!addr3) addr3 = (void*)(pGTASA + 0x001CC7CC);
    HOOK(ES2Shader_InitAfterCompile, addr3);
    
    logger->Info("=== SSAO READY ===");
    logger->Info("Place ssao.glsl in: %s/ssao/", aml->GetAndroidDataPath());
}
