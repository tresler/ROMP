#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <vector>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <SDL3_image/SDL_image.h>
#include <lo/lo.h>
#include <mutex>

// Global variables declarations
extern std::mutex oscMutex;
extern std::string videoFileName;
extern bool isFullscreen;
extern std::string oscAddress;
extern std::string ipAddress;
extern std::string deviceId;
extern bool showMessage;
extern std::vector<SDL_Texture*> textTextures;
extern std::vector<SDL_FRect> textRects;
extern TTF_Font* font;
extern std::string message;
extern bool needsRedraw;
extern bool loopVideo;
extern bool isPaused;
extern bool reloadVideo;
extern int screenWidth;
extern int screenHeight;
extern std::string oscPort;
extern double playbackSpeed;
extern std::string audioDeviceName;
extern std::string mediaPath;
extern bool stopPlayback;

enum TransitionType { TRANS_NONE, TRANS_FADEIN, TRANS_FADEOUT, TRANS_CROSS };
extern TransitionType currentTransition;
extern float transitionDuration;
extern Uint64 transitionStartTime;
extern float defaultFadeIn;
extern float defaultFadeOut;
extern float defaultCross;
extern bool triggerStopAfterFade;
extern int currentRetryAttempt;
extern const int MAX_LOAD_RETRIES;

// Function declarations
std::string getIPAddress();
void loadConfig(const std::string& filename);
int osc_message_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);
lo_server_thread setup_osc_server();
std::vector<std::string> splitTextIntoLines(const std::string& text);
void createTextTextures(SDL_Renderer* renderer, TTF_Font* font, const std::string& message, std::vector<SDL_Texture*>& textures, std::vector<SDL_FRect>& rects, SDL_Window* window);
void updateTextTextures(SDL_Renderer* renderer, SDL_Window* window);
void handlePlaybackSpeed(double& playbackSpeed, const SDL_Event& event, SDL_Renderer* renderer, SDL_Window* window);
int calculateFrameDelay(double playbackSpeed, double frameRate);
std::string constructFullPath(const std::string& filename);

#endif // UTILS_H