CXX = g++
CXXFLAGS = -Wall -O2 -pthread $(shell pkg-config --cflags sdl3 sdl3-image sdl3-ttf liblo libavformat libavcodec libavutil libswscale libswresample)
LIBS = $(shell pkg-config --libs sdl3 sdl3-image sdl3-ttf liblo libavformat libavcodec libavutil libswscale libswresample)

TARGET = romp
SRCS = main.cpp utils.cpp video_player.cpp
OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $(TARGET) $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

install: all
	install -d /usr/local/bin
	install -m 755 $(TARGET) /usr/local/bin/
	@echo "Installation complete. Run with: romp"

.PHONY: all clean install