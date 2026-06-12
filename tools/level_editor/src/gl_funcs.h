#pragma once

// Minimal OpenGL 3.3 core function loader (no external dependencies)
// Only loads what the level editor actually needs.

#ifdef _WIN32
#include <Windows.h>
#endif
#include <GL/gl.h>

// GL 2.0+ types
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

// GL 2.0+ function pointers (APIENTRY = __stdcall on Windows, required for 32-bit)
extern void (APIENTRY *glGenVertexArrays)(GLsizei, GLuint*);
extern void (APIENTRY *glDeleteVertexArrays)(GLsizei, const GLuint*);
extern void (APIENTRY *glBindVertexArray)(GLuint);
extern void (APIENTRY *glGenBuffers)(GLsizei, GLuint*);
extern void (APIENTRY *glDeleteBuffers)(GLsizei, const GLuint*);
extern void (APIENTRY *glBindBuffer)(GLenum, GLuint);
extern void (APIENTRY *glBufferData)(GLenum, GLsizeiptr, const void*, GLenum);
extern void (APIENTRY *glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
extern void (APIENTRY *glEnableVertexAttribArray)(GLuint);
extern GLuint (APIENTRY *glCreateShader)(GLenum);
extern void (APIENTRY *glDeleteShader)(GLuint);
extern void (APIENTRY *glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
extern void (APIENTRY *glCompileShader)(GLuint);
extern GLuint (APIENTRY *glCreateProgram)();
extern void (APIENTRY *glDeleteProgram)(GLuint);
extern void (APIENTRY *glAttachShader)(GLuint, GLuint);
extern void (APIENTRY *glLinkProgram)(GLuint);
extern void (APIENTRY *glUseProgram)(GLuint);
extern GLint (APIENTRY *glGetUniformLocation)(GLuint, const GLchar*);
extern void (APIENTRY *glUniform1i)(GLint, GLint);
extern void (APIENTRY *glUniform1f)(GLint, GLfloat);
extern void (APIENTRY *glUniform2f)(GLint, GLfloat, GLfloat);
extern void (APIENTRY *glUniform3f)(GLint, GLfloat, GLfloat, GLfloat);
extern void (APIENTRY *glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
extern void (APIENTRY *glUniform3fv)(GLint, GLsizei, const GLfloat*);
extern void (APIENTRY *glUniform4fv)(GLint, GLsizei, const GLfloat*);
extern void (APIENTRY *glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
extern void (APIENTRY *glGetShaderiv)(GLuint, GLenum, GLint*);
extern void (APIENTRY *glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
extern void (APIENTRY *glGetProgramiv)(GLuint, GLenum, GLint*);
extern void (APIENTRY *glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
extern void (APIENTRY *glGenerateMipmap)(GLenum);
extern void (APIENTRY *glActiveTexture)(GLenum);

// Compressed texture upload (DXT1 lightmaps)
extern void (APIENTRY *glCompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*);

// Framebuffer (shadow mapping + bloom)
extern void (APIENTRY *glGenFramebuffers)(GLsizei, GLuint*);
extern void (APIENTRY *glDeleteFramebuffers)(GLsizei, const GLuint*);
extern void (APIENTRY *glBindFramebuffer)(GLenum, GLuint);
extern void (APIENTRY *glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
extern GLenum (APIENTRY *glCheckFramebufferStatus)(GLenum);
extern void (APIENTRY *glDrawBuffers)(GLsizei, const GLenum*);

// Renderbuffer
extern void (APIENTRY *glGenRenderbuffers)(GLsizei, GLuint*);
extern void (APIENTRY *glDeleteRenderbuffers)(GLsizei, const GLuint*);
extern void (APIENTRY *glBindRenderbuffer)(GLenum, GLuint);
extern void (APIENTRY *glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
extern void (APIENTRY *glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);

// Additional constants
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif

#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_TEXTURE2
#define GL_TEXTURE2 0x84C2
#endif
#ifndef GL_TEXTURE3
#define GL_TEXTURE3 0x84C3
#endif
#ifndef GL_TEXTURE4
#define GL_TEXTURE4 0x84C4
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

// Framebuffer constants
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#ifndef GL_DEPTH_COMPONENT
#define GL_DEPTH_COMPONENT 0x1902
#endif
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER 0x812D
#endif
#ifndef GL_TEXTURE_BORDER_COLOR
#define GL_TEXTURE_BORDER_COLOR 0x1004
#endif
#ifndef GL_TEXTURE_COMPARE_MODE
#define GL_TEXTURE_COMPARE_MODE 0x884C
#endif
#ifndef GL_NONE
#define GL_NONE 0
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_RG_RGTC2
#define GL_COMPRESSED_RG_RGTC2 0x8DBD
#endif

bool LoadGLFunctions();
