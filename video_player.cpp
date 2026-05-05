#include <iostream>
#include <SDL3/SDL.h>
#include "video_player.h"
#include "utils.h"

#ifdef RPI
MMAL_COMPONENT_T* decoder = nullptr;
MMAL_POOL_T* pool = nullptr;

void initializeMMAL() {
    std::cout << "Initializing MMAL legacy decoder..." << std::endl;
    mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, &decoder);
    mmal_port_parameter_set_boolean(decoder->input[0], MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
    mmal_port_parameter_set_boolean(decoder->output[0], MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
    mmal_port_format_commit(decoder->input[0]);
    mmal_port_format_commit(decoder->output[0]);
    pool = mmal_port_pool_create(decoder->output[0], decoder->output[0]->buffer_num, decoder->output[0]->buffer_size);
    mmal_component_enable(decoder);
    std::cout << "MMAL ready." << std::endl;
}

void finalizeMMAL() {
    std::cout << "Finalizing MMAL" << std::endl;
    mmal_component_disable(decoder);
    mmal_port_pool_destroy(decoder->output[0], pool);
    mmal_component_destroy(decoder);
    std::cout << "MMAL finalized" << std::endl;
}

void decodeFrameWithMMAL(const AVFrame* frame) {
    std::cout << "Decoding frame with MMAL" << std::endl;
    MMAL_BUFFER_HEADER_T* buffer = mmal_queue_get(pool->queue);
    if (!buffer) {
        std::cerr << "Failed to get buffer from pool" << std::endl;
        return;
    }

    buffer->length = frame->linesize[0] * frame->height;
    memcpy(buffer->data, frame->data[0], buffer->length);
    mmal_port_send_buffer(decoder->input[0], buffer);
    mmal_port_send_buffer(decoder->output[0], buffer);
    std::cout << "Frame decoded with MMAL" << std::endl;
}
#endif

/*#ifdef RPI64
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <libv4l2.h>

int v4l2_fd = -1;
void* buffer = nullptr;
v4l2_buffer buf = {};
v4l2_plane planes[VIDEO_MAX_PLANES] = {};

void enumerateFormats(int fd, v4l2_buf_type type) {
    struct v4l2_fmtdesc fmtDesc = {};
    fmtDesc.type = type;
    fmtDesc.index = 0;
    std::cout << "Available formats for type " << type << ":" << std::endl;
    while (v4l2_ioctl(fd, VIDIOC_ENUM_FMT, &fmtDesc) == 0) {
        std::cout << "  " << fmtDesc.description << " (" << fmtDesc.pixelformat << ")" << std::endl;
        fmtDesc.index++;
    }
    if (fmtDesc.index == 0) {
        std::cout << "  No formats available" << std::endl;
    }
}

void initializeV4L2(int width, int height) {
    std::cout << "Initializing V4L2" << std::endl;
    v4l2_fd = v4l2_open("/dev/video10", O_RDWR);
    if (v4l2_fd < 0) {
        std::cerr << "Failed to open V4L2 device: " << strerror(errno) << std::endl;
        return;
    }

    v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;

    std::cout << "Setting V4L2 format" << std::endl;
    if (v4l2_ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
        std::cerr << "Failed to set V4L2 format: " << strerror(errno) << std::endl;
        std::cerr << "Type: " << fmt.type  << std::endl;
        std::cerr << "Width: " << width << ", Height: " << height << std::endl;
        std::cerr << "Pixelformat: " << fmt.fmt.pix_mp.pixelformat << ", field: " << fmt.fmt.pix_mp.field << std::endl;
        enumerateFormats(v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        enumerateFormats(v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        return;
    }

    v4l2_requestbuffers req = {};
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_DMABUF;

    std::cout << "Requesting V4L2 buffers" << std::endl;
    if (v4l2_ioctl(v4l2_fd, VIDIOC_REQBUFS, &req) < 0) {
        std::cerr << "Failed to request V4L2 buffers: " << strerror(errno) << std::endl;
        return;
    }

    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.index = 0;
    buf.m.planes = planes;
    buf.length = VIDEO_MAX_PLANES;

    std::cout << "Querying V4L2 buffer" << std::endl;
    if (v4l2_ioctl(v4l2_fd, VIDIOC_QUERYBUF, &buf) < 0) {
        std::cerr << "Failed to query V4L2 buffer: " << strerror(errno) << std::endl;
        return;
    }

    std::cout << "Buffer length: " << buf.m.planes[0].length << ", Buffer offset: " << buf.m.planes[0].m.mem_offset << std::endl;

    size_t buffer_size = width * height * 3 / 2;
    buf.m.planes[0].length = buffer_size;

    std::cout << "Mapping V4L2 buffer" << std::endl;
    buffer = v4l2_mmap(nullptr, buf.m.planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, v4l2_fd, buf.m.planes[0].m.mem_offset);
    if (buffer == MAP_FAILED) {
        std::cerr << "Failed to mmap V4L2 buffer: " << strerror(errno) << std::endl;
        return;
    }
    std::cout << "V4L2 initialized" << std::endl;
}

void finalizeV4L2() {
    std::cout << "Finalizing V4L2" << std::endl;
    if (buffer != MAP_FAILED) {
        v4l2_munmap(buffer, buf.m.planes[0].length);
    }
    if (v4l2_fd >= 0) {
        v4l2_close(v4l2_fd);
    }
    std::cout << "V4L2 finalized" << std::endl;
}

void decodeFrameWithV4L2(const AVFrame* frame) {
    std::cout << "Decoding frame with V4L2" << std::endl;
    if (v4l2_fd < 0 || buffer == MAP_FAILED) {
        std::cerr << "V4L2 not initialized" << std::endl;
        return;
    }

    std::cout << "Copying frame data to buffer" << std::endl;
    size_t frame_size = frame->width * frame->height * 3 / 2; // YUV420 frame size calculation
    std::cout << "Frame size: " << frame_size << ", Buffer size: " << buf.m.planes[0].length << std::endl;
    if (frame_size > buf.m.planes[0].length) {
        std::cerr << "Frame size is larger than buffer size" << std::endl;
        return;
    }
    memcpy(buffer, frame->data[0], frame_size);

    buf.m.planes[0].bytesused = frame_size;

    std::cout << "Queueing V4L2 buffer" << std::endl;
    if (v4l2_ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
        std::cerr << "Failed to queue V4L2 buffer: " << strerror(errno) << std::endl;
        return;
    }

    std::cout << "Starting V4L2 stream" << std::endl;
    if (v4l2_ioctl(v4l2_fd, VIDIOC_STREAMON, &buf.type) < 0) {
        std::cerr << "Failed to start V4L2 stream: " << strerror(errno) << std::endl;
        return;
    }

    std::cout << "Dequeuing V4L2 buffer" << std::endl;
    if (v4l2_ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
        std::cerr << "Failed to dequeue V4L2 buffer: " << strerror(errno) << std::endl;
        return;
    }
    std::cout << "Frame decoded successfully with V4L2" << std::endl;
}
#endif
*/

void handlePlaybackSpeed(double& playbackSpeed, const SDL_Event& event, SDL_Renderer* renderer, SDL_Window* window) {
    if (event.type == SDL_EVENT_KEY_DOWN) {
        switch (event.key.key) {
            case SDLK_LEFT:
                playbackSpeed -= 0.1;
                if (playbackSpeed < 0.1) {
                    playbackSpeed = 0.1;
                }
                std::cout << "Playback speed decreased to: " << playbackSpeed << "x" << std::endl;
                needsRedraw = true;
                break;
            case SDLK_RIGHT:
                playbackSpeed += 0.1;
                if (playbackSpeed > 4) {
                    playbackSpeed = 4;
                }
                std::cout << "Playback speed increased to: " << playbackSpeed << "x" << std::endl;
                needsRedraw = true;
                break;
            case SDLK_SPACE:
                playbackSpeed = 1.0;
                std::cout << "Playback speed reset to: " << playbackSpeed << "x" << std::endl;
                needsRedraw = true;
                break;
            case SDLK_F11:
                isFullscreen = !isFullscreen;
                SDL_SetWindowFullscreen(SDL_GetWindowFromID(event.window.windowID), isFullscreen);
                // Redraw text when switching to/from fullscreen mode
                updateTextTextures(renderer, window);
                break;
            case SDLK_ESCAPE:
                SDL_Event quitEvent;
                quitEvent.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quitEvent);
                break;
            case SDLK_I:
                showMessage = !showMessage;
                needsRedraw = true;
                break;
        }
    }
}

int calculateFrameDelay(double playbackSpeed, double frameRate) {
    if (playbackSpeed <= 0 || frameRate <= 0) {
        return 0;
    }
    return static_cast<int>(1000.0 / (frameRate * playbackSpeed));
}