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
extern void (APIENTRY *glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
extern void (APIENTRY *glGetShaderiv)(GLuint, GLenum, GLint*);
extern void (APIENTRY *glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
extern void (APIENTRY *glGetProgramiv)(GLuint, GLenum, GLint*);
extern void (APIENTRY *glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
extern void (APIENTRY *glGenerateMipmap)(GLenum);
extern void (APIENTRY *glActiveTexture)(GLenum);

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

bool LoadGLFunctions();
