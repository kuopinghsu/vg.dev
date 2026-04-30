# OpenVG Reference Implementation + Tiger Sample
# Output: ri/build/libOpenVG.a  and  ri/build/tiger

# --------------------------------------------------------------------------
# Directories
# --------------------------------------------------------------------------
RI_SRC    := ri/src
RI_INC    := ri/include
BUILD     ?= build

# --------------------------------------------------------------------------
# Toolchain
# --------------------------------------------------------------------------
CXX       := g++
CC        := gcc
AR        := ar
ARFLAGS   := rcs

# --------------------------------------------------------------------------
# Compiler flags
# --------------------------------------------------------------------------
CPPFLAGS  := -I$(RI_INC) \
             -I$(RI_INC)/VG \
             -I$(RI_INC)/EGL \
             -I$(RI_SRC) \
             -DOPENVG_STATIC_LIBRARY \
             -DEGL_STATIC_LIBRARY \
             -D_GNU_SOURCE

CXXFLAGS  := -O2 -std=c++11 -Wall -fno-strict-aliasing \
             -fpermissive \
             -Wno-int-to-pointer-cast \
             -Wno-catch-value

CFLAGS    := -O2 -std=c99  -Wall

# --------------------------------------------------------------------------
# RI library sources (null OS back-end, no window system required)
# --------------------------------------------------------------------------
RI_SRCS   := $(RI_SRC)/riApi.cpp        \
             $(RI_SRC)/riContext.cpp     \
             $(RI_SRC)/riFont.cpp        \
             $(RI_SRC)/riImage.cpp       \
             $(RI_SRC)/riMath.cpp        \
             $(RI_SRC)/riMiniEGL.cpp     \
             $(RI_SRC)/riPath.cpp        \
             $(RI_SRC)/riPixelPipe.cpp   \
             $(RI_SRC)/riRasterizer.cpp  \
             $(RI_SRC)/riVGU.cpp         \
             $(RI_SRC)/null/riEGLOS.cpp

RI_OBJS   := $(patsubst $(RI_SRC)/%.cpp, $(BUILD)/%.o, $(RI_SRCS))

RI_LIB    := $(BUILD)/libOpenVG.a

# --------------------------------------------------------------------------
# Tiger sample sources
# --------------------------------------------------------------------------
TIGER_SRCS := samples/tiger/main.c   \
              samples/tiger/tiger.c

TIGER_OBJS := $(patsubst samples/tiger/%.c, $(BUILD)/%.o, $(TIGER_SRCS))

TIGER_BIN  := $(BUILD)/tiger

# --------------------------------------------------------------------------
# Default target
# --------------------------------------------------------------------------
.PHONY: all clean

all: $(TIGER_BIN)

# --------------------------------------------------------------------------
# Link tiger
# --------------------------------------------------------------------------
$(TIGER_BIN): $(TIGER_OBJS) $(RI_LIB)
	$(CXX) -o $@ $(TIGER_OBJS) -L$(BUILD) -lOpenVG -lm -lstdc++

# --------------------------------------------------------------------------
# Compile tiger C sources
# --------------------------------------------------------------------------
$(BUILD)/main.o: samples/tiger/main.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/tiger.o: samples/tiger/tiger.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# --------------------------------------------------------------------------
# Build RI static library
# --------------------------------------------------------------------------
$(RI_LIB): $(RI_OBJS)
	$(AR) $(ARFLAGS) $@ $^

# RI .cpp → .o (generic rule, handles sub-directories)
$(BUILD)/%.o: $(RI_SRC)/%.cpp | $(BUILD)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/null/%.o: $(RI_SRC)/null/%.cpp | $(BUILD)
	@mkdir -p $(BUILD)/null
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

# --------------------------------------------------------------------------
# Create build directory
# --------------------------------------------------------------------------
$(BUILD):
	mkdir -p $(BUILD)

# --------------------------------------------------------------------------
# Run: render tiger and save tiger.ppm in ri/
# --------------------------------------------------------------------------
.PHONY: run
run: $(TIGER_BIN)
	$(TIGER_BIN) tiger.ppm 512

# --------------------------------------------------------------------------
# Clean
# --------------------------------------------------------------------------
clean:
	rm -rf $(BUILD) tiger.ppm
