#pragma once

#include <GL/gl3w.h>

void register_debug_callback();
#ifdef _WIN32
void APIENTRY debug_callback(GLenum src, GLenum type, GLuint id, GLenum severity,
	GLsizei len, const GLchar *msg, const GLvoid *user);
#else
void debug_callback(GLenum src, GLenum type, GLuint id, GLenum severity,
	GLsizei len, const GLchar *msg, const GLvoid *user);
#endif
void log_debug_msg(GLenum src, GLenum type, GLuint id, GLenum severity, GLsizei len, const GLchar *msg);

