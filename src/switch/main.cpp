/*
    Copyright 2018-2019 Hydr8gon

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <chrono>
#include <cstring>
#include <unistd.h>

#include "ui.h"

// Deal with conflicting typedefs
#define u64 u64_
#define s64 s64_

#include "PlatformConfig.h"
#include "../Platform.h"
#include "../Savestate.h"
#include "../GPU.h"
#include "../NDS.h"
#include "../SPU.h"

typedef struct
{
    vector<string> names;
    int *value;
} SettingValue;

string romPath, sramPath, statePath, sramStatePath;

u32 displayBuffer[256 * 384];
float topX, topY, topWidth, topHeight, botX, botY, botWidth, botHeight;

Thread core, audio, mic;
ClkrstSession cpuSession;
AppletHookCookie cookie;

AudioOutBuffer *audioReleasedBuffer;
AudioInBuffer micBuffer, *micReleasedBuffer;
u32 count;

u8 hotkeyMask;
bool lidClosed;

const int clockSpeeds[] = { 1020000000, 1224000000, 1581000000, 1785000000 };

const vector<string> controlNames =
{
    "A Button",
    "B Button",
    "Select Button",
    "Start Button",
    "D-Pad Right",
    "D-Pad Left",
    "D-Pad Up",
    "D-Pad Down",
    "R Button",
    "L Button",
    "X Button",
    "Y Button",
    "Close/Open Lid",
    "Microphone",
    "Pause Menu"
};

const vector<string> controlValues =
{
    "A Button", "B Button", "X Button", "Y Button",
    "Left Stick Click", "Right Stick Click",
    "L Button", "R Button", "ZL Button", "ZR Button",
    "Plus Button", "Minus Button",
    "D-Pad Left", "D-Pad Up", "D-Pad Right", "D-Pad Down",
    "Left Stick Left", "Left Stick Up", "Left Stick Right", "Left Stick Down",
    "Right Stick Left", "Right Stick Up", "Right Stick Right", "Right Stick Down"
};

const vector<string> settingNames =
{
    "Boot Game Directly",
    "Threaded 3D Renderer",
    "Frameskip",
    "Audio Volume",
    "Microphone Input",
    "Separate Savefiles from Savestates",
    "Screen Rotation",
    "Mid-Screen Gap",
    "Screen Layout",
    "Screen Sizing",
    "Screen Filtering",
    "Limit Framerate",
    "Switch Overclock"
};

const vector<SettingValue> settingValues =
{
    { { "Off", "On" },                                                               &Config::DirectBoot         },
    { { "Off", "On" },                                                               &Config::Threaded3D         },
    { { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9" },                          &Config::Frameskip          },
    { { "0%", "25%", "50%", "75%", "100%" },                                         &Config::AudioVolume        },
    { { "None", "Microphone", "White Noise" },                                       &Config::MicInputType       },
    { { "Off", "On" },                                                               &Config::SavestateRelocSRAM },
    { { "0", "90", "180", "270" },                                                   &Config::ScreenRotation     },
    { { "0 Pixels", "1 Pixel", "8 Pixels", "64 Pixels", "90 Pixels", "128 Pixels" }, &Config::ScreenGap          },
    { { "Natural", "Vertical", "Horizontal" },                                       &Config::ScreenLayout       },
    { { "Even", "Emphasize Top", "Emphasize Bottom" },                               &Config::ScreenSizing       },
    { { "Off", "On" },                                                               &Config::ScreenFilter       },
    { { "Off", "On" },                                                               &Config::LimitFPS           },
    { { "1020 MHz", "1224 MHz", "1581 MHz", "1785 MHz" },                            &Config::SwitchOverclock    }
};

const vector<string> pauseNames =
{
    "Resume",
    "Save State",
    "Load State",
    "Settings",
    "File Browser"
};

u32 *romIcon(string filename)
{
    FILE *rom = fopen(filename.c_str(), "rb");
    if (!rom)
        return NULL;

    u32 offset;
    fseek(rom, 0x68, SEEK_SET);
    fread(&offset, sizeof(u32), 1, rom);

    u8 data[512];
    fseek(rom, 0x20 + offset, SEEK_SET);
    fread(data, sizeof(u8), 512, rom);

    u16 palette[16];
    fseek(rom, 0x220 + offset, SEEK_SET);
    fread(palette, sizeof(u16), 16, rom);

    fclose(rom);

    // Get the 4-bit palette indexes
    u8 indexes[1024];
    for (int i = 0; i < 512; i++)
    {
        indexes[i * 2] = data[i] & 0x0F;
        indexes[i * 2 + 1] = data[i] >> 4;
    }

    // Get each pixel's 5-bit palette color and convert it to 8-bit
    u32 tiles[32 * 32];
    for (int i = 0; i < 1024; i++)
    {
        u8 r = ((palette[indexes[i]] >> 0)  & 0x1F) * 255 / 31;
        u8 g = ((palette[indexes[i]] >> 5)  & 0x1F) * 255 / 31;
        u8 b = ((palette[indexes[i]] >> 10) & 0x1F) * 255 / 31;
        tiles[i] = rgbaToU32(r, g, b, 255);
    }

    // Rearrange the pixels from 8x8 tiles to a 32x32 texture
    u32 *tex = new u32[32 * 32];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            for (int k = 0; k < 4; k++)
                memcpy(&tex[256 * i + 32 * j + 8 * k], &tiles[256 * i + 8 * j + 64 * k], 8 * sizeof(u32));

    return tex;
}

void swapValues(float *val1, float *val2)
{
    float temp = *val1;
    *val1 = *val2;
    *val2 = temp;
}

void setScreenLayout()
{
    int gapSizes[] = { 0, 1, 8, 64, 90, 128 };
    float gap = gapSizes[Config::ScreenGap];

    if (Config::ScreenLayout == 0) // Natural, choose based on rotation
        Config::ScreenLayout = (Config::ScreenRotation % 2 == 0) ? 1 : 2;

    if (Config::ScreenLayout == 1) // Vertical
    {
        if (Config::ScreenSizing == 0) // Even
        {
            topHeight = botHeight = 360 - gap / 2;
            if (Config::ScreenRotation % 2 == 0)
                topWidth = botWidth = topHeight * 4 / 3;
            else
                topWidth = botWidth = topHeight * 3 / 4;
        }
        else if (Config::ScreenSizing == 1) // Emphasize top
        {
            if (Config::ScreenRotation % 2 == 0) // 0, 180
            {
                botWidth = 256;
                botHeight = 192;
                topHeight = 720 - botHeight - gap;
                topWidth = topHeight * 4 / 3;
            }
            else // 90, 270
            {
                botWidth = 192;
                botHeight = 256;
                topHeight = 720 - botHeight - gap;
                topWidth = topHeight * 3 / 4;
            }
        }
        else // Emphasize bottom
        {
            if (Config::ScreenRotation % 2 == 0) // 0, 180
            {
                topWidth = 256;
                topHeight = 192;
                botHeight = 720 - topHeight - gap;
                botWidth = botHeight * 4 / 3;
            }
            else // 90, 270
            {
                botWidth = 192;
                botHeight = 256;
                topHeight = 720 - botHeight - gap;
                topWidth = topHeight * 3 / 4;
            }
        }

        topX = 640 - topWidth / 2;
        botX = 640 - botWidth / 2;
        topY = 0;
        botY = 720 - botHeight;
    }
    else // Horizontal
    {
        if (Config::ScreenRotation % 2 == 0) // 0, 180
        {
            topWidth = botWidth = 640 - gap / 2;
            topHeight = botHeight = topWidth * 3 / 4;
            topX = 0;
            botX = 1280 - topWidth;
        }
        else // 90, 270
        {
            topHeight = botHeight = 720;
            topWidth = botWidth = topHeight * 3 / 4;
            topX = 640 - topWidth - gap / 2;
            botX = 640 + gap / 2;
        }

        topY = botY = 360 - topHeight / 2;

        if (Config::ScreenSizing == 1) // Emphasize top
        {
            if (Config::ScreenRotation % 2 == 0) // 0, 180
            {
                botWidth = 256;
                botHeight = 192;
                topWidth = 1280 - botWidth - gap;
                if (topWidth > 960)
                    topWidth = 960;
                topHeight = topWidth * 3 / 4;
                topX = 640 - (botWidth + topWidth + gap) / 2;
                botX = topX + topWidth + gap;
                topY = 360 - topHeight / 2;
                botY = topY + topHeight - botHeight;
            }
            else // 90, 270
            {
                botWidth = 192;
                botHeight = 256;
                topX += (topWidth - botWidth) / 2;
                botX += (topWidth - botWidth) / 2;
                botY = 720 - botHeight;
            }
        }
        else if (Config::ScreenSizing == 2) // Emphasize bottom
        {
            if (Config::ScreenRotation % 2 == 0) // 0, 180
            {
                topWidth = 256;
                topHeight = 192;
                botWidth = 1280 - topWidth - gap;
                if (botWidth > 960)
                    botWidth = 960;
                botHeight = botWidth * 3 / 4;
                topX = 640 - (botWidth + topWidth + gap) / 2;
                botX = topX + topWidth + gap;
                botY = 360 - botHeight / 2;
                topY = botY + botHeight - topHeight;
            }
            else // 90, 270
            {
                topWidth = 192;
                topHeight = 256;
                topX += (botWidth - topWidth) / 2;
                botX -= (botWidth - topWidth) / 2;
                topY = 720 - topHeight;
            }
        }
    }

    // Swap the top and bottom screens for 90 and 180 degrees
    if (Config::ScreenRotation == 1 || Config::ScreenRotation == 2)
    {
        swapValues(&topX, &botX);
        swapValues(&topY, &botY);
        swapValues(&topWidth, &botWidth);
        swapValues(&topHeight, &botHeight);
    }

    setTextureFiltering(Config::ScreenFilter);
}

void onAppletHook(AppletHookType hook, void *param)
{
    if (hook == AppletHookType_OnOperationMode || hook == AppletHookType_OnPerformanceMode)
    {
        if (R_FAILED(pcvSetClockRate(PcvModule_Cpu, clockSpeeds[Config::SwitchOverclock])))
            clkrstSetClockRate(&cpuSession, clockSpeeds[Config::SwitchOverclock]);
    }
}

void runCore(void *args)
{
    while (!(hotkeyMask & BIT(2)))
    {
        chrono::steady_clock::time_point start = chrono::steady_clock::now();

        NDS::RunFrame();
        memcpy(displayBuffer, GPU::Framebuffer, sizeof(GPU::Framebuffer));

        chrono::duration<double> elapsed = chrono::steady_clock::now() - start;
        if (Config::LimitFPS && elapsed.count() < 1.0f / 60)
            usleep((1.0f / 60 - elapsed.count()) * 1000000);
    }
}

void fillAudioBuffer()
{
    // 1024 samples is equal to approximately 700 at the original rate
    s16 buf_in[700 * 2];
    s16 *buf_out = (s16*)audioReleasedBuffer->buffer;

    int num_in = SPU::ReadOutput(buf_in, 700);

    int margin = 6;
    if (num_in < 700 - margin)
    {
        int last = num_in - 1;
        if (last < 0)
            last = 0;

        for (int i = num_in; i < 700 - margin; i++)
            ((u32*)buf_in)[i] = ((u32*)buf_in)[last];

        num_in = 700 - margin;
    }

    float res_incr = (float)num_in / 1024;
    float res_timer = 0;
    int res_pos = 0;

    for (int i = 0; i < 1024; i++)
    {
        buf_out[i * 2]     = (buf_in[res_pos * 2]     * Config::AudioVolume * 64) >> 8;
        buf_out[i * 2 + 1] = (buf_in[res_pos * 2 + 1] * Config::AudioVolume * 64) >> 8;

        res_timer += res_incr;
        while (res_timer >= 1)
        {
            res_timer--;
            res_pos++;
        }
    }
}

void audioOutput(void *args)
{
    while (!(hotkeyMask & BIT(2)))
    {
        audoutWaitPlayFinish(&audioReleasedBuffer, &count, U64_MAX);
        fillAudioBuffer();
        audoutAppendAudioOutBuffer(audioReleasedBuffer);
    }
}

void micInput(void *args)
{
    while (!(hotkeyMask & BIT(2)))
    {
        if (Config::MicInputType == 0 || !(hotkeyMask & BIT(1)))
        {
            NDS::MicInputFrame(NULL, 0);
        }
        else if (Config::MicInputType == 1)
        {
            audinCaptureBuffer(&micBuffer, &micReleasedBuffer);
            NDS::MicInputFrame((s16*)micBuffer.buffer, 1440);
        }
        else
        {
            s16 input[1440];
            for (int i = 0; i < 1440; i++)
                input[i] = rand() & 0xFFFF;
            NDS::MicInputFrame(input, 1440);
        }
    }
}

void startCore(bool reset)
{
    setScreenLayout();

    appletLockExit();
    appletHook(&cookie, onAppletHook, NULL);

    if (reset)
    {
        sramPath = romPath.substr(0, romPath.rfind(".")) + ".sav";
        statePath = romPath.substr(0, romPath.rfind(".")) + ".mln";
        sramStatePath = statePath + ".sav";

        NDS::Init();
        NDS::LoadROM(romPath.c_str(), sramPath.c_str(), Config::DirectBoot);
    }

    threadCreate(&core, runCore, NULL, 0x8000, 0x30, 1);
    //threadStart(&core);

    if (Config::AudioVolume > 0)
    {
        audoutInitialize();
        audoutStartAudioOut();
        setupAudioBuffer();
        threadCreate(&audio, audioOutput, NULL, 0x8000, 0x2F, 0);
        threadStart(&audio);
    }
    if (Config::MicInputType == 1)
    {
        audinInitialize();
        audinStartAudioIn();
        threadCreate(&mic, micInput, NULL, 0x8000, 0x30, 0);
        threadStart(&mic);
    }
    if (Config::SwitchOverclock > 0)
    {
        pcvInitialize();
        if (R_FAILED(pcvSetClockRate(PcvModule_Cpu, clockSpeeds[Config::SwitchOverclock])))
        {
            clkrstInitialize();
            clkrstOpenSession(&cpuSession, PcvModule_Cpu);
            clkrstSetClockRate(&cpuSession, clockSpeeds[Config::SwitchOverclock]);
        }
    }
}

void pauseCore()
{
    //threadWaitForExit(&core);
    threadWaitForExit(&audio);
    threadWaitForExit(&mic);
    if (R_FAILED(pcvSetClockRate(PcvModule_Cpu, clockSpeeds[0])))
    {
        clkrstSetClockRate(&cpuSession, clockSpeeds[0]);
        clkrstExit();
    }
    pcvExit();
    audinStopAudioIn();
    audinExit();
    audoutStopAudioOut();
    audoutExit();
    appletUnhook(&cookie);
    appletUnlockExit();
}

void controlsMenu()
{
    int selection = 0;

    while (true)
    {
        vector<string> controlSubitems;
        for (unsigned int i = 0; i < controlNames.size(); i++)
        {
            if (Config::Mapping[i] == 0)
            {
                controlSubitems.push_back("None");
            }
            else
            {
                string subitem;
                int count = 0;
                for (unsigned int j = 0; j < controlValues.size(); j++)
                {
                    if (Config::Mapping[i] & BIT(j))
                    {
                        count++;
                        if (count < 5)
                        {
                            subitem += controlValues[j] + ", ";
                        }
                        else
                        {
                            subitem += "...";
                            break;
                        }
                    }
                }
                controlSubitems.push_back(subitem.substr(0, subitem.size() - ((count == 5) ? 0 : 2)));
            }
        }

        u32 pressed = menuScreen("Controls", "", "Clear", {}, controlNames, controlSubitems, &selection);

        if (pressed & KEY_A)
        {
            pressed = 0;
            while (pressed == 0 || pressed > KEY_RSTICK_DOWN)
                pressed = messageScreen("Controls", {"Press a button to add a mapping to: " + controlNames[selection]}, false);
            Config::Mapping[selection] |= pressed;
        }
        else if (pressed & KEY_B)
        {
            return;
        }
        else if ((pressed & KEY_X) && !(pressed & KEY_TOUCH))
        {
            Config::Mapping[selection] = 0;
        }
    }
}

void settingsMenu()
{
    int selection = 0;

    while (true)
    {
        vector<string> settingSubitems;
        for (unsigned int i = 0; i < settingNames.size(); i++)
            settingSubitems.push_back(settingValues[i].names[*settingValues[i].value]);

        u32 pressed = menuScreen("Settings", "", "Controls", {}, settingNames, settingSubitems, &selection);

        if (pressed & KEY_A)
        {
            (*settingValues[selection].value)++;
            if (*settingValues[selection].value >= (int)settingValues[selection].names.size())
                *settingValues[selection].value = 0;
        }
        else if (pressed & KEY_B)
        {
            Config::Save();
            return;
        }
        else if (pressed & KEY_X)
        {
            controlsMenu();
        }
    }
}

bool fileBrowser()
{
    int selection = 0;
    romPath = Config::LastROMFolder;

    while (true)
    {
        vector<string> files = dirContents(romPath, ".nds");
        vector<Icon> icons;

        for (unsigned int i = 0; i < files.size(); i++)
        {
            if (files[i].find(".nds", (files[i].length() - 4)) != string::npos)
                icons.push_back({romIcon(romPath + "/" + files[i]), 32});
            else
                icons.push_back({folderIcon, 64});
        }

        u32 pressed = menuScreen("melonDS", "Exit", "Settings", icons, files, {}, &selection);

        if (pressed & KEY_A && files.size() > 0)
        {
            romPath += "/" + files[selection];
            selection = 0;

            if (romPath.find(".nds", romPath.length() - 4) != string::npos)
                break;
        }
        else if (pressed & KEY_B && romPath != "sdmc:/")
        {
            romPath = romPath.substr(0, romPath.rfind("/"));
            selection = 0;
        }
        else if (pressed & KEY_X)
        {
            settingsMenu();
        }
        else if (pressed & KEY_PLUS)
        {
            return false;
        }
    }

    string folder = romPath.substr(0, romPath.rfind("/"));
    folder.append(1, '\0');
    strncpy(Config::LastROMFolder, folder.c_str(), folder.length());

    return true;
}

bool pauseMenu()
{
    pauseCore();

    int selection = 0;

    while (true)
    {
        u32 pressed = menuScreen("melonDS", "", "", {}, pauseNames, {}, &selection);

        if (pressed & KEY_A)
        {
            if (selection == 0) // Resume
            {
                break;
            }
            else if (selection == 1 || selection == 2) // Save/load state
            {
                Savestate *state = new Savestate(const_cast<char*>(statePath.c_str()), selection == 1);
                if (!state->Error)
                {
                    NDS::DoSavestate(state);
                    if (Config::SavestateRelocSRAM)
                        NDS::RelocateSave(const_cast<char*>(sramStatePath.c_str()), selection == 1);
                }
                delete state;
                break;
            }
            else if (selection == 3) // Settings
            {
                settingsMenu();
            }
            else // File browser
            {
                NDS::DeInit();
                if (fileBrowser())
                {
                    hotkeyMask &= ~BIT(2);
                    startCore(true);
                    return true;
                }
                return false;
            }
        }
        else if (pressed & KEY_B)
        {
            break;
        }
    }

    hotkeyMask &= ~BIT(2);
    startCore(false);
    return true;
}

int main(int argc, char **argv)
{
    initRenderer();
    Config::Load();

    // The old volume setting was scaled differently, so save upgrading users from ear rape
    if (Config::AudioVolume > 4)
    {
        Config::AudioVolume = 4;
        Config::Save();
    }

    if (!fileBrowser())
    {
        deinitRenderer();
        return 0;
    }

    if (!Platform::LocalFileExists("bios7.bin") || !Platform::LocalFileExists("bios9.bin") ||
        !Platform::LocalFileExists("firmware.bin"))
    {
        vector<string> message =
        {
            "One or more of the following required files don't exist or couldn't be accessed:",
            "bios7.bin -- ARM7 BIOS",
            "bios9.bin -- ARM9 BIOS",
            "firmware.bin -- firmware image",
            "Dump the files from your DS and place them in sdmc:/switch/melonds"
        };

        messageScreen("BIOS/Firmware not found", message, true);
        deinitRenderer();
        return 0;
    }

    micBuffer.next = NULL;
    micBuffer.buffer = new s16[(1440 * 2 + 0xFFF) & ~0xFFF];
    micBuffer.buffer_size = (1440 * 2 * sizeof(s16) + 0xFFF) & ~0xFFF;
    micBuffer.data_size = 1440 * 2 * sizeof(s16);
    micBuffer.data_offset = 0;

    startCore(true);

    while (appletMainLoop())
    {
        hidScanInput();
        u32 pressed = hidKeysDown(CONTROLLER_P1_AUTO);
        u32 released = hidKeysUp(CONTROLLER_P1_AUTO);

        // Check for button input
        for (int i = 0; i < 12; i++)
        {
            if (pressed & Config::Mapping[i])
                NDS::PressKey(i > 9 ? i + 6 : i);
            else if (released & Config::Mapping[i])
                NDS::ReleaseKey(i > 9 ? i + 6 : i);
        }

        // Check for hotkey input
        for (int i = 12; i < 15; i++)
        {
            if (pressed & Config::Mapping[i])
                hotkeyMask |= BIT(i - 12);
            else if (released & Config::Mapping[i])
                hotkeyMask &= ~BIT(i - 12);
        }

        if (hotkeyMask & BIT(0)) // Lid close/open
        {
            lidClosed = !lidClosed;
            NDS::SetLidClosed(lidClosed);
            hotkeyMask &= ~BIT(0);
        }
        else if (hotkeyMask & BIT(2)) // Pause menu
        {
            if (!pauseMenu())
                break;
        }

        // Check for touch input
        if (hidTouchCount() > 0)
        {
            touchPosition touch;
            hidTouchRead(&touch, 0);

            if (touch.px > botX && touch.px < botX + botWidth && touch.py > botY && touch.py < botY + botHeight)
            {
                int x, y;
                if (Config::ScreenRotation == 0) // 0
                {
                    x = (touch.px - botX) * 256.0f / botWidth;
                    y = (touch.py - botY) * 256.0f / botWidth;
                }
                else if (Config::ScreenRotation == 1) // 90
                {
                    x =       (touch.py - botY) * 192.0f / botWidth;
                    y = 192 - (touch.px - botX) * 192.0f / botWidth;
                }
                else if (Config::ScreenRotation == 2) // 180
                {
                    x =       (touch.px - botX) * -256.0f / botWidth;
                    y = 192 - (touch.py - botY) *  256.0f / botWidth;
                }
                else // 270
                {
                    x = (touch.py - botY) * -192.0f / botWidth;
                    y = (touch.px - botX) *  192.0f / botWidth;
                }
                NDS::PressKey(16 + 6);
                NDS::TouchScreen(x, y);
            }
        }
        else
        {
            NDS::ReleaseKey(16 + 6);
            NDS::ReleaseScreen();
        }

        NDS::RunFrame();
        memcpy(displayBuffer, GPU::Framebuffer, sizeof(GPU::Framebuffer));

        clearDisplay(0);
        drawImage(displayBuffer, 256, 192, true, topX, topY, topWidth, topHeight, Config::ScreenRotation);
        drawImage(&displayBuffer[256 * 192], 256, 192, true, botX, botY, botWidth, botHeight, Config::ScreenRotation);
        refreshDisplay();
    }

    hotkeyMask |= BIT(2);
    pauseCore();
    deinitRenderer();
    return 0;
}
