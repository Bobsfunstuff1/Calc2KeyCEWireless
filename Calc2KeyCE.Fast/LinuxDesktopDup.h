#pragma once
#include <cstdint>

class LinuxDesktopDup {
public:
    ~LinuxDesktopDup();
    bool Initialize();
    void Close();
    bool CaptureNext(uint8_t* dest);

private:
    int width = 0;
    int height = 0;
};
