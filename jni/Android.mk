LOCAL_PATH := $(call my-dir)

# ---- ImGui sebagai library statis (simbolnya disembunyikan agar tidak bentrok dengan ImGui menu ARM)
include $(CLEAR_VARS)
LOCAL_MODULE := imgui_static
LOCAL_SRC_FILES := ../imgui_src/imgui.cpp ../imgui_src/imgui_draw.cpp ../imgui_src/imgui_tables.cpp \
                   ../imgui_src/imgui_widgets.cpp ../imgui_src/backends/imgui_impl_opengl3.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../imgui_src $(LOCAL_PATH)/../imgui_src/backends
LOCAL_CPPFLAGS := -O2 -std=c++17 -fno-exceptions -fno-rtti -DIMGUI_IMPL_OPENGL_ES2 -DIMGUI_DISABLE_DEMO_WINDOWS
include $(BUILD_STATIC_LIBRARY)

# ---- mod
include $(CLEAR_VARS)
LOCAL_MODULE    := AntiKebalik
LOCAL_SRC_FILES := main.cpp
LOCAL_C_INCLUDES += $(LOCAL_PATH)/../aml_headers $(LOCAL_PATH)/../imgui_src $(LOCAL_PATH)/../imgui_src/backends
LOCAL_CPPFLAGS  := -O2 -std=c++17 -fno-exceptions -fno-rtti
LOCAL_STATIC_LIBRARIES := imgui_static
LOCAL_LDFLAGS   += -Wl,--exclude-libs,ALL
LOCAL_LDLIBS    := -llog -lEGL -lGLESv2 -ldl
include $(BUILD_SHARED_LIBRARY)
