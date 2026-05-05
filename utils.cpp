#include <iostream>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <lo/lo.h>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <SDL3_image/SDL_image.h>
#include <vector>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <mutex>
#include "utils.h"

// Global variables definitions
std::mutex oscMutex;
std::string videoFileName;
std::string oscAddress = "romp"; // Default OSC address
bool isFullscreen = true;
std::string ipAddress;
std::string deviceId = "1"; // Default ID
bool showMessage = true;
std::vector<SDL_Texture*> textTextures;
std::vector<SDL_FRect> textRects;
TTF_Font* font = nullptr;
std::string message;
bool needsRedraw = true;
bool loopVideo = false;
bool isPaused = false; 
Uint64 lastPauseToggleTime = 0; 
bool reloadVideo = false; 
double playbackSpeed = 1.0; 
int screenWidth = 1920;
int screenHeight = 1080;
std::string oscPort = "8000"; // Default OSC port
std::string audioDeviceName = "default";
std::string mediaPath = "";   // Default media path
bool stopPlayback = false;
TransitionType currentTransition = TRANS_NONE;
float transitionDuration = 1.0f;
Uint64 transitionStartTime = 0;
float defaultFadeIn = 1.0f;
float defaultFadeOut = 1.0f;
float defaultCross = 1.0f;
bool triggerStopAfterFade = false;
int currentRetryAttempt = 0;
const int MAX_LOAD_RETRIES = 3; // Maximum load attempts

// Get local IP address
std::string getIPAddress() {
    struct ifaddrs *ifap, *ifa;
    struct sockaddr_in *sa;
    char addr[INET_ADDRSTRLEN];

    if (getifaddrs(&ifap) == -1) return "0.0.0.0";

    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
            sa = (struct sockaddr_in *) ifa->ifa_addr;
            inet_ntop(AF_INET, &sa->sin_addr, addr, INET_ADDRSTRLEN);
            std::string currentAddr(addr);
            // Return first non-loopback address
            if (currentAddr != "127.0.0.1") {
                freeifaddrs(ifap);
                return currentAddr;
            }
        }
    }
    if (ifap) {
        freeifaddrs(ifap);
    }
    return "0.0.0.0";
}

// Load configuration
void loadConfig(const std::string& filename) {
    std::string configPath = filename;
    const char* homeDir = getenv("HOME");
    std::string rompFolder = homeDir ? std::string(homeDir) + "/.romp" : "";
    std::string rompFile = rompFolder.empty() ? "" : rompFolder + "/setup.txt";

    // If filename is just "setup.txt", look into the hidden ~/.romp/ directory first.
    if (filename == "setup.txt") {
        if (!rompFile.empty() && std::filesystem::exists(rompFile)) {
            configPath = rompFile;
        } else if (std::filesystem::exists("setup.txt")) {
            configPath = "setup.txt";
        } else if (!rompFile.empty()) {
            configPath = rompFile; // Target for creating default config
        }
    }

    std::ifstream file(configPath);
    if (file.is_open()) {
        std::string line;
        std::string key = "index=";
        std::string keyAddress = "address=";
        std::string keyWidth = "width=";
        std::string keyHeight = "height=";
        std::string keyOscPort = "osc_port=";
        std::string keyPath = "path=";
        std::string keyAudio = "audio_device=";
        std::string keyFadeIn = "fadein=";
        std::string keyFadeOut = "fadeout=";
        std::string keyCross = "cross=";
        while (std::getline(file, line)) {
            // Remove \r if file was created in Windows
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.compare(0, key.length(), key) == 0) {
                deviceId = line.substr(key.length());
                std::cout << "Config loaded: deviceId = " << deviceId << std::endl;
            } else if (line.compare(0, keyAddress.length(), keyAddress) == 0) {
                oscAddress = line.substr(keyAddress.length());
                std::cout << "Config loaded: oscAddress = " << oscAddress << std::endl;
            } else if (line.compare(0, keyWidth.length(), keyWidth) == 0) {
                try { screenWidth = std::stoi(line.substr(keyWidth.length())); } catch (...) {}
                std::cout << "Config loaded: width = " << screenWidth << std::endl;
            } else if (line.compare(0, keyHeight.length(), keyHeight) == 0) {
                try { screenHeight = std::stoi(line.substr(keyHeight.length())); } catch (...) {}
                std::cout << "Config loaded: height = " << screenHeight << std::endl;
            } else if (line.compare(0, keyOscPort.length(), keyOscPort) == 0) {
                std::string portVal = line.substr(keyOscPort.length());
                if (!portVal.empty()) {
                    oscPort = portVal;
                }
                std::cout << "Config loaded: osc_port = " << oscPort << std::endl;
            } else if (line.compare(0, keyPath.length(), keyPath) == 0) {
                mediaPath = line.substr(keyPath.length());
            } else if (line.compare(0, keyAudio.length(), keyAudio) == 0) {
                audioDeviceName = line.substr(keyAudio.length());
            } else if (line.compare(0, keyFadeIn.length(), keyFadeIn) == 0) {
                try { defaultFadeIn = std::stof(line.substr(keyFadeIn.length())); } catch (...) {}
            } else if (line.compare(0, keyFadeOut.length(), keyFadeOut) == 0) {
                try { defaultFadeOut = std::stof(line.substr(keyFadeOut.length())); } catch (...) {}
            } else if (line.compare(0, keyCross.length(), keyCross) == 0) {
                try { defaultCross = std::stof(line.substr(keyCross.length())); } catch (...) {}
            }
        }
        file.close();
    } else {
        std::cerr << "Cannot open config file " << configPath << ". Creating default." << std::endl;
        // Ensure the directory exists if we are using the hidden folder
        if (!rompFolder.empty() && configPath == rompFile) {
            std::filesystem::create_directories(rompFolder);
        }
        std::ofstream outFile(configPath);
        if (outFile.is_open()) {
            outFile << "index=1\n";
            outFile << "address=romp\n";
            outFile << "width=1920\n";
            outFile << "height=1080\n";
            outFile << "osc_port=9000\n";
            outFile << "path=\n";
            outFile.close();
        }
    }
}

// Construct full path to media file
std::string constructFullPath(const std::string& filename) {
    if (mediaPath.empty()) {
        return filename;
    }

    std::string basePath = mediaPath;

    // Replace '~' with HOME directory
    if (basePath.length() >= 1 && basePath[0] == '~') {
        const char* homeDir = getenv("HOME");
        if (homeDir) {
            basePath.replace(0, 1, homeDir);
        } else {
            // Fallback: just remove ~ if HOME is not set
            basePath.erase(0, 1);
            if (basePath.length() >= 1 && basePath[0] == '/') {
                basePath.erase(0, 1);
            }
        }
    }

    std::filesystem::path dir(basePath);
    std::filesystem::path file(filename);
    
    // Path operator / handles slashes correctly
    return (dir / file).string();
}

void parsePlayArguments(int argc, lo_arg **argv, const char *types) {
    loopVideo = false;
    videoFileName = ""; 
    
    for (int i = 0; i < argc; ++i) {
        if (types[i] == 's') {
            std::string argStr = &argv[i]->s;
            if (argStr == "loop") {
                loopVideo = true;
            } else if (!argStr.empty()) {
                // If not "loop", consider it the filename
                const std::string whitespace = " \t\n\r\f\v";
                size_t first = argStr.find_first_not_of(whitespace);
                if (first != std::string::npos) {
                    size_t last = argStr.find_last_not_of(whitespace);
                    videoFileName = argStr.substr(first, (last - first + 1));
                }
            }
        }
    }
}

// OSC message handler
int osc_message_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    std::string expectedPlayPath = "/" + oscAddress + "/" + deviceId + "/play";
    std::string expectedPausePath = "/" + oscAddress + "/" + deviceId + "/pause";
    std::string expectedFadeInPath = "/" + oscAddress + "/" + deviceId + "/fadein";
    std::string expectedFadeOutPath = "/" + oscAddress + "/" + deviceId + "/fadeout";
    std::string expectedCrossPath = "/" + oscAddress + "/" + deviceId + "/cross";
    std::string expectedInfoPath = "/" + oscAddress + "/" + deviceId + "/info";
    std::string expectedSpeedUpPath = "/" + oscAddress + "/" + deviceId + "/speedup";
    std::string expectedSpeedDownPath = "/" + oscAddress + "/" + deviceId + "/speeddown";
    std::string expectedSpeedPath = "/" + oscAddress + "/" + deviceId + "/speed";
    std::string expectedStopPath = "/" + oscAddress + "/" + deviceId + "/stop";

    std::string globalPlayPath = "/" + oscAddress + "/play";
    std::string globalPausePath = "/" + oscAddress + "/pause";
    std::string globalFadeInPath = "/" + oscAddress + "/fadein";
    std::string globalFadeOutPath = "/" + oscAddress + "/fadeout";
    std::string globalCrossPath = "/" + oscAddress + "/cross";
    std::string globalInfoPath = "/" + oscAddress + "/info";
    std::string globalSpeedUpPath = "/" + oscAddress + "/speedup";
    std::string globalSpeedDownPath = "/" + oscAddress + "/speeddown";
    std::string globalSpeedPath = "/" + oscAddress + "/speed";
    std::string globalStopPath = "/" + oscAddress + "/stop";

    std::cout << "OSC message received: " << path << std::endl;

    std::lock_guard<std::mutex> lock(oscMutex);
    lo_address sender = lo_message_get_source(msg);

    if ((strcmp(path, expectedPlayPath.c_str()) == 0 || strcmp(path, globalPlayPath.c_str()) == 0) && argc >= 1) {
        parsePlayArguments(argc, argv, types);
        if (!videoFileName.empty()) {
            std::cout << "Action: Play file " << videoFileName << " (loop: " << (loopVideo ? "yes" : "no") << ")" << std::endl;
            reloadVideo = true;
            needsRedraw = true;
            currentTransition = TRANS_NONE;
            lo_send(sender, ("/" + oscAddress + "/status").c_str(), "ss", videoFileName.c_str(), "loading");
        }
    } else if (strcmp(path, expectedPausePath.c_str()) == 0 || strcmp(path, globalPausePath.c_str()) == 0) {
        Uint64 currentTime = SDL_GetTicks();
        if (currentTime - lastPauseToggleTime > 500) { // Throttle toggle
            isPaused = !isPaused;
            lastPauseToggleTime = currentTime;
            std::cout << "Action: Pause " << (isPaused ? "on" : "off") << std::endl;
        }
    } else if ((strcmp(path, expectedFadeInPath.c_str()) == 0 || strcmp(path, globalFadeInPath.c_str()) == 0) && argc >= 1) {
        parsePlayArguments(argc, argv, types);
        reloadVideo = true;
        transitionDuration = defaultFadeIn;
        for (int i = 0; i < argc; i++) {
            if (types[i] == 'f') { transitionDuration = argv[i]->f; break; }
            if (types[i] == 'i') { transitionDuration = (float)argv[i]->i; break; }
        }
        currentTransition = TRANS_FADEIN;
        transitionStartTime = SDL_GetTicks();
        needsRedraw = true;
    } else if (strcmp(path, expectedFadeOutPath.c_str()) == 0 || strcmp(path, globalFadeOutPath.c_str()) == 0) {
        transitionDuration = defaultFadeOut;
        for (int i = 0; i < argc; i++) {
            if (types[i] == 'f') { transitionDuration = argv[i]->f; break; }
            if (types[i] == 'i') { transitionDuration = (float)argv[i]->i; break; }
        }
        currentTransition = TRANS_FADEOUT;
        transitionStartTime = SDL_GetTicks();
        triggerStopAfterFade = true;
    } else if ((strcmp(path, expectedCrossPath.c_str()) == 0 || strcmp(path, globalCrossPath.c_str()) == 0) && argc >= 1) {
        parsePlayArguments(argc, argv, types);
        reloadVideo = true;
        transitionDuration = defaultCross;
        for (int i = 0; i < argc; i++) {
            if (types[i] == 'f') { transitionDuration = argv[i]->f; break; }
            if (types[i] == 'i') { transitionDuration = (float)argv[i]->i; break; }
        }
        currentTransition = TRANS_CROSS;
        transitionStartTime = SDL_GetTicks();
        needsRedraw = true;
    } else if (strcmp(path, expectedInfoPath.c_str()) == 0 || strcmp(path, globalInfoPath.c_str()) == 0) {
        showMessage = !showMessage;
        needsRedraw = true;
        std::cout << "Action: Info " << (showMessage ? "shown" : "hidden") << std::endl;
    } else if (strcmp(path, expectedSpeedUpPath.c_str()) == 0 || strcmp(path, globalSpeedUpPath.c_str()) == 0) {
        playbackSpeed += 0.1;
        if (playbackSpeed > 4) {
            playbackSpeed = 4;
        }
        std::cout << "Action: Speed increased to " << playbackSpeed << "x" << std::endl;
    } else if (strcmp(path, expectedSpeedDownPath.c_str()) == 0 || strcmp(path, globalSpeedDownPath.c_str()) == 0) {
        playbackSpeed -= 0.1;
        if (playbackSpeed < 0.1) {
            playbackSpeed = 0.1;
        }
        std::cout << "Action: Speed decreased to " << playbackSpeed << "x" << std::endl;
    } else if (strcmp(path, expectedSpeedPath.c_str()) == 0 || strcmp(path, globalSpeedPath.c_str()) == 0) {
        if (argc == 1) {
            if (types[0] == 'i') playbackSpeed = argv[0]->i;
            else if (types[0] == 'f') playbackSpeed = argv[0]->f;
            std::cout << "Action: Speed set to " << playbackSpeed << "x" << std::endl;
        } else {
            playbackSpeed = 1.0;
            std::cout << "Action: Speed reset to 1.0x" << std::endl;
        }
    } else if (strcmp(path, expectedStopPath.c_str()) == 0 || strcmp(path, globalStopPath.c_str()) == 0) {
        stopPlayback = true;
        currentTransition = TRANS_NONE;
        transitionStartTime = 0;
        triggerStopAfterFade = false;
        needsRedraw = true;
        std::cout << "Action: Stop playback" << std::endl;
    } else {
        std::cout << "Invalid OSC path or argument types" << std::endl;
    }
    return 0;
}

// Setup OSC server
lo_server_thread setup_osc_server() {
    lo_server_thread st = lo_server_thread_new_with_proto(oscPort.c_str(), LO_UDP, nullptr); 

    std::string methodPlayPath = "/" + oscAddress + "/" + deviceId + "/play";
    std::string methodPausePath = "/" + oscAddress + "/" + deviceId + "/pause";
    std::string methodFadeInPath = "/" + oscAddress + "/" + deviceId + "/fadein";
    std::string methodFadeOutPath = "/" + oscAddress + "/" + deviceId + "/fadeout";
    std::string methodCrossPath = "/" + oscAddress + "/" + deviceId + "/cross";
    std::string methodInfoPath = "/" + oscAddress + "/" + deviceId + "/info";
    std::string methodSpeedUpPath = "/" + oscAddress + "/" + deviceId + "/speedup";
    std::string methodSpeedDownPath = "/" + oscAddress + "/" + deviceId + "/speeddown";
    std::string methodSpeedPath = "/" + oscAddress + "/" + deviceId + "/speed";
    std::string methodStopPath = "/" + oscAddress + "/" + deviceId + "/stop";

    std::string globalPlayPath = "/" + oscAddress + "/play";
    std::string globalPausePath = "/" + oscAddress + "/pause";
    std::string globalFadeInPath = "/" + oscAddress + "/fadein";
    std::string globalFadeOutPath = "/" + oscAddress + "/fadeout";
    std::string globalCrossPath = "/" + oscAddress + "/cross";
    std::string globalInfoPath = "/" + oscAddress + "/info";
    std::string globalSpeedUpPath = "/" + oscAddress + "/speedup";
    std::string globalSpeedDownPath = "/" + oscAddress + "/speeddown";
    std::string globalSpeedPath = "/" + oscAddress + "/speed";
    std::string globalStopPath = "/" + oscAddress + "/stop";

    lo_server_thread_add_method(st, methodPlayPath.c_str(), NULL, osc_message_handler, nullptr);
    lo_server_thread_add_method(st, globalPlayPath.c_str(), NULL, osc_message_handler, nullptr);

    lo_server_thread_add_method(st, methodPausePath.c_str(), NULL, osc_message_handler, nullptr);
    lo_server_thread_add_method(st, globalPausePath.c_str(), NULL, osc_message_handler, nullptr);

    lo_server_thread_add_method(st, methodFadeInPath.c_str(), NULL, osc_message_handler, nullptr);
    lo_server_thread_add_method(st, globalFadeInPath.c_str(), NULL, osc_message_handler, nullptr);

    lo_server_thread_add_method(st, methodFadeOutPath.c_str(), NULL, osc_message_handler, nullptr);
    lo_server_thread_add_method(st, methodFadeOutPath.c_str(), "f", osc_message_handler, nullptr);
    lo_server_thread_add_method(st, methodFadeOutPath.c_str(), "i", osc_message_handler, nullptr);
    lo_server_thread_add_method(st, globalFadeOutPath.c_str(), NULL, osc_message_handler, nullptr);
    lo_server_thread_add_method(st, globalFadeOutPath.c_str(), "f", osc_message_handler, nullptr);
    lo_server_thread_add_method(st, globalFadeOutPath.c_str(), "i", osc_message_handler, nullptr);

    lo_server_thread_add_method(st, methodCrossPath.c_str(), NULL, osc_message_handler, nullptr);
    lo_server_thread_add_method(st, globalCrossPath.c_str(), NULL, osc_message_handler, nullptr);

    lo_server_thread_add_method(st, methodInfoPath.c_str(), NULL, osc_message_handler, nullptr);
    lo_server_thread_add_method(st, globalInfoPath.c_str(), NULL, osc_message_handler, nullptr);

    lo_server_thread_add_method(st, methodSpeedUpPath.c_str(), NULL, osc_message_handler, nullptr);
    lo_server_thread_add_method(st, globalSpeedUpPath.c_str(), NULL, osc_message_handler, nullptr);

    lo_server_thread_add_method(st, methodSpeedDownPath.c_str(), NULL, osc_message_handler, nullptr);
    lo_server_thread_add_method(st, globalSpeedDownPath.c_str(), NULL, osc_message_handler, nullptr);

    lo_server_thread_add_method(st, methodSpeedPath.c_str(), "i", osc_message_handler, nullptr);
    lo_server_thread_add_method(st, methodSpeedPath.c_str(), "f", osc_message_handler, nullptr);
    lo_server_thread_add_method(st, methodSpeedPath.c_str(), NULL, osc_message_handler, nullptr);
    lo_server_thread_add_method(st, globalSpeedPath.c_str(), "i", osc_message_handler, nullptr);
    lo_server_thread_add_method(st, globalSpeedPath.c_str(), "f", osc_message_handler, nullptr);
    lo_server_thread_add_method(st, globalSpeedPath.c_str(), NULL, osc_message_handler, nullptr);

    lo_server_thread_add_method(st, methodStopPath.c_str(), NULL, osc_message_handler, nullptr);
    lo_server_thread_add_method(st, globalStopPath.c_str(), NULL, osc_message_handler, nullptr);

    lo_server_thread_start(st);
    return st;
}

// Split text into lines
std::vector<std::string> splitTextIntoLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

// Create text textures
void createTextTextures(SDL_Renderer* renderer, TTF_Font* font, const std::string& message, std::vector<SDL_Texture*>& textures, std::vector<SDL_FRect>& rects, SDL_Window* window) {
    SDL_Color textColor = {255, 255, 255, 255}; 

    int windowWidth, windowHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

    // Split lines
    std::vector<std::string> lines = splitTextIntoLines(message);

    // Cleanup old textures
    for (auto texture : textures) {
        SDL_DestroyTexture(texture);
    }
    textures.clear(); 
    rects.clear();

    // Create textures for each line
    int yOffset = 0;
    int totalHeight = 0;
    for (const std::string& line : lines) {
        SDL_Surface* textSurface = TTF_RenderText_Blended(font, line.c_str(), 0, textColor);
        if (!textSurface) {
            std::cerr << "Text surface error: " << SDL_GetError() << std::endl;
            continue;
        }
        totalHeight += textSurface->h;
        SDL_DestroySurface(textSurface);
    }

    yOffset = (windowHeight - totalHeight) / 2;

    for (const std::string& line : lines) {
        SDL_Surface* textSurface = TTF_RenderText_Blended(font, line.c_str(), 0, textColor);
        if (!textSurface) {
            std::cerr << "Text surface error: " << SDL_GetError() << std::endl;
            continue;
        }
        SDL_Texture* textTexture = SDL_CreateTextureFromSurface(renderer, textSurface);
        if (!textTexture) {
            std::cerr << "Texture creation error: " << SDL_GetError() << std::endl;
            SDL_DestroySurface(textSurface);
            continue;
        }
        SDL_FRect textRect;
        textRect.w = (float)textSurface->w;
        textRect.h = (float)textSurface->h;
        textRect.x = (float)(windowWidth - textRect.w) / 2.0f;
        textRect.y = (float)yOffset;
        yOffset += textRect.h;

        textures.push_back(textTexture);
        rects.push_back(textRect);

        SDL_DestroySurface(textSurface);
    }
}

// Update textures on resize
void updateTextTextures(SDL_Renderer* renderer, SDL_Window* window) {
    int windowWidth, windowHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    int fontSize = isFullscreen ? windowHeight / 15 : windowHeight / 30;
    TTF_CloseFont(font);
    font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", fontSize);
    if (!font) {
        std::cerr << "Failed to load font: " << SDL_GetError() << std::endl;
        return;
    }
    createTextTextures(renderer, font, message, textTextures, textRects, window);
}
