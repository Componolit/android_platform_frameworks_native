/* 
 ** Copyright 2007, The Android Open Source Project
 **
 ** Licensed under the Apache License, Version 2.0 (the "License"); 
 ** you may not use this file except in compliance with the License. 
 ** You may obtain a copy of the License at 
 **
 **     http://www.apache.org/licenses/LICENSE-2.0 
 **
 ** Unless required by applicable law or agreed to in writing, software 
 ** distributed under the License is distributed on an "AS IS" BASIS, 
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
 ** See the License for the specific language governing permissions and 
 ** limitations under the License.
 */

#include <ctype.h>
#include <string.h>
#include <errno.h>

#include <sys/ioctl.h>

#include <GLES/gl.h>
#include <GLES/glext.h>

#include <cutils/log.h>
#include <cutils/properties.h>

#include "hooks.h"
#include "egl_impl.h"

using namespace android;

// set this to 1 for crude GL debugging
#define CHECK_FOR_GL_ERRORS     0

// ----------------------------------------------------------------------------
// extensions for the framework
// ----------------------------------------------------------------------------

extern "C" {
GL_API void GL_APIENTRY glColorPointerBounds(GLint size, GLenum type, GLsizei stride,
        const GLvoid *ptr, GLsizei count);
GL_API void GL_APIENTRY glNormalPointerBounds(GLenum type, GLsizei stride,
        const GLvoid *pointer, GLsizei count);
GL_API void GL_APIENTRY glTexCoordPointerBounds(GLint size, GLenum type,
        GLsizei stride, const GLvoid *pointer, GLsizei count);
GL_API void GL_APIENTRY glVertexPointerBounds(GLint size, GLenum type,
        GLsizei stride, const GLvoid *pointer, GLsizei count);
GL_API void GL_APIENTRY glPointSizePointerOESBounds(GLenum type,
        GLsizei stride, const GLvoid *pointer, GLsizei count);
GL_API void GL_APIENTRY glMatrixIndexPointerOESBounds(GLint size, GLenum type,
        GLsizei stride, const GLvoid *pointer, GLsizei count);
GL_API void GL_APIENTRY glWeightPointerOESBounds(GLint size, GLenum type,
        GLsizei stride, const GLvoid *pointer, GLsizei count);
}

void glColorPointerBounds(GLint size, GLenum type, GLsizei stride,
        const GLvoid *ptr, GLsizei count) {
    glColorPointer(size, type, stride, ptr);
}
void glNormalPointerBounds(GLenum type, GLsizei stride,
        const GLvoid *pointer, GLsizei count) {
    glNormalPointer(type, stride, pointer);
}
void glTexCoordPointerBounds(GLint size, GLenum type,
        GLsizei stride, const GLvoid *pointer, GLsizei count) {
    glTexCoordPointer(size, type, stride, pointer);
}
void glVertexPointerBounds(GLint size, GLenum type,
        GLsizei stride, const GLvoid *pointer, GLsizei count) {
    glVertexPointer(size, type, stride, pointer);
}

void GL_APIENTRY glPointSizePointerOESBounds(GLenum type,
        GLsizei stride, const GLvoid *pointer, GLsizei count) {
    glPointSizePointerOES(type, stride, pointer);
}

GL_API void GL_APIENTRY glMatrixIndexPointerOESBounds(GLint size, GLenum type,
        GLsizei stride, const GLvoid *pointer, GLsizei count) {
    glMatrixIndexPointerOES(size, type, stride, pointer);
}

GL_API void GL_APIENTRY glWeightPointerOESBounds(GLint size, GLenum type,
        GLsizei stride, const GLvoid *pointer, GLsizei count) {
    glWeightPointerOES(size, type, stride, pointer);
}

// ----------------------------------------------------------------------------
// Actual GL entry-points
// ----------------------------------------------------------------------------

#undef API_ENTRY
#undef CALL_GL_API
#undef CALL_GL_API_RETURN

#if USE_FAST_TLS_KEY && !CHECK_FOR_GL_ERRORS

    #if defined(__i386__)
        /* Tell gcc 4.4+ to always omit the frame pointer to have working code
         * independ of compiler flag
         */
        #define API_ENTRY(_api) __attribute__((noinline))            \
                __attribute__((optimize("omit-frame-pointer")))      \
                _api

        /* pop %%ebp only because x86 doesn't support naked functions */
        /* 4.4+ supports __attribute__((optimize)) that disable function
         * prologues
         */
        #if __GNUC__ < 4 || \
        (__GNUC__ == 4 && __GNUC_MINOR__ < 4 )
             #define PROLOGUE  "pop %%ebp   \n"
        #else
             #define PROLOGUE  ""
        #endif

        #define CALL_GL_API(_api, ...)                         \
            asm volatile(                                           \
                PROLOGUE                                            \
                "movl %%gs:0, %%ecx         \n"                     \
                "movl %c[tls](%%ecx), %%eax \n"                     \
                "test %%eax, %%eax          \n"                     \
                "je out" #_api "            \n"                     \
                "movl %c[api](%%eax), %%ecx \n"                     \
                "test %%ecx, %%ecx          \n"                     \
                "je out" #_api "            \n"                     \
                "jmp *%%ecx                 \n"                     \
                "out" #_api ": ret          \n"                     \
                :                                                   \
                : [tls] "i"(TLS_SLOT_OPENGL_API*4),                 \
                  [api] "i"(__builtin_offsetof(gl_hooks_t,          \
                                               gl._api))            \
                :                                                   \
                );
    #else
        #ifdef HAVE_ARM_TLS_REGISTER
            #define GET_TLS(reg) \
                "mrc p15, 0, " #reg ", c13, c0, 3 \n"
        #else
            #define GET_TLS(reg) \
                "mov   " #reg ", #0xFFFF0FFF      \n"  \
                "ldr   " #reg ", [" #reg ", #-15] \n"
        #endif

        #define API_ENTRY(_api) __attribute__((naked)) _api

        #define CALL_GL_API(_api, ...)                              \
             asm volatile(                                          \
                GET_TLS(r12)                                        \
                "ldr   r12, [r12, %[tls]] \n"                       \
                "cmp   r12, #0            \n"                       \
                "ldrne pc,  [r12, %[api]] \n"                       \
                "mov   r0, #0             \n"                       \
                "bx    lr                 \n"                       \
                :                                                   \
                : [tls] "J"(TLS_SLOT_OPENGL_API*4),                 \
                  [api] "J"(__builtin_offsetof(gl_hooks_t, gl._api))    \
                :                                                   \
                );
    #endif

    #define CALL_GL_API_RETURN(_api, ...) \
        CALL_GL_API(_api, __VA_ARGS__) \
        return 0; // placate gcc's warnings. never reached.

#else

    #if CHECK_FOR_GL_ERRORS
    
        #define CHECK_GL_ERRORS(_api) \
            do { GLint err = glGetError(); \
                ALOGE_IF(err != GL_NO_ERROR, "%s failed (0x%04X)", #_api, err); \
            } while(false);

    #else

        #define CHECK_GL_ERRORS(_api) do { } while(false);

    #endif


    #define API_ENTRY(_api) _api

    #define CALL_GL_API(_api, ...)                                      \
        gl_hooks_t::gl_t const * const _c = &getGlThreadSpecific()->gl; \
        _c->_api(__VA_ARGS__);                                          \
        CHECK_GL_ERRORS(_api)

    #define CALL_GL_API_RETURN(_api, ...)                               \
        gl_hooks_t::gl_t const * const _c = &getGlThreadSpecific()->gl; \
        return _c->_api(__VA_ARGS__)

#endif


extern "C" {
#include "gl_api.in"
#include "glext_api.in"
}

#undef API_ENTRY
#undef CALL_GL_API
#undef CALL_GL_API_RETURN

/*
 * glGetString() is special because we expose some extensions in the wrapper
 */

extern "C" const GLubyte * __glGetString(GLenum name);

const GLubyte * glGetString(GLenum name)
{
    const GLubyte * ret = egl_get_string_for_current_context(name);
    if (ret == NULL) {
        ret = __glGetString(name);
    }
    return ret;
}
