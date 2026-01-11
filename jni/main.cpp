#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <GLES3/gl3.h>
#include <string.h>

MYMOD(net.ssao.brutal, SSAO_Brutal, 3.0, ntyzsky)
NEEDGAME(com.rockstargames.gtasa)

#define SHADER_LEN 65536

uintptr_t pGTASA = 0;
void* hGTASA = NULL;

// G-Buffer
GLuint gBuffer = 0;
GLuint gDepthTex = 0;
GLuint gNormalTex = 0;
GLuint gColorTex = 0;

// SSAO
GLuint ssaoFBO = 0;
GLuint ssaoTex = 0;
GLuint ssaoProgram = 0;
GLuint ssaoVAO = 0;
GLuint ssaoVBO = 0;

// Blit
GLuint blitProgram = 0;
GLuint blitVAO = 0;
GLuint blitVBO = 0;

int screenWidth = 0;
int screenHeight = 0;
bool initialized = false;
bool inGeometryPass = false;

// Game vars
uint32_t* m_snTimeInMilliseconds;

// Game functions
typedef struct RwMatrix {
    float right[3];
    uint32_t flags;
    float up[3];
    uint32_t pad1;
    float at[3];
    uint32_t pad2;
    float pos[3];
    uint32_t pad3;
} RwMatrix;

typedef struct RwCamera {
    char _pad[0x60];
    RwMatrix viewMatrix;
} RwCamera;

struct CPlaceable {
    char _pad[0x4];
    RwMatrix* m_matrix;
};

struct CCamera {
    CPlaceable placeable;
    char _pad[0x730];
    RwCamera* pRwCamera;
    char _pad2[0x1C];
    RwMatrix mCameraMatrix;
    RwMatrix mCameraMatrixOld;
    RwMatrix mViewMatrix;
    RwMatrix mMatInverse;
};

typedef RwMatrix* (*RwMatrixInvert_t)(RwMatrix* out, const RwMatrix* in);
typedef float* (*GetCurrentViewMatrix_t)();
typedef float* (*GetCurrentProjectionMatrix_t)();

RwMatrixInvert_t RwMatrixInvert;
GetCurrentViewMatrix_t GetCurrentViewMatrix;
GetCurrentProjectionMatrix_t GetCurrentProjectionMatrix;

CCamera* TheCamera;

char modifiedFragShader[SHADER_LEN];
char modifiedVertShader[SHADER_LEN];

// Matrix inversion helper
void InvertMatrix4x4(const float* m, float* out) {
    float inv[16], det;
    
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    det = 1.0f / det;
    
    for(int i = 0; i < 16; i++) {
        out[i] = inv[i] * det;
    }
}

// Convert RwMatrix to GL matrix
void RwMatrixToGL(const RwMatrix* rw, float* gl) {
    gl[0] = rw->right[0]; gl[4] = rw->up[0]; gl[8] = rw->at[0];  gl[12] = rw->pos[0];
    gl[1] = rw->right[1]; gl[5] = rw->up[1]; gl[9] = rw->at[1];  gl[13] = rw->pos[1];
    gl[2] = rw->right[2]; gl[6] = rw->up[2]; gl[10] = rw->at[2]; gl[14] = rw->pos[2];
    gl[3] = 0.0f;         gl[7] = 0.0f;      gl[11] = 0.0f;      gl[15] = 1.0f;
}

void InjectNormalOutput(char* fragShader) {
    char* mainPos = strstr(fragShader, "void main()");
    if(!mainPos) return;
    
    char* bracePos = strchr(mainPos, '{');
    if(!bracePos) return;
    
    if(strstr(fragShader, "layout(location = 0) out")) return;
    
    size_t preMainLen = mainPos - fragShader;
    strncpy(modifiedFragShader, fragShader, preMainLen);
    modifiedFragShader[preMainLen] = '\0';
    
    strcat(modifiedFragShader, 
        "layout(location = 0) out vec4 gNormal;\n"
        "layout(location = 1) out vec4 gColor;\n"
        "varying vec3 vNormal;\n\n");
    
    strncat(modifiedFragShader, mainPos, bracePos - mainPos + 1);
    strcat(modifiedFragShader, "\n    gNormal = vec4(normalize(vNormal) * 0.5 + 0.5, 1.0);\n");
    
    char* restOfShader = bracePos + 1;
    char temp[SHADER_LEN];
    strcpy(temp, restOfShader);
    
    char* searchPos = temp;
    char* glFragColorPos;
    while((glFragColorPos = strstr(searchPos, "gl_FragColor")) != NULL) {
        *glFragColorPos = '\0';
        strcat(modifiedFragShader, searchPos);
        strcat(modifiedFragShader, "gColor");
        searchPos = glFragColorPos + 12;
    }
    strcat(modifiedFragShader, searchPos);
    
    strcpy(fragShader, modifiedFragShader);
    logger->Info("Injected normal output");
}

void InjectNormalVarying(char* vertShader) {
    if(strstr(vertShader, "vNormal")) return;
    
    char* normalAttr = strstr(vertShader, "attribute vec3 Normal");
    if(!normalAttr) {
        normalAttr = strstr(vertShader, "in vec3 Normal");
        if(!normalAttr) return;
    }
    
    char* mainPos = strstr(vertShader, "void main()");
    if(!mainPos) return;
    
    size_t preMainLen = mainPos - vertShader;
    strncpy(modifiedVertShader, vertShader, preMainLen);
    modifiedVertShader[preMainLen] = '\0';
    
    strcat(modifiedVertShader, "varying vec3 vNormal;\n\n");
    
    char* bracePos = strchr(mainPos, '{');
    if(!bracePos) return;
    
    strncat(modifiedVertShader, mainPos, bracePos - mainPos + 1);
    strcat(modifiedVertShader, "\n    vNormal = normalize(Normal);\n");
    strcat(modifiedVertShader, bracePos + 1);
    
    strcpy(vertShader, modifiedVertShader);
    logger->Info("Injected normal varying");
}

DECL_HOOK(int, RQShaderBuildSource, int flags, char** pxlsrc, char** vtxsrc) {
    int ret = RQShaderBuildSource(flags, pxlsrc, vtxsrc);
    
    bool isWorldShader = (flags & 0x10000000) || (flags & 0x20) || (flags & 0x400);
    
    if(isWorldShader && inGeometryPass) {
        InjectNormalVarying(*vtxsrc);
        InjectNormalOutput(*pxlsrc);
    }
    
    return ret;
}

const char* ssaoVertShader = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;

void main() {
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPos, 1.0);
}
)";

const char* ssaoFragShader = R"(#version 300 es
precision highp float;

in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D gDepth;
uniform sampler2D gNormal;
uniform sampler2D gColor;
uniform mat4 invProjMatrix;
uniform mat4 invViewMatrix;
uniform vec2 uScreenSize;
uniform float uTime;

const int SAMPLES = 16;
const float AO_RADIUS = 0.5;
const float SSAO_DENSITY = 1.0;

vec4 wPos(vec2 uv) {
    float depth = texture(gDepth, uv).r;
    if(depth >= 1.0) return vec4(0.0);
    
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos = invProjMatrix * ndc;
    viewPos /= viewPos.w;
    vec4 worldPos = invViewMatrix * viewPos;
    
    return vec4(worldPos.xyz, depth);
}

float SAO(vec3 worldpos, float curDepth, vec3 normal) {
    vec2 coord = vTexCoord;
    
    if(curDepth >= 1.0) return 1.0;
    
    float d0 = curDepth;
    float LinDepth = 1.0 * 0.05 / (1.0 + d0 * (0.05 - 1.0));
    
    vec3 pos = worldpos;
    vec3 n2 = normal;
    
    vec2 uv;
    float ao = 0.0;
    float sr = -2.5;
    float SSAO_SAMPLES = float(SAMPLES);
    float d = AO_RADIUS / (SSAO_SAMPLES * (n2.z + sr));
    
    vec2 dir;
    float angle = 42.528;
    dir.x = sin(angle);
    dir.y = cos(angle);
    dir *= d;
    
    float radius = LinDepth;
    float AO_Rad = 0.5 * LinDepth;
    AO_Rad = 1.0 - AO_Rad;
    AO_Rad = min(AO_Rad, 0.14);
    
    radius = mix(0.7 * AO_Rad, 0.16 * AO_Rad, radius);
    
    mat2 rotMat = mat2(0.76465, 0.64444, -0.64444, 0.76465);
    
    for(float dx = 1.0; dx < SSAO_SAMPLES; dx += 1.0) {
        uv = coord.xy + (dir.xy * dx / 0.5) * radius;
        
        if(uv.x > 1.0 || uv.x < 0.0 || uv.y > 1.0 || uv.y < 0.0)
            break;
        
        vec4 nwp = wPos(uv);
        
        vec3 occlusion_vector = normalize(nwp.xyz - pos);
        float dist = length(nwp.xyz - pos);
        occlusion_vector *= 2.0;
        
        float AO = clamp(-dot(n2, occlusion_vector), 0.0, 1.0);
        ao += 3.0 * (AO * (1.0 - clamp(dist / 2.0, 0.0, 1.0))) * 
              clamp(1.0 - abs(nwp.w - curDepth) * 100.0, 0.0, 1.0);
        
        dir.xy = rotMat * dir.xy;
    }
    
    float result = ao / SSAO_SAMPLES;
    return pow(1.0 - pow(result, 1.0) * SSAO_DENSITY, 1.0);
}

void main() {
    float depth = texture(gDepth, vTexCoord).r;
    
    if(depth >= 0.9999) {
        FragColor = texture(gColor, vTexCoord);
        return;
    }
    
    vec3 normal = normalize(texture(gNormal, vTexCoord).rgb * 2.0 - 1.0);
    vec4 worldPosDepth = wPos(vTexCoord);
    vec3 worldPos = worldPosDepth.xyz;
    vec4 color = texture(gColor, vTexCoord);
    
    float ao = SAO(worldPos, depth, normal);
    
    vec2 vignette = abs(vTexCoord - 0.5) * 2.0;
    float vig = 1.0 - pow(max(vignette.x, vignette.y), 3.0) * 0.2;
    
    FragColor = vec4(color.rgb * ao * vig, color.a);
}
)";

const char* blitVertShader = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;

void main() {
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPos, 1.0);
}
)";

const char* blitFragShader = R"(#version 300 es
precision highp float;

in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D screenTexture;

void main() {
    FragColor = texture(screenTexture, vTexCoord);
}
)";

GLuint CompileShader(const char* vertSrc, const char* fragSrc, const char* name) {
    char infoLog[1024];
    GLint success;
    
    GLuint vert = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vert, 1, &vertSrc, NULL);
    glCompileShader(vert);
    glGetShaderiv(vert, GL_COMPILE_STATUS, &success);
    if(!success) {
        glGetShaderInfoLog(vert, 1024, NULL, infoLog);
        logger->Error("[%s] Vert: %s", name, infoLog);
        return 0;
    }
    
    GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(frag, 1, &fragSrc, NULL);
    glCompileShader(frag);
    glGetShaderiv(frag, GL_COMPILE_STATUS, &success);
    if(!success) {
        glGetShaderInfoLog(frag, 1024, NULL, infoLog);
        logger->Error("[%s] Frag: %s", name, infoLog);
        return 0;
    }
    
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if(!success) {
        logger->Error("[%s] Link fail", name);
        return 0;
    }
    
    glDeleteShader(vert);
    glDeleteShader(frag);
    
    logger->Info("[%s] OK: %d", name, prog);
    return prog;
}

void SetupBuffers(int width, int height) {
    screenWidth = width;
    screenHeight = height;
    
    logger->Info("Setup: %dx%d", width, height);
    
    glGenFramebuffers(1, &gBuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, gBuffer);
    
    glGenTextures(1, &gDepthTex);
    glBindTexture(GL_TEXTURE_2D, gDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, gDepthTex, 0);
    
    glGenTextures(1, &gNormalTex);
    glBindTexture(GL_TEXTURE_2D, gNormalTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_HALF_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gNormalTex, 0);
    
    glGenTextures(1, &gColorTex);
    glBindTexture(GL_TEXTURE_2D, gColorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, gColorTex, 0);
    
    GLenum drawBuffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, drawBuffers);
    
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        logger->Error("G-Buffer fail!");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    glGenFramebuffers(1, &ssaoFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO);
    glGenTextures(1, &ssaoTex);
    glBindTexture(GL_TEXTURE_2D, ssaoTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ssaoTex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    ssaoProgram = CompileShader(ssaoVertShader, ssaoFragShader, "SSAO");
    blitProgram = CompileShader(blitVertShader, blitFragShader, "Blit");
    
    float quadVerts[] = {
        -1.0f,  1.0f, 0.0f,  0.0f, 1.0f,
        -1.0f, -1.0f, 0.0f,  0.0f, 0.0f,
         1.0f, -1.0f, 0.0f,  1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f,  0.0f, 1.0f,
         1.0f, -1.0f, 0.0f,  1.0f, 0.0f,
         1.0f,  1.0f, 0.0f,  1.0f, 1.0f
    };
    
    glGenVertexArrays(1, &ssaoVAO);
    glGenBuffers(1, &ssaoVBO);
    glBindVertexArray(ssaoVAO);
    glBindBuffer(GL_ARRAY_BUFFER, ssaoVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
    glBindVertexArray(0);
    
    glGenVertexArrays(1, &blitVAO);
    glGenBuffers(1, &blitVBO);
    glBindVertexArray(blitVAO);
    glBindBuffer(GL_ARRAY_BUFFER, blitVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
    glBindVertexArray(0);
    
    initialized = true;
    logger->Info("Ready!");
}

void RenderSSAO() {
    glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(ssaoProgram);
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gDepthTex);
    glUniform1i(glGetUniformLocation(ssaoProgram, "gDepth"), 0);
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gNormalTex);
    glUniform1i(glGetUniformLocation(ssaoProgram, "gNormal"), 1);
    
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, gColorTex);
    glUniform1i(glGetUniformLocation(ssaoProgram, "gColor"), 2);
    
    // Get matrices
    float* projMat = GetCurrentProjectionMatrix();
    float* viewMat = GetCurrentViewMatrix();
    
    if(projMat && viewMat) {
        float invProj[16], invView[16];
        InvertMatrix4x4(projMat, invProj);
        InvertMatrix4x4(viewMat, invView);
        
        glUniformMatrix4fv(glGetUniformLocation(ssaoProgram, "invProjMatrix"), 1, GL_FALSE, invProj);
        glUniformMatrix4fv(glGetUniformLocation(ssaoProgram, "invViewMatrix"), 1, GL_FALSE, invView);
    }
    
    glUniform2f(glGetUniformLocation(ssaoProgram, "uScreenSize"), (float)screenWidth, (float)screenHeight);
    
    if(m_snTimeInMilliseconds) {
        glUniform1f(glGetUniformLocation(ssaoProgram, "uTime"), (float)(*m_snTimeInMilliseconds));
    }
    
    glBindVertexArray(ssaoVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void BlitToScreen() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(blitProgram);
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ssaoTex);
    glUniform1i(glGetUniformLocation(blitProgram, "screenTexture"), 0);
    
    glBindVertexArray(blitVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

DECL_HOOKv(RenderScene, bool param) {
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    
    if(!initialized && viewport[2] > 0 && viewport[3] > 0) {
        SetupBuffers(viewport[2], viewport[3]);
    }
    
    if(initialized) {
        inGeometryPass = true;
        glBindFramebuffer(GL_FRAMEBUFFER, gBuffer);
        GLenum drawBuffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, drawBuffers);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    
    RenderScene(param);
    
    if(initialized) {
        inGeometryPass = false;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        RenderSSAO();
        BlitToScreen();
    }
}

extern "C" void OnModPreLoad() {
    logger->SetTag("SSAO");
}

extern "C" void OnModLoad() {
    logger->SetTag("SSAO");
    
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    
    if(!pGTASA || !hGTASA) {
        logger->Error("GTA:SA not found");
        return;
    }
    
    const GLubyte* version = glGetString(GL_VERSION);
    logger->Info("GL: %s", version);
    
    if(!strstr((const char*)version, "OpenGL ES 3.")) {
        logger->Error("Need GLES3!");
        return;
    }
    
    m_snTimeInMilliseconds = (uint32_t*)(pGTASA + 0x00953138);
    SET_TO(TheCamera, aml->GetSym(hGTASA, "TheCamera"));
    SET_TO(RwMatrixInvert, pGTASA + 0x001e3a28);
    SET_TO(GetCurrentViewMatrix, pGTASA + 0x001ba6e0);
    SET_TO(GetCurrentProjectionMatrix, pGTASA + 0x001ba6f8);
    
    logger->Info("Camera: %p", TheCamera);
    
    HOOKPLT(RQShaderBuildSource, pGTASA + 0x6720F8);
    HOOK(RenderScene, pGTASA + 0x003f609c);
    
    logger->Info("SSAO LOADED!");
}
