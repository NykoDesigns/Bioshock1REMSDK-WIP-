#include "gl_funcs.h"
#include <SDL.h>

// Function pointer definitions (APIENTRY = __stdcall on Win32)
void (APIENTRY *glGenVertexArrays)(GLsizei, GLuint*) = nullptr;
void (APIENTRY *glDeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
void (APIENTRY *glBindVertexArray)(GLuint) = nullptr;
void (APIENTRY *glGenBuffers)(GLsizei, GLuint*) = nullptr;
void (APIENTRY *glDeleteBuffers)(GLsizei, const GLuint*) = nullptr;
void (APIENTRY *glBindBuffer)(GLenum, GLuint) = nullptr;
void (APIENTRY *glBufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
void (APIENTRY *glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
void (APIENTRY *glEnableVertexAttribArray)(GLuint) = nullptr;
GLuint (APIENTRY *glCreateShader)(GLenum) = nullptr;
void (APIENTRY *glDeleteShader)(GLuint) = nullptr;
void (APIENTRY *glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
void (APIENTRY *glCompileShader)(GLuint) = nullptr;
GLuint (APIENTRY *glCreateProgram)() = nullptr;
void (APIENTRY *glDeleteProgram)(GLuint) = nullptr;
void (APIENTRY *glAttachShader)(GLuint, GLuint) = nullptr;
void (APIENTRY *glLinkProgram)(GLuint) = nullptr;
void (APIENTRY *glUseProgram)(GLuint) = nullptr;
GLint (APIENTRY *glGetUniformLocation)(GLuint, const GLchar*) = nullptr;
void (APIENTRY *glUniform1i)(GLint, GLint) = nullptr;
void (APIENTRY *glUniform3f)(GLint, GLfloat, GLfloat, GLfloat) = nullptr;
void (APIENTRY *glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;
void (APIENTRY *glGetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
void (APIENTRY *glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
void (APIENTRY *glGetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
void (APIENTRY *glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
void (APIENTRY *glGenerateMipmap)(GLenum) = nullptr;
void (APIENTRY *glActiveTexture)(GLenum) = nullptr;

#define LOAD(name) name = (decltype(name))SDL_GL_GetProcAddress(#name); if(!name) return false;

bool LoadGLFunctions()
{
    LOAD(glGenVertexArrays);
    LOAD(glDeleteVertexArrays);
    LOAD(glBindVertexArray);
    LOAD(glGenBuffers);
    LOAD(glDeleteBuffers);
    LOAD(glBindBuffer);
    LOAD(glBufferData);
    LOAD(glVertexAttribPointer);
    LOAD(glEnableVertexAttribArray);
    LOAD(glCreateShader);
    LOAD(glDeleteShader);
    LOAD(glShaderSource);
    LOAD(glCompileShader);
    LOAD(glCreateProgram);
    LOAD(glDeleteProgram);
    LOAD(glAttachShader);
    LOAD(glLinkProgram);
    LOAD(glUseProgram);
    LOAD(glGetUniformLocation);
    LOAD(glUniform1i);
    LOAD(glUniform3f);
    LOAD(glUniformMatrix4fv);
    LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog);
    LOAD(glGetProgramiv);
    LOAD(glGetProgramInfoLog);
    LOAD(glGenerateMipmap);
    LOAD(glActiveTexture);
    return true;
}
