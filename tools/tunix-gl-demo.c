/*
 * tunix-gl-demo -- prove that the Mesa port renders, and put the result on
 * screen.
 *
 * Tunix has no DRM device and no window system, so there is no surface for GL
 * to present to. The path that does work is:
 *
 *   EGL surfaceless platform -> GLES2 context -> render into a framebuffer
 *   object -> glReadPixels -> blit into the mmap()ed /dev/fb0.
 *
 * The blit is the same protocol src/userspace/fb_test.c uses: TUNIX_FBIO_GET_INFO
 * to learn the pixel layout, TUNIX_FBIO_SET_MODE to take the display away from
 * the text console, then TUNIX_FBIO_FLUSH after each frame.
 *
 * --probe renders one frame into memory and reports the GL strings without
 * touching /dev/fb0, so the port's build script can run this on the build host
 * where no framebuffer exists.
 */

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <tunix/framebuffer.h>

#define PROBE_WIDTH 256
#define PROBE_HEIGHT 256

static const char *const VERTEX_SHADER =
    "attribute vec2 position;\n"
    "attribute vec3 color;\n"
    "uniform float angle;\n"
    "varying vec3 vertex_color;\n"
    "void main() {\n"
    "    float s = sin(angle);\n"
    "    float c = cos(angle);\n"
    "    vec2 rotated = vec2(position.x * c - position.y * s,\n"
    "                        position.x * s + position.y * c);\n"
    "    vertex_color = color;\n"
    "    gl_Position = vec4(rotated, 0.0, 1.0);\n"
    "}\n";

static const char *const FRAGMENT_SHADER =
    "precision mediump float;\n"
    "varying vec3 vertex_color;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(vertex_color, 1.0);\n"
    "}\n";

struct gl_state {
    EGLDisplay display;
    EGLContext context;
    GLuint program;
    GLuint framebuffer;
    GLuint color_texture;
    GLuint depth_buffer;
    GLint angle_location;
    GLint position_location;
    GLint color_location;
    unsigned width;
    unsigned height;
};

static void report(const char *message) {
    fprintf(stderr, "tunix-gl-demo: %s\n", message);
}

static void report_egl(const char *message) {
    fprintf(stderr, "tunix-gl-demo: %s (EGL error 0x%04x)\n", message,
            (unsigned)eglGetError());
}

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];
        GLsizei length = 0;
        glGetShaderInfoLog(shader, (GLsizei)sizeof(log), &length, log);
        fprintf(stderr, "tunix-gl-demo: shader compilation failed: %.*s\n",
                (int)length, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint build_program(void) {
    GLuint vertex = compile_shader(GL_VERTEX_SHADER, VERTEX_SHADER);
    if (!vertex) return 0;
    GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SHADER);
    if (!fragment) {
        glDeleteShader(vertex);
        return 0;
    }

    GLuint program = glCreateProgram();
    if (program) {
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);

        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[512];
            GLsizei length = 0;
            glGetProgramInfoLog(program, (GLsizei)sizeof(log), &length, log);
            fprintf(stderr, "tunix-gl-demo: program link failed: %.*s\n",
                    (int)length, log);
            glDeleteProgram(program);
            program = 0;
        }
    }

    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return program;
}

/*
 * The surfaceless platform has no window and no native pixmap, so the context
 * is made current without any surface and all rendering goes to an FBO. That
 * requires EGL_KHR_surfaceless_context, which mesa's surfaceless platform
 * always advertises; check anyway so a misconfigured build fails with a clear
 * message instead of an opaque eglMakeCurrent error.
 */
static int gl_state_init(struct gl_state *state, unsigned width, unsigned height) {
    memset(state, 0, sizeof(*state));
    state->display = EGL_NO_DISPLAY;
    state->context = EGL_NO_CONTEXT;
    state->width = width;
    state->height = height;

    state->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (state->display == EGL_NO_DISPLAY) {
        report_egl("no EGL display");
        return -1;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(state->display, &major, &minor)) {
        report_egl("eglInitialize failed");
        return -1;
    }

    const char *extensions = eglQueryString(state->display, EGL_EXTENSIONS);
    if (!extensions || !strstr(extensions, "EGL_KHR_surfaceless_context")) {
        report("EGL_KHR_surfaceless_context is not available");
        return -1;
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        report_egl("eglBindAPI(EGL_OPENGL_ES_API) failed");
        return -1;
    }

    /* EGL_SURFACE_TYPE has to be spelled out. Its default in eglChooseConfig is
     * EGL_WINDOW_BIT, and the surfaceless platform has no window configs at
     * all, so leaving it out matches nothing and eglChooseConfig reports
     * success with a count of zero. We never create a surface either way -- the
     * context is made current bare and rendering goes to an FBO -- so any
     * config that can back an ES2 context will do. */
    static const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config = NULL;
    EGLint config_count = 0;
    if (!eglChooseConfig(state->display, config_attributes, &config, 1,
                         &config_count) || config_count < 1) {
        report_egl("no usable EGL config");
        return -1;
    }

    static const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    state->context = eglCreateContext(state->display, config, EGL_NO_CONTEXT,
                                      context_attributes);
    if (state->context == EGL_NO_CONTEXT) {
        report_egl("eglCreateContext failed");
        return -1;
    }

    if (!eglMakeCurrent(state->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        state->context)) {
        report_egl("eglMakeCurrent failed");
        return -1;
    }

    glGenFramebuffers(1, &state->framebuffer);
    glGenTextures(1, &state->color_texture);
    glGenRenderbuffers(1, &state->depth_buffer);

    /* A texture rather than a renderbuffer for the colour attachment: the only
     * colour-renderable renderbuffer formats in core GLES2 are RGB565/RGBA4/
     * RGB5_A1, and RGBA8 needs the OES_rgb8_rgba8 extension. An RGBA/UNSIGNED_
     * BYTE texture is core, so this keeps full 8-bit channels without depending
     * on an extension. Non-power-of-two sizes need CLAMP_TO_EDGE and a non-
     * mipmapped filter to be complete in ES2. */
    glBindTexture(GL_TEXTURE_2D, state->color_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindRenderbuffer(GL_RENDERBUFFER, state->depth_buffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                          (GLsizei)width, (GLsizei)height);

    glBindFramebuffer(GL_FRAMEBUFFER, state->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, state->color_texture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, state->depth_buffer);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        report("the render target framebuffer is incomplete");
        return -1;
    }

    state->program = build_program();
    if (!state->program) return -1;

    state->angle_location = glGetUniformLocation(state->program, "angle");
    state->position_location = glGetAttribLocation(state->program, "position");
    state->color_location = glGetAttribLocation(state->program, "color");
    if (state->position_location < 0 || state->color_location < 0) {
        report("shader attributes were optimised away");
        return -1;
    }

    glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    glEnable(GL_DEPTH_TEST);
    return 0;
}

static void gl_state_finish(struct gl_state *state) {
    if (state->display == EGL_NO_DISPLAY) return;
    if (state->context != EGL_NO_CONTEXT) {
        eglMakeCurrent(state->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroyContext(state->display, state->context);
    }
    eglTerminate(state->display);
}

static void render_frame(struct gl_state *state, float angle) {
    static const GLfloat positions[] = {
         0.0f,  0.75f,
        -0.65f, -0.5f,
         0.65f, -0.5f,
    };
    static const GLfloat colors[] = {
        1.0f, 0.25f, 0.25f,
        0.25f, 1.0f, 0.35f,
        0.3f, 0.45f, 1.0f,
    };

    /* A slowly shifting clear colour makes it obvious the frames are live. */
    glClearColor(0.05f + 0.05f * sinf(angle), 0.06f, 0.12f, 1.0f);
    glClear((GLbitfield)GL_COLOR_BUFFER_BIT | (GLbitfield)GL_DEPTH_BUFFER_BIT);

    glUseProgram(state->program);
    glUniform1f(state->angle_location, angle);

    glVertexAttribPointer((GLuint)state->position_location, 2, GL_FLOAT,
                          GL_FALSE, 0, positions);
    glEnableVertexAttribArray((GLuint)state->position_location);
    glVertexAttribPointer((GLuint)state->color_location, 3, GL_FLOAT,
                          GL_FALSE, 0, colors);
    glEnableVertexAttribArray((GLuint)state->color_location);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
}

static uint32_t scale_component(uint32_t value, uint32_t mask_size) {
    if (!mask_size) return 0;
    uint32_t maximum = mask_size >= 32U ? UINT32_MAX : ((1U << mask_size) - 1U);
    return (value * maximum + 127U) / 255U;
}

/*
 * glReadPixels hands back tightly packed RGBA rows with the origin at the
 * bottom left; the framebuffer wants its own channel layout, its own pitch, and
 * the origin at the top left. Convert per pixel rather than assuming 0xAARRGGBB,
 * because the mode's masks are whatever the bootloader negotiated.
 */
static void blit_to_framebuffer(const struct tunix_fb_info *info,
                                uint8_t *destination, const uint8_t *pixels) {
    for (uint32_t y = 0; y < info->height; y++) {
        const uint8_t *source = pixels + (size_t)(info->height - 1U - y) * info->width * 4U;
        uint32_t *row = (uint32_t *)(void *)(destination + (size_t)y * info->pitch);
        for (uint32_t x = 0; x < info->width; x++) {
            row[x] = (scale_component(source[x * 4U + 0U], info->red_mask_size)
                          << info->red_field_position) |
                     (scale_component(source[x * 4U + 1U], info->green_mask_size)
                          << info->green_field_position) |
                     (scale_component(source[x * 4U + 2U], info->blue_mask_size)
                          << info->blue_field_position);
        }
    }
}

static void print_gl_strings(void) {
    printf("vendor=%s\n", (const char *)glGetString(GL_VENDOR));
    printf("renderer=%s\n", (const char *)glGetString(GL_RENDERER));
    printf("version=%s\n", (const char *)glGetString(GL_VERSION));
}

static int run_probe(void) {
    struct gl_state state;
    if (gl_state_init(&state, PROBE_WIDTH, PROBE_HEIGHT) != 0) {
        gl_state_finish(&state);
        return 1;
    }

    render_frame(&state, 0.0f);

    uint8_t *pixels = malloc((size_t)PROBE_WIDTH * PROBE_HEIGHT * 4U);
    if (!pixels) {
        report("out of memory");
        gl_state_finish(&state);
        return 1;
    }
    glReadPixels(0, 0, PROBE_WIDTH, PROBE_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    /* The triangle covers the middle of the target, so the centre pixel must
     * differ from the clear colour. Without this the probe would pass on a
     * driver that silently renders nothing. */
    const uint8_t *centre = pixels + ((size_t)(PROBE_HEIGHT / 2) * PROBE_WIDTH +
                                      PROBE_WIDTH / 2) * 4U;
    int drew_something = centre[0] > 32 || centre[1] > 32 || centre[2] > 64;

    print_gl_strings();
    printf("centre_pixel=%02x%02x%02x\n", centre[0], centre[1], centre[2]);
    free(pixels);
    gl_state_finish(&state);

    if (!drew_something) {
        report("the triangle was not rasterised");
        return 1;
    }
    printf("tunix-gl-demo: PASS offscreen render\n");
    return 0;
}

static int run_framebuffer(unsigned frames) {
    int status = 1;
    int fd = -1;
    int graphics_active = 0;
    void *mapping = MAP_FAILED;
    uint8_t *pixels = NULL;
    struct gl_state state;
    struct tunix_fb_info info;

    memset(&state, 0, sizeof(state));
    state.display = EGL_NO_DISPLAY;

    fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        report("cannot open /dev/fb0");
        goto cleanup;
    }

    memset(&info, 0, sizeof(info));
    if (ioctl(fd, TUNIX_FBIO_GET_INFO, &info) < 0 ||
        info.abi_version != TUNIX_FB_ABI_VERSION ||
        info.bits_per_pixel != 32U || !info.framebuffer_size ||
        !info.mapping_size || info.memory_offset > info.mapping_size ||
        info.framebuffer_size > info.mapping_size - info.memory_offset) {
        report("unsupported framebuffer ABI");
        goto cleanup;
    }

    if (gl_state_init(&state, info.width, info.height) != 0) goto cleanup;

    pixels = malloc((size_t)info.width * info.height * 4U);
    if (!pixels) {
        report("out of memory");
        goto cleanup;
    }

    uint32_t mode = TUNIX_FB_MODE_GRAPHICS;
    if (ioctl(fd, TUNIX_FBIO_SET_MODE, &mode) < 0) {
        report("framebuffer is busy or not writable");
        goto cleanup;
    }
    graphics_active = 1;

    mapping = mmap(NULL, (size_t)info.mapping_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        report("cannot map the framebuffer");
        goto cleanup;
    }

    for (unsigned frame = 0; frame < frames; frame++) {
        render_frame(&state, (float)frame * 0.08f);
        glReadPixels(0, 0, (GLsizei)info.width, (GLsizei)info.height,
                     GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        blit_to_framebuffer(&info, (uint8_t *)mapping + info.memory_offset, pixels);
        (void)ioctl(fd, TUNIX_FBIO_FLUSH, NULL);
    }

    status = 0;

cleanup:
    if (graphics_active) {
        uint32_t console_mode = TUNIX_FB_MODE_CONSOLE;
        (void)ioctl(fd, TUNIX_FBIO_SET_MODE, &console_mode);
    }
    if (mapping != MAP_FAILED) (void)munmap(mapping, (size_t)info.mapping_size);
    free(pixels);
    /* Report the GL strings before tearing the context down, and only once the
     * console has the display back so the text is actually visible. */
    if (status == 0) print_gl_strings();
    gl_state_finish(&state);
    if (fd >= 0) (void)close(fd);
    if (status == 0) printf("tunix-gl-demo: PASS framebuffer render\n");
    return status;
}

int main(int argc, char **argv) {
    unsigned frames = 60;
    int probe = 0;

    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--probe") == 0) {
            probe = 1;
        } else if (strcmp(argv[index], "--frames") == 0 && index + 1 < argc) {
            long value = strtol(argv[++index], NULL, 10);
            if (value < 1 || value > 100000) {
                report("--frames takes a value between 1 and 100000");
                return 2;
            }
            frames = (unsigned)value;
        } else {
            fprintf(stderr, "usage: %s [--probe] [--frames N]\n", argv[0]);
            return 2;
        }
    }

    return probe ? run_probe() : run_framebuffer(frames);
}
