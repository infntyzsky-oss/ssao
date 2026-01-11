#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <GLES2/gl2.h>
#include <string.h>

MYMOD(net.ssao.gtasa, SSAO_Dynamic, 1.0, ntyzsky)
NEEDGAME(com.rockstargames.gtasa)

#define SHADER_LEN 32768

uintptr_t pGTASA = 0;
void* hGTASA = NULL;

// ============================================================================
// SSAO Shader Code
// ============================================================================

const char* ssaoFragmentShader = R"(
precision mediump float;
varying vec2 v_texCoord0;
uniform sampler2D uColorTex;
uniform vec2 uPixelSize;
uniform float uIntensity;

float getDepth(vec2 uv) {
    vec3 col = texture2D(uColorTex, uv).rgb;
    return dot(col, vec3(0.299, 0.587, 0.114));
}

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main() {
    vec3 color = texture2D(uColorTex, v_texCoord0).rgb;
    float depth = getDepth(v_texCoord0);
    
    float ao = 0.0;
    float radius = 2.5;
    float angleOffset = hash(v_texCoord0) * 6.28318;
    
    for(int i = 0; i < 8; i++) {
        float angle = (float(i) / 8.0) * 6.28318 + angleOffset;
        vec2 offset = vec2(cos(angle), sin(angle)) * radius * uPixelSize;
        
        float sampleDepth = getDepth(v_texCoord0 + offset);
        float diff = depth - sampleDepth;
        
        ao += max(0.0, diff) * smoothstep(0.0, radius * 0.5, abs(diff));
    }
    
    ao = clamp(ao / 8.0, 0.0, 1.0);
    float occlusion = 1.0 - (ao * uIntensity);
    
    gl_FragColor = vec4(color * occlusion, 1.0);
}
)";

char customFragShader[SHADER_LEN];
char customVertShader[SHADER_LEN];
bool bSSAOInjected = false;

// ============================================================================
// Hook RQShaderBuildSource via SYMBOL NAME (like SAShaderLoader)
// ============================================================================

typedef int (*RQShaderBuildSource_t)(int, char**, char**);
RQShaderBuildSource_t RQShaderBuildSource_orig = NULL;

int RQShaderBuildSource_hook(int flags, char** pxlsrc, char** vtxsrc) {
    int ret = RQShaderBuildSource_orig(flags, pxlsrc, vtxsrc);
    
    // Log all shader flags to find the right one
    static int logCount = 0;
    if(logCount < 20) {
        logger->Info("Shader flag: 0x%X", flags);
        logCount++;
    }
    
    // Inject SSAO shader (try flag 0x10 first, adjust if needed)
    if((flags == 0x10 || flags == 0x200010) && !bSSAOInjected) {
        logger->Info("Injecting SSAO fragment shader (flags=0x%X)", flags);
        strncpy(customFragShader, ssaoFragmentShader, SHADER_LEN - 1);
        customFragShader[SHADER_LEN - 1] = '\0';
        *pxlsrc = customFragShader;
        bSSAOInjected = true;
    }
    
    return ret;
}

// ============================================================================
// ES2Shader Extended
// ============================================================================

struct ES2ShaderEx {
    char base[256];
    int uid_uPixelSize;
    int uid_uIntensity;
};

typedef void (*ES2Shader_Select_t)(void*);
ES2Shader_Select_t ES2Shader_Select_orig = NULL;

void ES2Shader_Select_hook(ES2ShaderEx* self) {
    ES2Shader_Select_orig(self);
    
    // Get GL function
    typedef int (*glGetUniformLocation_t)(int, const char*);
    typedef void (*glUniform1fv_t)(int, int, const float*);
    typedef void (*glUniform2fv_t)(int, int, const float*);
    
    static glGetUniformLocation_t _glGetUniformLocation = NULL;
    static glUniform1fv_t _glUniform1fv = NULL;
    static glUniform2fv_t _glUniform2fv = NULL;
    
    if(!_glGetUniformLocation) {
        _glGetUniformLocation = (glGetUniformLocation_t)aml->GetSym(hGTASA, "glGetUniformLocation");
        _glUniform1fv = (glUniform1fv_t)aml->GetSym(hGTASA, "glUniform1fv");
        _glUniform2fv = (glUniform2fv_t)aml->GetSym(hGTASA, "glUniform2fv");
        
        if(!_glGetUniformLocation) {
            void* hGLES = aml->GetLibHandle("libGLESv2.so");
            _glGetUniformLocation = (glGetUniformLocation_t)aml->GetSym(hGLES, "glGetUniformLocation");
            _glUniform1fv = (glUniform1fv_t)aml->GetSym(hGLES, "glUniform1fv");
            _glUniform2fv = (glUniform2fv_t)aml->GetSym(hGLES, "glUniform2fv");
        }
    }
    
    if(!_glGetUniformLocation) return;
    
    // Initialize uniforms
    if(self->uid_uPixelSize == -1) {
        int shaderId = *(int*)self;
        self->uid_uPixelSize = _glGetUniformLocation(shaderId, "uPixelSize");
        self->uid_uIntensity = _glGetUniformLocation(shaderId, "uIntensity");
        
        if(self->uid_uPixelSize >= 0) {
            logger->Info("SSAO shader active! Uniforms: pixelSize=%d, intensity=%d", 
                        self->uid_uPixelSize, self->uid_uIntensity);
        }
    }
    
    // Update uniforms
    if(self->uid_uPixelSize >= 0 && _glUniform2fv) {
        float pixelSize[2] = {1.0f / 1280.0f, 1.0f / 720.0f};
        _glUniform2fv(self->uid_uPixelSize, 1, pixelSize);
        
        if(_glUniform1fv) {
            float intensity = 0.7f;
            _glUniform1fv(self->uid_uIntensity, 1, &intensity);
        }
    }
}

typedef void (*ES2Shader_InitAfterCompile_t)(void*);
ES2Shader_InitAfterCompile_t ES2Shader_InitAfterCompile_orig = NULL;

void ES2Shader_InitAfterCompile_hook(ES2ShaderEx* self) {
    ES2Shader_InitAfterCompile_orig(self);
    
    self->uid_uPixelSize = -1;
    self->uid_uIntensity = -1;
}

// ============================================================================
// Mod Entry - Hook by SYMBOL NAME
// ============================================================================

extern "C" void OnModLoad() {
    logger->SetTag("SSAO");
    
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    
    if(!pGTASA || !hGTASA) {
        logger->Error("GTA:SA not found!");
        return;
    }
    
    logger->Info("Hooking by symbol names (no hardcoded offsets)...");
    
    // Hook RQShader::BuildSource by symbol
    void* buildSourceAddr = aml->GetSym(hGTASA, "_ZN8RQShader11BuildSourceEjPPKcS2_");
    if(buildSourceAddr) {
        if(aml->Hook(buildSourceAddr, (void*)RQShaderBuildSource_hook, (void**)&RQShaderBuildSource_orig)) {
            logger->Info("✓ Hooked RQShader::BuildSource");
        } else {
            logger->Error("✗ Failed to hook RQShader::BuildSource");
        }
    } else {
        logger->Error("✗ Symbol _ZN8RQShader11BuildSourceEjPPKcS2_ not found!");
    }
    
    // Hook ES2Shader::Select by symbol
    void* selectAddr = aml->GetSym(hGTASA, "_ZN9ES2Shader6SelectEv");
    if(selectAddr) {
        if(aml->Hook(selectAddr, (void*)ES2Shader_Select_hook, (void**)&ES2Shader_Select_orig)) {
            logger->Info("✓ Hooked ES2Shader::Select");
        } else {
            logger->Error("✗ Failed to hook ES2Shader::Select");
        }
    } else {
        logger->Error("✗ Symbol _ZN9ES2Shader6SelectEv not found!");
    }
    
    // Hook ES2Shader::InitializeAfterCompile by symbol
    void* initAddr = aml->GetSym(hGTASA, "_ZN9ES2Shader22InitializeAfterCompileEv");
    if(initAddr) {
        if(aml->Hook(initAddr, (void*)ES2Shader_InitAfterCompile_hook, (void**)&ES2Shader_InitAfterCompile_orig)) {
            logger->Info("✓ Hooked ES2Shader::InitializeAfterCompile");
        } else {
            logger->Error("✗ Failed to hook ES2Shader::InitializeAfterCompile");
        }
    } else {
        logger->Error("✗ Symbol _ZN9ES2Shader22InitializeAfterCompileEv not found!");
    }
    
    logger->Info("SSAO Dynamic loaded - waiting for shader injection...");
}
