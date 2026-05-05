#include <iostream>
#include <filesystem>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <SDL3_image/SDL_image.h>
#include "video_player.h"
#include "utils.h"
#include <algorithm> // Added for std::transform
#include <unistd.h>  // Added for access() function
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
}

// HW format storage for callback
static enum AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;

// FFmpeg HW format selection callback
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    for (const enum AVPixelFormat *p = pix_fmts; *p != -1; p++) {
        if (*p == hw_pix_fmt)
            return *p;
    }
    std::cerr << "Warning: HW format not found, using software fallback." << std::endl;
    return pix_fmts[0];
}

// Cleanup current media resources
void stopCurrentMedia(AVFormatContext** formatContext, 
                      AVCodecContext** codecContext, AVCodecContext** audioCodecContext,
                      AVBufferRef** hw_device_ctx, SDL_Texture** texture, 
                      AVFrame** frame, AVFrame** sw_frame, AVFrame** audioFrame,
                      SDL_Texture** imageTexture, SDL_AudioStream** audioStream,
                      SwrContext** swrCtx, AVPacket* packet, bool* packetPending) {
    
    if (*packetPending && packet) {
        av_packet_unref(packet);
        *packetPending = false;
    }

    if (*formatContext) { avformat_close_input(formatContext); *formatContext = nullptr; }
    if (*codecContext) { avcodec_free_context(codecContext); *codecContext = nullptr; }
    if (*audioCodecContext) { avcodec_free_context(audioCodecContext); *audioCodecContext = nullptr; }
    if (*hw_device_ctx) { av_buffer_unref(hw_device_ctx); *hw_device_ctx = nullptr; }
    if (*texture) { SDL_DestroyTexture(*texture); *texture = nullptr; }
    if (*frame) { av_frame_free(frame); *frame = nullptr; }
    if (*sw_frame) { av_frame_free(sw_frame); *sw_frame = nullptr; }
    if (*audioFrame) { av_frame_free(audioFrame); *audioFrame = nullptr; }
    if (*imageTexture) { SDL_DestroyTexture(*imageTexture); *imageTexture = nullptr; }
    if (*audioStream) { SDL_DestroyAudioStream(*audioStream); *audioStream = nullptr; }
    if (*swrCtx) { swr_free(swrCtx); *swrCtx = nullptr; }
}

int main(int argc, char* argv[]) {
    // Load device configuration
    loadConfig("setup.txt");

    ipAddress = getIPAddress();
    
    // HEVC HW device diagnostics
    if (access("/dev/video19", R_OK | W_OK) == 0) {
        std::cout << "DEBUG: HW decoder /dev/video19 is accessible." << std::endl;
    } else {
        std::cerr << "DEBUG: HW decoder /dev/video19 NOT accessible (errno: " << errno << ")." << std::endl;
    }

    std::cout << "Local IP: " << ipAddress << std::endl;

    lo_server_thread oscServer = setup_osc_server();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::cerr << "Failed to init SDL: " << SDL_GetError() << std::endl;
        return -1;
    }

    if (!TTF_Init()) {
        std::cerr << "Failed to init TTF: " << SDL_GetError() << std::endl;
        return -1;
    }

    SDL_Window* window = SDL_CreateWindow("ROMP - Raspberry OSC Media Player", screenWidth, screenHeight, SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::cerr << "Failed to create window: " << SDL_GetError() << std::endl;
        return -1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::cerr << "Failed to create renderer: " << SDL_GetError() << std::endl;
        return -1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    SDL_SetWindowFullscreen(window, true);
    SDL_HideCursor();

    font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 48);
    if (!font) {
        std::cerr << "Failed to load font: " << SDL_GetError() << std::endl;
        return -1;
    }

    // Initial state
    SDL_Texture* texture = nullptr;
    SDL_Texture* imageTexture = nullptr;
    bool imageLoaded = false;
    bool videoLoaded = false;

    // Attempt to load splash logo
    std::string logoPath = constructFullPath("logo.jpg");
    SDL_Surface* logoSurface = IMG_Load(logoPath.c_str());
    if (logoSurface) {
        imageTexture = SDL_CreateTextureFromSurface(renderer, logoSurface);
        SDL_DestroySurface(logoSurface);
        if (imageTexture) {
            imageLoaded = true;
            std::cout << "ROMP Splash logo loaded." << std::endl;
        }
    }

    message = "ROMP System [" + oscAddress + "] Ready on Index: " + deviceId + "\nIP: " + ipAddress;
    createTextTextures(renderer, font, message, textTextures, textRects, window);
    needsRedraw = true;

    // Forced first draw
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (imageLoaded && imageTexture) {
        SDL_RenderTexture(renderer, imageTexture, nullptr, nullptr);
    }
    if (showMessage) {
        for (size_t i = 0; i < textTextures.size(); ++i) {
            SDL_RenderTexture(renderer, textTextures[i], nullptr, &textRects[i]);
        }
    }
    SDL_RenderPresent(renderer);

    SDL_Texture* crossfadeOldTexture = nullptr; // For Crossfade
    AVFormatContext* formatContext = nullptr;
    AVCodecContext* codecContext = nullptr;
    AVCodecContext* audioCodecContext = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* sw_frame = nullptr; // Frame for copying data from HW layer
    AVFrame* audioFrame = nullptr;
    AVBufferRef* hw_device_ctx = nullptr; // HW acceleration context
    SDL_AudioStream* audioStream = nullptr;
    SwrContext* swrCtx = nullptr;

    double frameRate = 0.0;
    int videoStreamIndex = -1;
    int audioStreamIndex = -1;

    AVCodecParameters* codecParameters = nullptr;

    Uint64 startTime = SDL_GetTicks();
    bool tenSecondAfterStart = false;
    bool firstFrameDecoded = false; // Prevent green flash
    std::string currentVideoFileName;
    Uint64 nextFrameTime = 0;

    AVPacket* packet = av_packet_alloc();
    bool packetPending = false;
    char errbuf[AV_ERROR_MAX_STRING_SIZE]; 

    while (true) {
        // IP recovery
        if (ipAddress == "0.0.0.0") {
            std::string newIp = getIPAddress();
            if (newIp != "0.0.0.0") {
                ipAddress = newIp;
                std::cout << "IP acquired: " << ipAddress << std::endl;
                message = "ROMP System [" + oscAddress + "] Ready on Index: " + deviceId + "\nIP: " + ipAddress;
                createTextTextures(renderer, font, message, textTextures, textRects, window);
                needsRedraw = true;
            }
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                goto cleanup;
            }
            handlePlaybackSpeed(playbackSpeed, e, renderer, window);

            // Refresh info on 'i'
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_I) {
                std::string newIp = getIPAddress();
                if (newIp != ipAddress && newIp != "0.0.0.0") {
                    ipAddress = newIp;
                    std::cout << "IP updated: " << ipAddress << std::endl;
                }
                message = "ROMP System [" + oscAddress + "] Ready on Index: " + deviceId + "\nIP: " + ipAddress;
                createTextTextures(renderer, font, message, textTextures, textRects, window);
                needsRedraw = true;
            }

            if (e.type == SDL_EVENT_WINDOW_RESIZED || e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                updateTextTextures(renderer, window);
                needsRedraw = true;
            }
        }

        // 10s auto-hide info
        if (!tenSecondAfterStart) {
            if (SDL_GetTicks() - startTime > 10000) {
                tenSecondAfterStart = true;
                if (showMessage) {
                    showMessage = false;
                    needsRedraw = true;
                }
                std::cout << "ROMP Startup info auto-hidden." << std::endl;
            }
        }

        if (stopPlayback) {
            stopPlayback = false;
            stopCurrentMedia(&formatContext, &codecContext, &audioCodecContext, &hw_device_ctx, &texture, 
                             &frame, &sw_frame, &audioFrame, &imageTexture, &audioStream, 
                             &swrCtx, packet, &packetPending);
            if (crossfadeOldTexture) { SDL_DestroyTexture(crossfadeOldTexture); crossfadeOldTexture = nullptr; }
            
            currentTransition = TRANS_NONE; // Immediately end any running effect
            videoLoaded = false;
            imageLoaded = false;
            currentVideoFileName.clear();
            needsRedraw = true;
            hw_pix_fmt = AV_PIX_FMT_NONE;
            isPaused = false; 
            std::cout << "Stopped. Displaying black screen." << std::endl;
        }

        // Transition logic
        float transitionProgress = 1.0f;
        if (currentTransition != TRANS_NONE) {
            transitionProgress = (float)(SDL_GetTicks() - transitionStartTime) / (transitionDuration * 1000.0f);
            if (transitionProgress > 1.0f) {
                transitionProgress = 1.0f;
                if (currentTransition == TRANS_FADEOUT && triggerStopAfterFade) {
                    stopPlayback = true;
                    triggerStopAfterFade = false;
                } else {
                    currentTransition = TRANS_NONE;
                }
                if (currentTransition == TRANS_CROSS && crossfadeOldTexture) {
                    SDL_DestroyTexture(crossfadeOldTexture);
                    crossfadeOldTexture = nullptr;
                }
            }
        }

        // Apply audio gain based on transition
        if (audioStream) {
            float audioGain = 1.0f;
            if (currentTransition == TRANS_FADEIN || currentTransition == TRANS_CROSS) {
                audioGain = transitionProgress;
            } else if (currentTransition == TRANS_FADEOUT) {
                audioGain = 1.0f - transitionProgress;
            }
            SDL_SetAudioStreamGain(audioStream, audioGain);
        }

        // 0. Načtení nového souboru (video nebo obrázek)
        std::string fileToLoad;
        bool shouldReload = false;
        {
            std::lock_guard<std::mutex> lock(oscMutex);
            if (!videoFileName.empty()) {
                fileToLoad = videoFileName;
                shouldReload = reloadVideo;
                videoFileName.clear(); // Vyčistíme, aby se nenačítalo znovu
                reloadVideo = false;
            }
        }

        if (!fileToLoad.empty() && (fileToLoad != currentVideoFileName || shouldReload)) {
            // Konstrukce absolutní cesty dle parametru path= v setup.txt
            std::string fullVideoPath = constructFullPath(fileToLoad);

            currentRetryAttempt = 0;
            bool loadSuccess = false;

            while (currentRetryAttempt < MAX_LOAD_RETRIES && !loadSuccess) {
                currentRetryAttempt++;
                std::cout << "Pokus o načtení média '" << fullVideoPath << "', pokus #" << currentRetryAttempt << "/" << MAX_LOAD_RETRIES << std::endl;

            // Zjisti příponu pomocí moderního C++17 std::filesystem
            std::string ext = std::filesystem::path(fullVideoPath).extension().string();
            if (!ext.empty() && ext[0] == '.') {
                ext = ext.substr(1); // odstranění úvodní tečky
            }
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            bool isAudioFile = (ext == "mp3" || ext == "wav" || ext == "flac" || ext == "ogg" || ext == "m4a");

            firstFrameDecoded = false; // Reset on every new load

            if (currentTransition == TRANS_CROSS) {
                // Create snapshot of current content for smooth transition
                if (crossfadeOldTexture) SDL_DestroyTexture(crossfadeOldTexture);
                crossfadeOldTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, screenWidth, screenHeight);
                SDL_SetRenderTarget(renderer, crossfadeOldTexture);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);
                
                // Draw current state to texture
                if (videoLoaded && texture) {
                    SDL_SetTextureAlphaMod(texture, 255);
                    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
                } else if (imageLoaded && imageTexture) {
                    SDL_SetTextureAlphaMod(imageTexture, 255);
                    SDL_RenderTexture(renderer, imageTexture, nullptr, nullptr);
                }
                
                SDL_SetRenderTarget(renderer, nullptr);
                std::cout << "Crossfade snapshot created." << std::endl;
            }

            // Image handling
            if (ext == "jpg" || ext == "jpeg" || ext == "png") {
                stopCurrentMedia(&formatContext, &codecContext, &audioCodecContext, &hw_device_ctx, &texture, 
                                 &frame, &sw_frame, &audioFrame, &imageTexture, &audioStream, 
                                 &swrCtx, packet, &packetPending);

                hw_pix_fmt = AV_PIX_FMT_NONE;

                if (imageTexture) {
                    SDL_DestroyTexture(imageTexture);
                    imageTexture = nullptr;
                }
                SDL_Surface* imgSurface = IMG_Load(fullVideoPath.c_str());
                if (!imgSurface) {
                    std::cerr << "Image load error: " << fullVideoPath << " - " << SDL_GetError() << std::endl;
                    imageLoaded = false;
                    videoLoaded = false;
                } else {
                    imageTexture = SDL_CreateTextureFromSurface(renderer, imgSurface);
                    SDL_DestroySurface(imgSurface); 
                    if (!imageTexture) {
                        std::cerr << "Texture creation error: " << SDL_GetError() << std::endl;
                        imageLoaded = false;
                        videoLoaded = false;
                    } else {
                        imageLoaded = true;
                        videoLoaded = false;
                        needsRedraw = true;
                        std::cout << "Image loaded: " << fullVideoPath << std::endl;
                        loadSuccess = true;
                    }
                }
            }
            // Video or Audio handling
            else {
                stopCurrentMedia(&formatContext, &codecContext, &audioCodecContext, &hw_device_ctx, &texture, 
                                 &frame, &sw_frame, &audioFrame, &imageTexture, &audioStream, 
                                 &swrCtx, packet, &packetPending);

                hw_pix_fmt = AV_PIX_FMT_NONE;

                if (avformat_open_input(&formatContext, fullVideoPath.c_str(), nullptr, nullptr) != 0) {
                    std::cerr << "Cannot open video: " << fullVideoPath << std::endl;
                    videoLoaded = false;
                    imageLoaded = false;
                    SDL_Delay(500);
                    continue;
                }
                if (avformat_find_stream_info(formatContext, nullptr) < 0) {
                    std::cerr << "Cannot find stream info." << std::endl;
                    avformat_close_input(&formatContext);
                    videoLoaded = false;
                    imageLoaded = false;
                    SDL_Delay(500);
                    continue;
                }

                // Najdi video stream
                videoStreamIndex = -1;
                audioStreamIndex = -1;
                for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
                    auto type = formatContext->streams[i]->codecpar->codec_type;
                    if (type == AVMEDIA_TYPE_VIDEO) {
                        videoStreamIndex = i;
                    } else if (type == AVMEDIA_TYPE_AUDIO) {
                        audioStreamIndex = i;
                    }
                }

                if (videoStreamIndex == -1 && !isAudioFile) {
                    std::cerr << "Nebyl nalezen video stream!" << std::endl;
                    avformat_close_input(&formatContext);
                    videoLoaded = false;
                    imageLoaded = false;
                    SDL_Delay(500);
                    continue;
                }

                // Setup Audio if present
                if (audioStreamIndex != -1) {
                    AVCodecParameters* audioParams = formatContext->streams[audioStreamIndex]->codecpar;
                    const AVCodec* audioCodec = avcodec_find_decoder(audioParams->codec_id);
                    if (audioCodec) {
                        audioCodecContext = avcodec_alloc_context3(audioCodec);
                        avcodec_parameters_to_context(audioCodecContext, audioParams);
                        if (avcodec_open2(audioCodecContext, audioCodec, nullptr) >= 0) {
                            // Setup SDL Audio Stream
                            SDL_AudioSpec targetSpec;
                            targetSpec.format = SDL_AUDIO_F32;
                            targetSpec.channels = 2;
                            targetSpec.freq = 48000;

                            // Vyhledání zařízení podle názvu ze setup.txt
                            SDL_AudioDeviceID devId = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
                            if (audioDeviceName != "default") {
                                int devCount = 0;
                                SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&devCount);
                                if (devices) {
                                    std::string target = audioDeviceName;
                                    std::transform(target.begin(), target.end(), target.begin(), ::tolower);
                                    // Map "jack" to "headphones", a common ALSA name
                                    if (target == "jack") target = "headphones";

                                    for (int i = 0; i < devCount; i++) {
                                        const char* name = SDL_GetAudioDeviceName(devices[i]);
                                        if (name) {
                                            std::string devName = name;
                                            std::transform(devName.begin(), devName.end(), devName.begin(), ::tolower);
                                            if (devName.find(target) != std::string::npos) {
                                                devId = devices[i];
                                                std::cout << "Selected audio device: " << name << " (ID: " << devId << ")" << std::endl;
                                                break;
                                            }
                                        }
                                    }
                                    SDL_free(devices);
                                }
                            }

                            audioStream = SDL_OpenAudioDeviceStream(devId, &targetSpec, nullptr, nullptr);
                            if (audioStream) {
                                SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(audioStream));
                                
                                // Setup Resampler
                                AVChannelLayout outLayout;
                                av_channel_layout_default(&outLayout, 2);
                                swr_alloc_set_opts2(&swrCtx, 
                                    &outLayout, AV_SAMPLE_FMT_FLT, 48000,
                                    &audioCodecContext->ch_layout, audioCodecContext->sample_fmt, audioCodecContext->sample_rate,
                                    0, nullptr);
                                swr_init(swrCtx);
                                audioFrame = av_frame_alloc();
                                std::cout << "Audio initialized (" << audioCodec->name << ")" << std::endl;
                            }
                        }
                    }
                }

                if (isAudioFile) {
                    videoLoaded = false;
                    imageLoaded = true; // Use logo/black for audio files
                    loadSuccess = true;
                    currentVideoFileName = fileToLoad;
                    std::cout << "Audio file loaded: " << fullVideoPath << std::endl;
                    continue; 
                }

                codecParameters = formatContext->streams[videoStreamIndex]->codecpar;
                
                // Print stream info for HW failure diagnostics
                std::cout << "Stream info: " << codecParameters->width << "x" << codecParameters->height 
                          << ", profile: " << codecParameters->profile 
                          << ", format: " << codecParameters->format << std::endl;
                
                // Get FPS (Framerate) from video stream for correct timing
                AVRational fps = formatContext->streams[videoStreamIndex]->avg_frame_rate;
                if (fps.den > 0 && fps.num > 0) {
                    frameRate = av_q2d(fps);
                } else {
                    frameRate = 25.0; // Fallback if header doesn't know its framerate
                }

                const AVCodec* codec = nullptr;

                // 1. Pokus o hardwarový dekodér (v4l2m2m)
                const AVCodec* hw_codec = nullptr;
                bool hw_init_success = false;
                if (codecParameters->codec_id == AV_CODEC_ID_H264) {
                    hw_codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
                } else if (codecParameters->codec_id == AV_CODEC_ID_HEVC) {
                    // For HEVC on RPi 4, we don't use v4l2m2m wrapper (it's stateless).
                    // Use standard hevc decoder which finds HW acceleration via hw_config.
                    hw_codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
                }

                if (hw_codec) {
                    codecContext = avcodec_alloc_context3(hw_codec);
                    if (!codecContext) {
                        std::cerr << "Chyba: Nelze alokovat kontext pro HW dekodér (" << hw_codec->name << ")." << std::endl;
                        // Fallback to SW will be handled below
                    } else {
                        avcodec_parameters_to_context(codecContext, codecParameters);

                        // Set general HW acceleration (if supported and not pure v4l2m2m)
                        AVHWDeviceType hw_type_generic = AV_HWDEVICE_TYPE_NONE;
                        for (int i = 0;; i++) {
                            const AVCodecHWConfig *config = avcodec_get_hw_config(hw_codec, i);
                            if (!config) break;
                            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) {
                                // On RPi 4 for HEVC we prefer DRM (for rpivid stateless decoder)
                                if (config->device_type == AV_HWDEVICE_TYPE_DRM) {
                                    if (av_hwdevice_ctx_create(&hw_device_ctx, config->device_type, nullptr, nullptr, 0) == 0) {
                                        hw_type_generic = config->device_type;
                                        hw_pix_fmt = config->pix_fmt;
                                        codecContext->get_format = get_hw_format;
                                        codecContext->hw_device_ctx = av_buffer_ref(hw_device_ctx);
                                        std::cout << "Úspěšně inicializována obecná HW akcelerace kontextu: " << av_hwdevice_get_type_name(hw_type_generic) << std::endl;
                                        hw_init_success = true;
                                        break;
                                    }
                                }
                            }
                        }
                        
                        // For v4l2m2m decoders that don't use general hw_device_ctx, reset hw_pix_fmt
                        if (hw_type_generic == AV_HWDEVICE_TYPE_NONE && std::string(hw_codec->name).find("v4l2m2m") != std::string::npos) {
                            std::cout << "Native V4L2M2M decoder detected, attempting to open." << std::endl;
                            hw_pix_fmt = AV_PIX_FMT_NONE; // V4L2M2M often manages format internally
                        }

                        // Attempt to open HW decoder with stability parameters for RPi
                        AVDictionary* opts = nullptr;
                        if (codecParameters->codec_id == AV_CODEC_ID_HEVC) {
                            av_dict_set(&opts, "num_capture_buffers", "16", 0); 
                        }

                        int open_res = avcodec_open2(codecContext, hw_codec, &opts);
                        av_dict_free(&opts);

                        if (open_res < 0) {
                            char errbuf[AV_ERROR_MAX_STRING_SIZE];
                            av_strerror(open_res, errbuf, sizeof(errbuf));
                            std::cerr << "Warning: Cannot open HW decoder (" << hw_codec->name << "): " << errbuf << ". Falling back to SW." << std::endl;
                            avcodec_free_context(&codecContext);
                            codecContext = nullptr;
                            if (hw_device_ctx) av_buffer_unref(&hw_device_ctx); 
                            hw_device_ctx = nullptr;
                            hw_pix_fmt = AV_PIX_FMT_NONE; 
                        } else {
                            hw_init_success = true;
                            std::cout << "Using HW decoder: " << codecContext->codec->name << std::endl;
                        }
                    }
                }

                // SW fallback
                if (!hw_init_success) {
                    codec = avcodec_find_decoder(codecParameters->codec_id); 
                    if (!codec) {
                        std::cerr << "Critical: Cannot find SW decoder for codec ID: " << codecParameters->codec_id << std::endl;
                        avformat_close_input(&formatContext);
                        videoLoaded = false;
                        imageLoaded = false;
                        SDL_Delay(500);
                        continue;
                    }
                    codecContext = avcodec_alloc_context3(codec);
                    if (!codecContext) {
                        std::cerr << "Critical: Cannot allocate SW context." << std::endl;
                        avformat_close_input(&formatContext);
                        videoLoaded = false;
                        imageLoaded = false;
                        SDL_Delay(500);
                        continue;
                    }
                    avcodec_parameters_to_context(codecContext, codecParameters);
                    if (avcodec_open2(codecContext, codec, nullptr) < 0) {
                        std::cerr << "Critical: Cannot open SW decoder (" << codec->name << ")." << std::endl;
                        avcodec_free_context(&codecContext);
                        codecContext = nullptr;
                    } else {
                        std::cout << "Using SW decoder: " << codec->name << std::endl;
                        hw_pix_fmt = AV_PIX_FMT_NONE; 
                        if (hw_device_ctx) av_buffer_unref(&hw_device_ctx); // Free if allocated but not used
                        hw_device_ctx = nullptr;
                    }
                }

                if (!codecContext) { 
                    std::cerr << "Critical: No functional decoder initialized." << std::endl;
                    avformat_close_input(&formatContext);
                    videoLoaded = false;
                    imageLoaded = false;
                    SDL_Delay(500);
                    continue;
                }

                frame = av_frame_alloc();
                sw_frame = av_frame_alloc();

                videoLoaded = true;
                imageLoaded = false;
                needsRedraw = true;
                currentVideoFileName = fileToLoad;
                std::cout << "Video loaded: " << fullVideoPath << std::endl;
                nextFrameTime = SDL_GetTicks();

                if (texture) {
                    SDL_DestroyTexture(texture);
                    texture = nullptr;
                }
                SDL_PixelFormat pixelFormat = SDL_PIXELFORMAT_IYUV;
                if (codecContext->sw_pix_fmt == AV_PIX_FMT_NV12 || codecContext->pix_fmt == AV_PIX_FMT_NV12) {
                    pixelFormat = SDL_PIXELFORMAT_NV12;
                }

                texture = SDL_CreateTexture(
                    renderer,
                    pixelFormat,
                    SDL_TEXTUREACCESS_STREAMING,
                    codecParameters->width,
                    codecParameters->height
                );

                if (!texture) {
                    std::cerr << "Texture error: " << SDL_GetError() << std::endl;
                    stopCurrentMedia(&formatContext, &codecContext, &audioCodecContext, &hw_device_ctx, &texture, 
                                     &frame, &sw_frame, &audioFrame, &imageTexture, &audioStream, 
                                     &swrCtx, packet, &packetPending);
                    videoLoaded = false;
                    SDL_Delay(500);
                    continue;
                }
                loadSuccess = true;
            }
            } // End of while retry
            if (loadSuccess) currentVideoFileName = fileToLoad;
        }

        // 1. Vykreslení videa, pokud je načteno a není pauza
        if ((videoLoaded || audioStreamIndex != -1) && formatContext && !isPaused) {
            int response = 0;

            if (!packetPending) {
                int read_res = 0;
                while ((read_res = av_read_frame(formatContext, packet)) >= 0) {
                    if (packet->stream_index == videoStreamIndex || packet->stream_index == audioStreamIndex) {
                        packetPending = true;
                        break;
                    }
                    av_packet_unref(packet);
                }
                
                if (read_res == AVERROR_EOF) {
                    if (loopVideo) {
                        av_seek_frame(formatContext, videoStreamIndex, 0, AVSEEK_FLAG_BACKWARD);
                        if (codecContext) avcodec_flush_buffers(codecContext);
                        if (audioCodecContext) avcodec_flush_buffers(audioCodecContext);
                        if (audioStream) SDL_ClearAudioStream(audioStream);
                        nextFrameTime = SDL_GetTicks();
                        continue; // Proceed to next iteration for immediate reload
                    } else {
                        // Video finished and not looping -> clean up and show black
                        std::cout << "Video '" << currentVideoFileName << "' dohrálo." << std::endl;
                        
                        stopCurrentMedia(&formatContext, &codecContext, &audioCodecContext, &hw_device_ctx, &texture, 
                             &frame, &sw_frame, &audioFrame, &imageTexture, &audioStream, 
                             &swrCtx, packet, &packetPending);
                        
                        videoLoaded = false;
                        currentVideoFileName.clear();
                        needsRedraw = true;
                        continue;
                    }
                }
            }

            if (packetPending && packet->stream_index == audioStreamIndex && audioCodecContext) {
                response = avcodec_send_packet(audioCodecContext, packet);
                if (response == 0) {
                    packetPending = false;
                    while (avcodec_receive_frame(audioCodecContext, audioFrame) == 0) {
                        uint8_t* outData[2];
                        int maxOutSamples = swr_get_out_samples(swrCtx, audioFrame->nb_samples);
                        float* resampledData = (float*)av_malloc(maxOutSamples * 2 * sizeof(float));
                        outData[0] = (uint8_t*)resampledData;
                        
                        int converted = swr_convert(swrCtx, outData, maxOutSamples, (const uint8_t**)audioFrame->data, audioFrame->nb_samples);
                        if (converted > 0) {
                            SDL_PutAudioStreamData(audioStream, resampledData, converted * 2 * sizeof(float));
                        }
                        av_free(resampledData);
                    }
                    av_packet_unref(packet);
                }
            }

            if (packetPending && packet->stream_index == videoStreamIndex && codecContext) {
                response = avcodec_send_packet(codecContext, packet);
                if (response == 0) {
                    packetPending = false;
                    av_packet_unref(packet);
                } else if (response == AVERROR(EAGAIN)) {
                    // Decoder full
                } else {
                    std::cerr << "Packet send error: " << av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, response) << std::endl;
                    av_packet_unref(packet);
                    packetPending = false;
                }
            }

            response = avcodec_receive_frame(codecContext, frame);
            if (response == 0) {
                AVFrame *render_frame = frame;

                if (frame->format == hw_pix_fmt && hw_pix_fmt != AV_PIX_FMT_NONE) {
                    // If frame is in HW format, download it safely to RAM (for compatibility)
                    if (av_hwframe_transfer_data(sw_frame, frame, 0) < 0) {
                        std::cerr << "HW to RAM transfer error." << std::endl;
                        render_frame = nullptr;
                    } else {
                        render_frame = sw_frame;
                    }
                }
                
                if (render_frame) {
                    // Upload raw data to GPU via SDL texture
                    if (render_frame->format == AV_PIX_FMT_YUV420P || render_frame->format == AV_PIX_FMT_YUVJ420P) {
                        SDL_UpdateYUVTexture(texture, nullptr, render_frame->data[0], render_frame->linesize[0], render_frame->data[1], render_frame->linesize[1], render_frame->data[2], render_frame->linesize[2]);
                    } else if (render_frame->format == AV_PIX_FMT_NV12) {
                        SDL_UpdateNVTexture(texture, nullptr, render_frame->data[0], render_frame->linesize[0], render_frame->data[1], render_frame->linesize[1]);
                    } else {
                        std::cerr << "Warning: Unknown or unsupported pixel format: " << render_frame->format << std::endl;
                    }
                    if (!firstFrameDecoded) {
                        firstFrameDecoded = true;
                        std::cout << "První snímek dekódován, uvolňuji zobrazení." << std::endl;
                    }
                    needsRedraw = true;
                }
            } else if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
                // No frame available yet
            } else if (response < 0) {
                std::cerr << "Decoding error: " << av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, response) << std::endl;
            }
        }

        // Render loop
        if (needsRedraw || currentTransition != TRANS_NONE) {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);

            // Crossfade background
            if (currentTransition == TRANS_CROSS && crossfadeOldTexture) {
                SDL_SetTextureAlphaMod(crossfadeOldTexture, 255);
                SDL_RenderTexture(renderer, crossfadeOldTexture, nullptr, nullptr);
            }
            
            // Rendering logic
            bool canRenderVideo = videoLoaded && firstFrameDecoded && texture;
            bool canRenderImage = imageLoaded && imageTexture && !videoLoaded;

            if ((canRenderVideo || canRenderImage) && !(currentTransition == TRANS_FADEOUT && transitionProgress >= 1.0f)) {
                SDL_Texture* toRender = canRenderVideo ? texture : imageTexture;

                // Alpha mod for transitions
                if (currentTransition == TRANS_CROSS || currentTransition == TRANS_FADEIN) {
                    SDL_SetTextureAlphaMod(toRender, (Uint8)(transitionProgress * 255));
                } else {
                    SDL_SetTextureAlphaMod(toRender, 255);
                }

                SDL_RenderTexture(renderer, toRender, nullptr, nullptr);
            }

            // Fade overlay
            if (currentTransition == TRANS_FADEOUT || currentTransition == TRANS_FADEIN) {
                Uint8 alpha = 0;
                if (currentTransition == TRANS_FADEOUT) alpha = (Uint8)(transitionProgress * 255);
                else alpha = (Uint8)((1.0f - transitionProgress) * 255);
                
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, alpha);
                SDL_RenderFillRect(renderer, nullptr);
            }

            // Text overlays
            if (showMessage) {
                for (size_t i = 0; i < textTextures.size(); ++i) {
                    SDL_RenderTexture(renderer, textTextures[i], nullptr, &textRects[i]);
                }
            }

            SDL_RenderPresent(renderer);
            if (!videoLoaded) needsRedraw = false;
        }

        // Frame timing
        if (videoLoaded && !isPaused) {
            int frameDelay = calculateFrameDelay(playbackSpeed, frameRate);
            Uint64 currentTime = SDL_GetTicks();
            if (currentTime < nextFrameTime) {
                SDL_Delay(nextFrameTime - currentTime);
            }
            nextFrameTime += frameDelay;
            if (nextFrameTime < SDL_GetTicks()) nextFrameTime = SDL_GetTicks();
            
            if (frame) av_frame_unref(frame);
            if (sw_frame) av_frame_unref(sw_frame);
        } else {
            SDL_Delay(16); // ~60fps loop for smooth transitions
        }
    }

cleanup:
    if (oscServer) {
        lo_server_thread_free(oscServer);
    }
    if (imageTexture) SDL_DestroyTexture(imageTexture);
    for (SDL_Texture* texture : textTextures) {
        SDL_DestroyTexture(texture);
    }
    if (frame) av_frame_free(&frame);
    if (sw_frame) av_frame_free(&sw_frame);
    if (packet) av_packet_free(&packet);
    if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_CloseFont(font);
    TTF_Quit();
    SDL_Quit();
    avcodec_free_context(&codecContext);
    avformat_close_input(&formatContext);

    return 0;
}