#ifndef VIDEO_PLAYER_H
#define VIDEO_PLAYER_H

#include <SDL3/SDL.h>

#ifdef RPI
#include <mmal.h>
#include <mmal_component.h>
#include <mmal_connection.h>
#include <mmal_pool.h>
#include <mmal_queue.h>
#include <mmal_util.h>
#include <mmal_util_params.h>
#include <mmal_vc_client.h>
#endif
#ifdef RPI64
#include <linux/videodev2.h>
#include <libv4l2.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

// Function declarations
void handlePlaybackSpeed(double& playbackSpeed, const SDL_Event& event, SDL_Renderer* renderer, SDL_Window* window);
int calculateFrameDelay(double playbackSpeed, double frameRate);

#ifdef RPI
void initializeMMAL();
void finalizeMMAL();
void decodeFrameWithMMAL(const AVFrame* frame);
#endif
/*
#ifdef RPI64
void initializeV4L2(int width, int height); // Modified declaration
void finalizeV4L2();
void decodeFrameWithV4L2(const AVFrame* frame);
#endif
*/

#endif // VIDEO_PLAYER_H