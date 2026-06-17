#pragma once
#include <iostream>

#if defined(__APPLE__)
    #include <OpenGL/gl3.h>
#else
    // Replace with your preferred extension loader header (glad, glew, etc.)
    #include <GL/glew.h> 
#endif

// Vertex Shader Source
const char* crtVertexShaderSource = R"glsl(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoords;

out vec2 TexCoords;

void main() {
    TexCoords = aTexCoords;
    gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
}
)glsl";

// Fragment Shader Source
const char* crtFragmentShaderSource = R"glsl(
#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D screenTexture;
uniform vec2 textureSize;
uniform vec2 windowSize;
uniform int isBorderPass;
uniform float uvYOffset;

void main() {
    vec2 uv = TexCoords;
    uv.y = (uv.y * 0.5) + uvYOffset;

    if (isBorderPass == 1) {
        float lastColumnX = (textureSize.x - 1.0) / textureSize.x;
        FragColor = texture(screenTexture, vec2(lastColumnX, uv.y));
    } else {
        vec2 cc = uv - vec2(0.5, uvYOffset + 0.25);
        uv += cc * (dot(cc, cc) * 0.05);

        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < uvYOffset || uv.y > (uvYOffset + 0.5)) {
            FragColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }

        vec4 baseColor = texture(screenTexture, uv);
        float scanline = sin((TexCoords.y * textureSize.y) * 3.14159265) * 0.4 + 0.6;
        FragColor = vec4(baseColor.rgb * scanline, 1.0);
    }
}
)glsl";

// Compilation helper
inline GLuint CompileCRTShader() {
    GLint success;
    char infoLog[512];

    // Compile Vertex Shader
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &crtVertexShaderSource, NULL);
    glCompileShader(vertexShader);
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(vertexShader, 512, NULL, infoLog);
        std::cerr << "Vertex Shader Compilation Failed:\n" << infoLog << std::endl;
    }

    // Compile Fragment Shader
    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &crtFragmentShaderSource, NULL);
    glCompileShader(fragmentShader);
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragmentShader, 512, NULL, infoLog);
        std::cerr << "Fragment Shader Compilation Failed:\n" << infoLog << std::endl;
    }

    // Link Shader Program
    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(program, 512, NULL, infoLog);
        std::cerr << "Shader Program Linking Failed:\n" << infoLog << std::endl;
    }

    // Clean up individual shader objects
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    return program;
}