#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <GLES2/gl2.h>
#include <string.h>

MYMOD(net.ssao.gtasa, SSAO_v200, 1.0, ntyzsky)
NEEDGAME(com.rockstargames.gtasa)

#define SHADER_LEN 32768

uintptr_t pGTASA = 0;
void* hGTASA = NULL;

const char* ssaoFragmentShader = R"(
precision mediump float;
varying vec2 v_texCoord0;
uniform sampler2D uColorTex;

float getDepth(vec2 uv) {
    vec3 col = texture2D(uColorTex, uv).rgb;
    return dot(col, vec3(0.299, 0.587, 0.114));
}

void main() {
    vec3 color = texture2D(uColorTex, v_texCoord0).rgb;
    float depth = getDepth(v_texCoord0);
    
    float ao = 0.0;
    vec2 pixelSize = vec2(1.0/1280.0, 1.0/720.0);
    
    for(int i = 0; i < 4; i++) {
        float angle = float(i) * 1.5708;
        vec2 offset = vec2(cos(angle), sin(angle)) * 2.0 * pixelSize;
        float sampleDepth = getDepth(v_texCoord0 + offset);
        ao += max(0.0, depth - sampleDepth);
    }
    
    ao = clamp(ao / 4.0, 0.0, 1.0);
    float occlusion = 1.0 - (ao * 0.7);
    
    gl_FragColor = vec4(color * occlusion, 1.0);
}
)";

char customFragShader[SHADER_LEN];
bool bInjected = false;

// RQShader::BuildSource hook
DECL_HOOK(int, RQShaderBuildSource, int flags, char** pxlsrc, char** vtxsrc) {
    int ret = RQShaderBuildSource(flags, pxlsrc, vtxsrc);
    
    static int count = 0;
    if(count < 15) {
        logger->Info("Shader flag: 0x%X", flags);
        count++;
    }
    
    if(!bInjected && (flags == 0x10 || flags == 0x200010 || flags == 0x4000010)) {
        logger->Info("✓ INJECTING SSAO! (flag=0x%X)", flags);
        strncpy(customFragShader, ssaoFragmentShader, SHADER_LEN - 1);
        customFragShader[SHADER_LEN - 1] = '\0';
        *pxlsrc = customFragShader;
        bInjected = true;
    }
    
    return ret;
}

extern "C" void OnModPreLoad() {
    logger->SetTag("SSAO");
    logger->Info("SSAO PreLoad");
}

extern "C" void OnModLoad() {
    logger->SetTag("SSAO");
    
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    
    if(!pGTASA || !hGTASA) {
        logger->Error("GTA:SA not found!");
        return;
    }
    
    logger->Info("Hooking RQShader::BuildSource...");
    
    // Hook by symbol name
    void* addr = (void*)aml->GetSym(hGTASA, "_ZN8RQShader11BuildSourceEjPPKcS2_");
    
    if(!addr) {
        logger->Error("Symbol not found! Using offset fallback...");
        addr = (void*)(pGTASA + 0x001CFA38);
    }
    
    HOOK(RQShaderBuildSource, addr);
    
    logger->Info("✓ SSAO v2.00 loaded!");
}
