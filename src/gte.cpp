#include "SDL_inc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmath>
#include <time.h>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>
#include <thread>
#include <algorithm>
#ifdef WASM_BUILD
#include "emscripten.h"
#include <emscripten/html5.h>
#else
#include "tinyfd/tinyfiledialogs.h"
#endif

#include "joystick_adapter.h"
#include "audio_coprocessor.h"
#include "blitter.h"
#include "palette.h"

#include "timekeeper.h"
#include "system_state.h"
#include "emulator_config.h"
#include "game_config.h"

#include "mos6502/mos6502.h"

#include "devtools/memory_map.h"
#include "devtools/breakpoints.h"
#include "devtools/source_map.h"

#include "ui/ui_utils.h"
#include "devtools/profiler.h"
#include "devtools/disassembler.h"

#ifndef WASM_BUILD
#include "devtools/profiler_window.h"
#include "devtools/mem_browser_window.h"
#include "devtools/vram_window.h"
#include "devtools/stepping_window.h"
#include "devtools/patching_window.h"
#include "devtools/controller_options_window.h"
#include "imgui.h"
#include "implot.h"
#include "imgui/backends/imgui_impl_sdl2.h"
#include "imgui/backends/imgui_impl_sdlrenderer2.h"
#include "whereami/whereami.h"
#endif

#ifndef WINDOW_TITLE
#define WINDOW_TITLE "Brick Game"
#endif

using namespace std;

const int GT_WIDTH = 128;
const int GT_HEIGHT = 128;

RomType loadedRomType;

mos6502 *cpu_core;
Blitter *blitter;
AudioCoprocessor *soundcard;
JoystickAdapter *joysticks;
SystemState system_state;
CartridgeState cartridge_state;

const int SCREEN_WIDTH = 683;	
const int SCREEN_HEIGHT = 512;
RGB_Color *palette;

MemoryMap* loadedMemoryMap;
GameConfig* gameconfig;
std::string currentRomFilePath;
std::string nvramFileFullPath;
std::string flashFileFullPath;

bool vsyncProfileArmed = false;
bool vsyncProfileRunning = false;

bool showMenu = false;
bool menuOpening = false;
int resetQueued = 0;
#define MUTE_SOURCE_MANUAL 1
#define MUTE_SOURCE_MENU 2
int muteMask = 0;
bool paddle_emulation_enabled = false;//user set, overrides joystick behavior

#define NTSC_RES_SCALE_DEFAULT 2.0f
#define NTSC_BLOOM_DECAY_DEFAULT 0.8f
#define NTSC_COLOR_SHIFT_DEFAULT 0.75f
#define NTSC_FILTER_ENABLED_DEFAULT false
#define NTSC_BLOOM_ENABLED_DEFAULT false
#define NTSC_PHOSPHOR_BLENDING_ENABLED_DEFAULT false
bool ntsc_filter_enabled = NTSC_FILTER_ENABLED_DEFAULT;
float ntsc_res_scale = NTSC_RES_SCALE_DEFAULT;//3
bool ntsc_bloom_enabled = NTSC_BLOOM_ENABLED_DEFAULT;
float ntsc_bloom_decay = NTSC_BLOOM_DECAY_DEFAULT;//.75
float ntsc_color_shift = NTSC_COLOR_SHIFT_DEFAULT;////1.5f;
bool phosphor_blending_enabled = NTSC_PHOSPHOR_BLENDING_ENABLED_DEFAULT;
bool paddle_touch_mode = false;
bool paddleDetected = false;
bool dksPaddleDetected = false;
bool use_any_joystick_as_paddle = true;//this needs to be on at all times, hard-coded
int paddle_device_index = 0; //use only if use_any_joystick_as_paddle is enabled
int paddle_axis_index = 0; //use only if use_any_joystick_as_paddle is enabled
bool romRequestedPaddle = false; // source of truth
SDL_JoystickID dksPaddle_instanceID = -1;
int32_t currentPaddleRawValue = 0;
#define SIGNAL_PADDLE_MODE 0xA5
#define RECEIVE_PADDLE_MODE_ADDRESS 0x2009 //memory location

// Keep a global or static pointer to track the currently open active joystick
SDL_Joystick* active_paddle_handle = NULL;

void PaddleInit() {
    int num_joysticks = SDL_NumJoysticks();
    const char* nametest = NULL;
    SDL_JoystickID bakInstanceID = -1;

    // Verify if our existing selection is still valid and branded a Paddle
    if (dksPaddle_instanceID != -1 && active_paddle_handle != NULL) {
        bakInstanceID = dksPaddle_instanceID;
        nametest = SDL_JoystickName(active_paddle_handle);
        if (nametest != NULL && strstr(nametest, "Paddle") != NULL) {
            return; // Active device is already a valid hardware paddle. Exit.
        }
    }

    // Clear old state tracking variables
    paddleDetected = false; 
	dksPaddleDetected = false;
    dksPaddle_instanceID = -1;
    
    SDL_Joystick* chosen_joystick = NULL;
    int chosen_index = -1;
    const char* chosen_name = NULL;

    // Scan available devices
    for (int i = 0; i < num_joysticks; i++) {
        const char* name = SDL_JoystickNameForIndex(i);
        SDL_Joystick* j = SDL_JoystickOpen(i); 
        if (!j) continue;

        // Take the first available joystick as a baseline fallback
        if (chosen_joystick == NULL) {
            chosen_joystick = j;
            chosen_index = i;
            chosen_name = name;
        }

        // If we hit a branded paddle, it takes absolute priority
        if (name != NULL && strstr(name, "Paddle") != NULL) {
			dksPaddleDetected = true;
            // Close the previous fallback handle if we had one open
            if (chosen_joystick != NULL && chosen_joystick != j) {
                SDL_JoystickClose(chosen_joystick);
            }
            chosen_joystick = j;
            chosen_index = i;
            chosen_name = name;
            break; // Stop scanning immediately
        }

        // Close this joystick handle if it isn't our fallback or a paddle candidate
        if (j != chosen_joystick) {
            SDL_JoystickClose(j);
        }
    }

    // Apply the chosen selection state
    if (chosen_joystick != NULL) {
        // Close the completely old global handle if it changed
        if (active_paddle_handle != NULL && active_paddle_handle != chosen_joystick) {
            SDL_JoystickClose(active_paddle_handle);
        }

        active_paddle_handle = chosen_joystick;
        dksPaddle_instanceID = SDL_JoystickInstanceID(active_paddle_handle);
        paddle_device_index = chosen_index;
        paddle_axis_index = 0;
        paddleDetected = true;

        if (dksPaddle_instanceID != bakInstanceID) {
            printf("Hardware Verified: %s (Instance ID: %d)\n", 
                   chosen_name ? chosen_name : "Unknown", dksPaddle_instanceID);
        }
    } else {
        // No devices found at all, clean up the global handle
        if (active_paddle_handle != NULL) {
            SDL_JoystickClose(active_paddle_handle);
            active_paddle_handle = NULL;
        }
    }
}

// Static or global variables to track the timer
static uint32_t lastPaddleCheck = 0;
const uint32_t PADDLE_CHECK_INTERVAL = 1000; // Check every 1 second

// void UpdatePaddleStatus() {
//     // Only run the scan if we don't have a paddle yet
//     //if (!paddleDetected) { //runs no matter what, because someone might plug in multiple controllers like a weirdo
// 	uint32_t currentTime = SDL_GetTicks();
// 	if (currentTime - lastPaddleCheck > PADDLE_CHECK_INTERVAL) {
// 		PaddleInit();
// 		lastPaddleCheck = currentTime;
// 	}
//     //}
// }

void SavePreferences() {
    std::ofstream file("emulator_prefs.cfg");
    if (file.is_open()) {
        file << "paddle_emulation_enabled=" << (paddle_emulation_enabled ? "1" : "0") << "\n";
        file << "ntsc_filter_enabled=" << (ntsc_filter_enabled ? "1" : "0") << "\n";
        file << "phosphor_blending_enabled=" << (phosphor_blending_enabled ? "1" : "0") << "\n";
        file << "ntsc_res_scale=" << (ntsc_res_scale) << "\n";
		file << "ntsc_color_shift=" << (ntsc_color_shift) << "\n";//float
		file << "ntsc_bloom_enabled=" << (ntsc_bloom_enabled ? "1" : "0") << "\n";
		file << "ntsc_bloom_decay=" << (ntsc_bloom_decay) << "\n";//float
        file.close();
    }
}

void LoadPreferences() {
    // 1. Establish hardcoded default states (off by default)
    paddle_emulation_enabled = false;
    ntsc_filter_enabled = NTSC_FILTER_ENABLED_DEFAULT;
    phosphor_blending_enabled = NTSC_PHOSPHOR_BLENDING_ENABLED_DEFAULT;
	ntsc_bloom_enabled = NTSC_BLOOM_ENABLED_DEFAULT;
	ntsc_res_scale = NTSC_RES_SCALE_DEFAULT;
	ntsc_color_shift = NTSC_COLOR_SHIFT_DEFAULT;//float
	ntsc_bloom_decay = NTSC_BLOOM_DECAY_DEFAULT;//float

    std::ifstream file("emulator_prefs.cfg");
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            // Skip empty lines or lines meant as comments
            if (line.empty() || line[0] == '#') continue;

            size_t delim_pos = line.find('=');
            if (delim_pos != std::string::npos) {
                std::string key = line.substr(0, delim_pos);
                std::string val_str = line.substr(delim_pos + 1);
                
                // Convert value string to integer (0 or 1)
                int val = std::atoi(val_str.c_str());

                // 2. Map explicit keys to matching global states
                if (key == "paddle_emulation_enabled") {
                    paddle_emulation_enabled = (val != 0);
                } else if (key == "ntsc_filter_enabled") {
                    ntsc_filter_enabled = (val != 0);
                } else if (key == "phosphor_blending_enabled") {
                    phosphor_blending_enabled = (val != 0);
                } else if (key == "ntsc_res_scale") {
                    //ntsc_res_scale = val;
					ntsc_res_scale = (float)std::atof(val_str.c_str()); // Use atof for float conversion
                } else if (key == "ntsc_bloom_enabled"){
					ntsc_bloom_enabled = val;
				} else if (key == "ntsc_color_shift") {
    				ntsc_color_shift = (float)std::atof(val_str.c_str()); // Use atof for float conversion
            	} else if (key == "ntsc_bloom_decay") {
    				ntsc_bloom_decay = (float)std::atof(val_str.c_str()); // Use atof for float conversion
            	}
			}
        }
        file.close();
    }
}

void SaveNVRAM() {
	fstream file;
	if(loadedRomType != RomType::FLASH2M_RAM32K) return;
	printf("SAVING %s\n", nvramFileFullPath.c_str());
	file.open(nvramFileFullPath.c_str(), ios_base::out | ios_base::binary | ios_base::trunc);
	file.write((char*) cartridge_state.save_ram, CARTRAMSIZE);
	file.close();
}

void LoadNVRAM() {
	fstream file;
	if(loadedRomType != RomType::FLASH2M_RAM32K) return;
	printf("LOADING %s\n", nvramFileFullPath.c_str());
	file.open(nvramFileFullPath.c_str(), ios_base::in | ios_base::binary);
	file.read((char*) cartridge_state.save_ram, CARTRAMSIZE);
	file.close();
}

std::thread savingThread;

void SaveModifiedFlash() {
	if(EmulatorConfig::noSave) return;
	fstream file_out, file_in;
	uint8_t* rom_cursor = cartridge_state.rom;
	uint8_t buf[256];
	file_in.open(currentRomFilePath, ios_base::in | ios_base::binary);
	file_out.open(flashFileFullPath.c_str(), ios_base::out | ios_base::binary | ios_base::trunc);
	while(file_in) {
		file_in.read((char*) buf, 256);
		size_t bytesRead = file_in.gcount();
		if(bytesRead) {
			for(int i = 0; i < bytesRead; ++i) {
				buf[i] ^= *(rom_cursor++);
			}
			file_out.write((char*) buf, bytesRead);
		}
	}
	file_in.close();
	file_out.close();
#ifdef WASM_BUILD
	EM_ASM(
		FS.syncfs(false, function (err) {
			assert(!err);
			});
	);
#endif
}

fstream orig_rom, xor_file;
void LoadModifiedFlash() {
	uint8_t* rom_cursor = cartridge_state.rom;
	uint8_t buf[256];
	uint8_t bufx[256];
	size_t bytes_read = 0;
	std::cout << "opening " << currentRomFilePath << " and " << flashFileFullPath << "\n";
	orig_rom.open(currentRomFilePath, ios_base::in | ios_base::binary);
	xor_file.open(flashFileFullPath, ios_base::in | ios_base::binary);
	std::cout << "XORing files together... \n";
	while(orig_rom && xor_file) {
		orig_rom.read((char*) buf, 256);
		xor_file.read((char*) bufx, 256); 
		for(int i = 0; i < orig_rom.gcount(); ++i) {
			*(rom_cursor++) = buf[i] ^ bufx[i];
		}
		bytes_read += 256;
	}
	std::cout << bytes_read << " bytes read from xor file\n";
#ifndef WASM_BUILD
	orig_rom.close();
	xor_file.close();
#endif
}

const uint8_t VIA_ORB    = 0x0;
const uint8_t VIA_ORA    = 0x1;
const uint8_t VIA_DDRB   = 0x2;
const uint8_t VIA_DDRA   = 0x3;
const uint8_t VIA_T1CL   = 0x4;
const uint8_t VIA_T1CH   = 0x5;
const uint8_t VIA_T1LL   = 0x6;
const uint8_t VIA_T1LH   = 0x7;
const uint8_t VIA_T2CL   = 0x8;
const uint8_t VIA_T2CH   = 0x9;
const uint8_t VIA_SR     = 0xA;
const uint8_t VIA_ACR    = 0xB;
const uint8_t VIA_PCR    = 0xC;
const uint8_t VIA_IFR    = 0xD;
const uint8_t VIA_IER    = 0xE;
const uint8_t VIA_ORA_NH = 0xF;

//Pins of VIA Port A used for Serial comms (or other misc cartridge use)
const uint8_t VIA_SPI_BIT_CLK  = 0b00000001;
const uint8_t VIA_SPI_BIT_MOSI = 0b00000010;
const uint8_t VIA_SPI_BIT_CS   = 0b00000100;
const uint8_t VIA_SPI_BIT_MISO = 0b10000000;

#define RAM_HIGHBITS_SHIFT 7

#define FULL_RAM_ADDRESS(x) (((system_state.banking & BANK_RAM_MASK) << RAM_HIGHBITS_SHIFT) | (x))

extern unsigned char font_map[];

Timekeeper timekeeper;
Profiler profiler(timekeeper);

SDL_Surface* gRAM_Surface = NULL;
SDL_Surface* vRAM_Surface = NULL;

SDL_Window* mainWindow = NULL;
SDL_Window* buffers_window = NULL;
Uint32 rmask, gmask, bmask, amask;

#ifndef WASM_BUILD
ImGuiContext* main_imgui_ctx;
ImPlotContext* main_implot_ctx;

std::vector<BaseWindow*> toolWindows;
#endif

SDL_Renderer* mainRenderer = NULL;
SDL_Texture* framebufferTexture = NULL;

bool isFullScreen = false;

bool profiler_open = false;
bool buffers_open = false;
int profiler_x_axis = 0;

uint8_t open_bus() {
	return rand() % 256;
}

uint8_t VDMA_Read(uint16_t address) {
	blitter->CatchUp();
	if(system_state.dma_control & DMA_COPY_ENABLE_BIT) {
		return open_bus();
	} else {
		uint8_t* bufPtr;
		uint32_t offset = 0;
		if(system_state.dma_control & DMA_CPU_TO_VRAM) {
			bufPtr = system_state.vram;
			if(system_state.banking & BANK_VRAM_MASK) {
				offset = 0x4000;
			}
		} else {
			bufPtr = system_state.gram;
			offset = (((system_state.banking & BANK_GRAM_MASK) << 2) | (blitter->gram_mid_bits)) << 14;
		}
		return bufPtr[(address & 0x3FFF) | offset];
	}
}

void VDMA_Write(uint16_t address, uint8_t value) {
	blitter->CatchUp();
	if(system_state.dma_control & DMA_COPY_ENABLE_BIT) {
		blitter->SetParam(address, value);
	} else {
		uint8_t* bufPtr;
		uint32_t offset = 0;
		SDL_Surface* targetSurface = NULL;
		uint32_t yShift = 0;
		if(system_state.dma_control & DMA_CPU_TO_VRAM) {
			bufPtr = system_state.vram;
			targetSurface = vRAM_Surface;
			if(system_state.banking & BANK_VRAM_MASK) {
				offset = 0x4000;
				yShift = GT_HEIGHT;
			}
		} else {
			bufPtr = system_state.gram;
			targetSurface = gRAM_Surface;
			yShift = (((system_state.banking & BANK_GRAM_MASK) << 2) | (blitter->gram_mid_bits)) * GT_HEIGHT;
			offset = (((system_state.banking & BANK_GRAM_MASK) << 2) | (blitter->gram_mid_bits)) << 14;
		}
		bufPtr[(address & 0x3FFF) | offset] = value;

		uint8_t x, y;
		x = address & 127;
		y = (address >> 7) & 127;
		put_pixel32(targetSurface, x, y + yShift, Palette::ConvertColor(targetSurface, value));
	}
}

void UpdateFlashShiftRegister(uint8_t nextVal) {
	//TODO: Care about DDR bits
	//For now assuming that if we're using Flash2M hardware we're behaving ourselves
	uint8_t oldVal = system_state.VIA_regs[VIA_ORA];
	uint8_t risingBits = nextVal & ~oldVal;
	if(risingBits & VIA_SPI_BIT_CLK) {
		cartridge_state.bank_shifter = cartridge_state.bank_shifter << 1;
		cartridge_state.bank_shifter &= 0xFE;
		cartridge_state.bank_shifter |= !!(oldVal & VIA_SPI_BIT_MOSI);
	} else if(risingBits & VIA_SPI_BIT_CS) {
		//flash cart CS is connected to latch clock
		if((cartridge_state.bank_mask ^ cartridge_state.bank_shifter) & 0x80) {
			SaveNVRAM();
		}
		cartridge_state.bank_mask = cartridge_state.bank_shifter;
		if(loadedRomType != RomType::FLASH2M_RAM32K) {
			cartridge_state.bank_mask |= 0x80;
		}
		//printf("Flash highbits set to %x\n", cartridge_state.bank_mask);
	}
}

uint8_t MemoryRead_Flash2M(uint16_t address) {
	if(address & 0x4000) {
		return cartridge_state.rom[0b111111100000000000000 | (address & 0x3FFF)];
	} else {
		if(!(cartridge_state.bank_mask & 0x80))
			return cartridge_state.save_ram[(address & 0x3FFF) | ((cartridge_state.bank_mask & 0x40) << 8)];
		else return cartridge_state.rom[((cartridge_state.bank_mask & 0x7F) << 14) | (address & 0x3FFF)];
	}
}

uint8_t MemoryRead_Unknown(uint16_t address) {
	//If cartridge_state.size is smaller than unbanked ROM range, align end with 0xFFFF and wrap
	//If cartridge_state.size is bigger than unbanked ROM range, access mainWindow at end of file.
	//TODO: Decide if unknown ROM type should just terminate emulator :P
	if(cartridge_state.size <= 32768) {
		return cartridge_state.rom[((address & 0x7FFF) + 32768 - cartridge_state.size) % cartridge_state.size];
	} else {
		return cartridge_state.rom[((address & 0x7FFF) + cartridge_state.size - 32768)];
	}
}

uint8_t* GetRAM(const uint16_t address) {
	return &(system_state.ram[FULL_RAM_ADDRESS(address & 0x1FFF)]);
}

uint8_t MemoryReadResolve(const uint16_t address, bool stateful) {
	if (address == 0x2007) { //unused/write-only address
		uint8_t status = 0x55; // Base "Emulator" ID
        if (paddleDetected) {//because now we can override paddle with mouse if we want
            // Tell the game: "This is a physical dial, not mouse, remap rotation"
            status |= 0x02; //01010101
        } 
		if (paddle_emulation_enabled){
			status |= 0x08; //
		}
		if (dksPaddleDetected) {
			status |= 0x20; //32 specifically my paddle, used for unlocking demo
		}
        return status;
	} else if(address & 0x8000) {
		switch(loadedRomType) {
			case RomType::EEPROM8K:
			return cartridge_state.rom[address & 0x1FFF];
			case RomType::EEPROM32K:
			return cartridge_state.rom[address & 0x7FFF];
			case RomType::FLASH2M:
			case RomType::FLASH2M_RAM32K:
			return MemoryRead_Flash2M(address);
			case RomType::UNKNOWN:
			return MemoryRead_Unknown(address);
		}
	} else if(address & 0x4000) {
		return VDMA_Read(address);
	} else if((address >= 0x3000) && (address <= 0x3FFF)) {
		return soundcard->ram_read(address);
	} else if((address >= 0x2800) && (address <= 0x2FFF)) {
		return system_state.VIA_regs[address & 0xF];
	} else if(address < 0x2000) {
		if(stateful) {
			if(!system_state.ram_initialized[FULL_RAM_ADDRESS(address & 0x1FFF)]) {
				//printf("WARNING! Uninitialized RAM read at %x (Bank %x)\n", address, system_state.banking >> 5);
			}
		}
		return *GetRAM(address);
	} else if((address == 0x2008) || (address == 0x2009)) {
		return joysticks->read((uint8_t) address, stateful);
	}
	if(stateful) {
		printf("Attempted to read write-only device, may be unintended? %x\n", address);
	}
	return open_bus();
}

uint8_t MemoryRead(uint16_t address) {
	return MemoryReadResolve(address, true);
}

uint8_t MemorySync(uint16_t address) {
	if(timekeeper.clock_mode == CLOCKMODE_NORMAL) {
		if(Breakpoints::checkBreakpoint(address, cartridge_state.bank_mask)) {
			timekeeper.clock_mode = CLOCKMODE_STOPPED;
			Disassembler::Decode(MemoryReadResolve, loadedMemoryMap, address, 32);
			cpu_core->Freeze();
		}
		uint8_t opcode = MemoryReadResolve(address, false);
		if(opcode == 0x20) { //JSR
			uint16_t jsr_dest = MemoryReadResolve(address+1, false) | (MemoryReadResolve(address+2, false) << 8);
			profiler.LogJSR(address, cartridge_state.bank_mask, jsr_dest);
		} else if(opcode == 0x60) { //RTS
			profiler.LogRTS(address, cartridge_state.bank_mask);
		}
	}
	return MemoryRead(address);
}

void MemoryWrite(uint16_t address, uint8_t value) {
	// Catch the game trying to "write" to the Gamepad 2 port
    if (address == 0x2009) {
#ifndef WASM_BUILD
        if (value == SIGNAL_PADDLE_MODE) {
            //paddle_emulation_enabled = true;//user set only
			romRequestedPaddle = true;
        } else if (value == 0x00) {
            //paddle_emulation_enabled = false;//user set only
			romRequestedPaddle = false;
        }
#endif
        return; // Absorb the write cycle
    }
	else if(address & 0x8000) {
		if(loadedRomType == RomType::FLASH2M_RAM32K) {
			if(!(address & 0x4000)) {
				if(!(cartridge_state.bank_mask & 0x80)) {
					cartridge_state.save_ram[(address & 0x3FFF) | ((cartridge_state.bank_mask & 0x40) << 8)] = value;
				}
			}
		}
		if(loadedRomType == RomType::FLASH2M) {
			if(cartridge_state.write_mode) {
				uint8_t* location;
				if(address & 0x4000) {
					location = &(cartridge_state.rom[0b111111100000000000000 | (address & 0x3FFF)]);
				} else {
					location = &(cartridge_state.rom[((cartridge_state.bank_mask & 0x7F) << 14) | (address & 0x3FFF)]);
				}
				*location &= value;
				cartridge_state.write_mode = false;
			} else {
				//Skipping over details like bypass and unlock commands for now
				//So off-spec flash operation will be inaccurate
				if(value == 0x10) {
					//Chip Erase
					for(int i = 0; i < (1 << 21); ++i) {
						cartridge_state.rom[i] = 0xFF;
					}
				} else if (value == 0x30) {
					//Sector erase
					uint8_t sectorBits = ((address & (1 << 13)) >> 13) | ((cartridge_state.bank_mask & 0x7F) << 1);
					uint8_t sectorNum = sectorBits >> 3;
					if(sectorNum < 31) {
						//most of the sector table
						uint32_t x = sectorNum << 16;
						for(uint32_t i = 0; i < (1 << 16); ++i) {
							cartridge_state.rom[x] = 0xFF;
							++x;
						}
					} else if((sectorBits & 4) == 0) {
						uint32_t x = 0x1F0000;
						for(uint32_t i = 0; i < (1 << 15); ++i) {
							cartridge_state.rom[x] = 0xFF;
							++x;
						}
					} else if(sectorBits == 0b11111100) {
						uint32_t x = 0x1F8000;
						for(uint32_t i = 0; i < (1 << 13); ++i) {
							cartridge_state.rom[x] = 0xFF;
							++x;
						}
					} else if(sectorBits == 0b11111101) {
						uint32_t x = 0x1FA000;
						for(uint32_t i = 0; i < (1 << 13); ++i) {
							cartridge_state.rom[x] = 0xFF;
							++x;
						}
					} else if((sectorBits >> 1) == 0b1111111) {
						uint32_t x = 0x1FC000;
						for(uint32_t i = 0; i < (1 << 14); ++i) {
							cartridge_state.rom[x] = 0xFF;
							++x;
						}
					}
				} else if(value == 0xA0) {
					cartridge_state.write_mode = true;
				} else if(value == 0x90) {
					//first byte of lock command should be a good time to write to file
#ifdef WASM_BUILD
					SaveModifiedFlash();
#else
					if(savingThread.joinable()) {
						savingThread.join();
					}
					savingThread = std::thread(SaveModifiedFlash);
#endif
				}
			}
		}
	}
	else if(address & 0x4000) {
		VDMA_Write(address, value);
	} else if(address >= 0x3000 && address <= 0x3FFF) {
		soundcard->ram_write(address, value);
	} else if((address & 0x2000)) {
		if(address & 0x800) {
			if(loadedRomType == RomType::FLASH2M) {
				if((address & 0xF) == VIA_ORA) {
					UpdateFlashShiftRegister(value);
				}
			}
			if((address & 0xF) == VIA_ORB) {
				if((system_state.VIA_regs[VIA_ORB] & 0x80) && !(value & 0x80)) {
					//falling edge of high bit of ORB
					if(value & 0x40) {
						//report duration
						profiler.LogTime(value & 0x3F);
					} else {
						//store timestamp
						profiler.profilingTimeStamps[value & 0x3F] = timekeeper.totalCyclesCount;
					}
				}
			}
			system_state.VIA_regs[address & 0xF] = value;
		} else {
			if((address & 0x000F) == 0x0007) {
				blitter->CatchUp();
				if((value & DMA_VID_OUT_PAGE_BIT) != (system_state.dma_control & DMA_VID_OUT_PAGE_BIT)) {
					profiler.bufferFlipCount++;
					if(profiler.measure_by_frameflip) {
						profiler.ResetTimers();
						profiler.last_blitter_activity = blitter->pixels_this_frame;
						blitter->pixels_this_frame = 0;
					}
				}
				system_state.dma_control = value;
				system_state.dma_control_irq = (system_state.dma_control & DMA_COPY_IRQ_BIT) != 0;
				if(system_state.dma_control & DMA_TRANSPARENCY_BIT) {
					SDL_SetColorKey(gRAM_Surface, SDL_TRUE, SDL_MapRGB(gRAM_Surface->format, 0, 0, 0));
				} else {
					SDL_SetColorKey(gRAM_Surface, SDL_FALSE, 0);
				}
			} else if((address & 0x000F) == 0x0005) {
				blitter->CatchUp();
				system_state.banking = value;
				//printf("banking reg set to %x\n", value);
			} else {
				soundcard->register_write(address, value);
			}
		}
	}
	else if(address < 0x2000) {
		/*if(!system_state.ram_initialized[FULL_RAM_ADDRESS(address & 0x1FFF)]) {
			printf("First RAM write at %x (Bank %x) (Value %x)\n", address, system_state.banking >> 6, value);
		}*/
		system_state.ram_initialized[FULL_RAM_ADDRESS(address & 0x1FFF)] = true;
		system_state.ram[FULL_RAM_ADDRESS(address & 0x1FFF)] = value;
	}
}

SDL_Event e;
bool running = true;
bool gofast = false;
bool paused = true;
bool lshift = false;
bool rshift = false;

void randomize_vram() {
	for(int i = 0; i < VRAM_BUFFER_SIZE; i ++) {
		system_state.vram[i] = rand() % 256;
		put_pixel32(vRAM_Surface, i & 127, i >> 7, Palette::ConvertColor(vRAM_Surface, system_state.vram[i]));
	}
	for(int i = 0; i < GRAM_BUFFER_SIZE; i ++) {
		system_state.gram[i] = rand() % 256;
		put_pixel32(gRAM_Surface, i & 127, i >> 7, Palette::ConvertColor(gRAM_Surface, system_state.gram[i]));
	}
}

void randomize_memory() {
	for(int i = 0; i < RAMSIZE; i++) {
		system_state.ram[i] = rand() % 256;
		system_state.ram_initialized[i] = false;
	}

	for(int i = 0; i < VRAM_BUFFER_SIZE; i++) {
		system_state.vram[i] = rand() % 256;	
	}

	for(int i = 0; i < GRAM_BUFFER_SIZE; i++) {
		system_state.gram[i] = rand() % 256;	
	}
	
	system_state.dma_control = rand() % 256;
	system_state.dma_control_irq = (system_state.dma_control & DMA_COPY_IRQ_BIT) != 0;
	system_state.banking = rand() % 256;
	blitter->gram_mid_bits = rand() % 4;
}

extern "C" {
void PauseEmulation() {
  paused = true;

  AudioCoprocessor::singleton_acp_state->isEmulationPaused = true;
}

void ResumeEmulation() {
  paused = false;

  AudioCoprocessor::singleton_acp_state->isEmulationPaused = false;
}
}

void CPUStopped() {
	PauseEmulation();
	printf("CPU stopped");
#ifdef TINYFILEDIALOGS_H
	tinyfd_notifyPopup("Alert",
		"CPU has stopped either due to STP opcode",
		"info");
#endif
}

const char * open_rom_dialog() {
	char const * lFilterPatterns[1] = {"*.gtr"};
#ifdef TINYFILEDIALOGS_H
	return tinyfd_openFileDialog(
		"Select a GameTank ROM file",
		"",
		1,
		lFilterPatterns,
		"GameTank Rom",
		0);
#else
	return EMBED_ROM_FILE;
#endif
}

extern "C" {
	// Attempts to load a rom by filename into a buffer
	// 0 on success
	// -1 on failure (e.g. file by name doesn't exist)
	int LoadRomFile(const char* filename) {
		std::filesystem::path filepath(filename);
		currentRomFilePath = filepath.string();
#ifdef WASM_BUILD
		std::filesystem::path nvramPath("/idbfs");
		nvramPath /= std::filesystem::path(currentRomFilePath).filename();
#else
		std::filesystem::path nvramPath(filename);
#endif
		nvramPath.replace_extension("sav");
		nvramFileFullPath = nvramPath.string();
		if (EmulatorConfig::xorFile != NULL) {
		  flashFileFullPath = std::string(EmulatorConfig::xorFile);
		} else {
		    nvramPath.replace_extension("xor");
		    flashFileFullPath = nvramPath.string();
		}
		nvramPath.replace_extension("gtrcfg");

		gameconfig = new GameConfig(nvramPath.string().c_str());

		std::filesystem::path defaultMemMapFilePath = filepath.parent_path().append("../build/out.map");
		std::filesystem::path defaultSourceMapFilePath = filepath.parent_path().append("../build/sourcemap.dbg");

		if(std::filesystem::exists(defaultMemMapFilePath)) {
			printf("found default memory map file location %s\n", defaultMemMapFilePath.c_str());
			loadedMemoryMap = new MemoryMap(defaultMemMapFilePath.string());
			Breakpoints::linkBreakpoints(*loadedMemoryMap);
		} else {
			loadedMemoryMap = new MemoryMap();
			printf("default memory map file %s not found\n", defaultMemMapFilePath.c_str());
		}

		if(std::filesystem::exists(defaultSourceMapFilePath)) {
			printf("found default source map file location %s\n", defaultSourceMapFilePath.c_str());
			std::string sourceMapPathString = defaultSourceMapFilePath.string();
			SourceMap::singleton = new SourceMap(sourceMapPathString);
		} else {
			printf("default source map file %s not found\n", defaultSourceMapFilePath.c_str());
		}

		printf("loading %s\n", filename);
		FILE* romFileP = fopen(filename, "rb");
		if(!romFileP) {
			printf("Unable to open file: %s\n", filename);
			return -1;
		}

		fseek(romFileP, 0L, SEEK_END);
		cartridge_state.size = ftell(romFileP);
		//cartridge_state.rom = new uint8_t [cartridge_state.size];
		cartridge_state.write_mode = false;
		rewind(romFileP);
		switch(cartridge_state.size) {
			case 8192:
			loadedRomType = RomType::EEPROM8K;
			printf("Detected 8K (EEPROM)\n");
			break;
			case 32768:
			loadedRomType = RomType::EEPROM32K;
			printf("Detected 32K (EEPROM)\n");
			break;
			case 2097152:
			loadedRomType = RomType::FLASH2M;
			printf("Detected 2M (Flash)\n");
			break;
			default:
			loadedRomType = RomType::UNKNOWN;
			printf("Unknown ROM type: Size is %d bytes\n", cartridge_state.size);
			break;
		}
		fread(cartridge_state.rom, sizeof(uint8_t), cartridge_state.size, romFileP);
		fclose(romFileP);
		if(cpu_core) {
			ResumeEmulation();
			cpu_core->Reset();
			cartridge_state.write_mode = false;
		}

		if(loadedRomType == RomType::FLASH2M) {

			if(std::filesystem::exists(flashFileFullPath.c_str())) {
				std::cout << "Loading flash save from " << flashFileFullPath << "\n";
				LoadModifiedFlash();
			} else {
				std::cout << "Couldn't find " << flashFileFullPath << "\n";
			}

			if(
				(cartridge_state.rom[0x1FFFF0] == 'S') &&
				(cartridge_state.rom[0x1FFFF1] == 'A') &&
				(cartridge_state.rom[0x1FFFF2] == 'V') &&
				(cartridge_state.rom[0x1FFFF3] == 'E')) {
					loadedRomType = RomType::FLASH2M_RAM32K;
					if(std::filesystem::exists(nvramFileFullPath.c_str())) {
						LoadNVRAM();
					}
				}
		}
		return 0;
	}

	void SetButtons(int buttonMask) {
		if(joysticks != NULL) {
			joysticks->SetHeldButtons(buttonMask);
		}
	}

	void takeScreenShot() {
		SDL_Surface *screenshot = SDL_CreateRGBSurface(0, SCREEN_WIDTH, SCREEN_HEIGHT, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
		SDL_RenderReadPixels(mainRenderer, NULL, SDL_PIXELFORMAT_ARGB8888, screenshot->pixels, screenshot->pitch);
		SDL_SaveBMP(screenshot, "screenshot.bmp");
		SDL_FreeSurface(screenshot);
	}

#ifdef WASM_BUILD
	extern "C" {
		EMSCRIPTEN_KEEPALIVE
		
		void SetPaddleMode(bool enabled) {
			paddle_emulation_enabled = enabled;
			if (paddle_emulation_enabled){
				if (paddle_touch_mode) {
					SDL_SetRelativeMouseMode(SDL_FALSE);
				}
				else
				{
					SDL_SetRelativeMouseMode(SDL_TRUE);
				}
			}
		}
		void SetPaddleTouchMode(bool enabled) {
			paddle_touch_mode = enabled;
			// If we switch to touch, we must ensure relative mode is off
			if (paddle_touch_mode) {
				SDL_SetRelativeMouseMode(SDL_FALSE);
			}
			else
			{
				SDL_SetRelativeMouseMode(SDL_TRUE);
			}
		}
		void EMSCRIPTEN_KEEPALIVE SetPaddleValue(int val) {
        // Map the 0-255 value directly to the joystick bits
        // instead of relying on the mouse-coordinate math
			if (joysticks != nullptr) {
				joysticks->SetPaddleBitsDirect(val); 
			}
    	}

		void EMSCRIPTEN_KEEPALIVE UpdatePaddleFromMouseJS(int index, int dx) {
			// Calls your existing logic that adds dx to the current paddle position
			joysticks->UpdatePaddleFromMouse(0, dx);
		}

	}
	#endif
}
#ifndef WASM_BUILD
template <typename T>
void closeToolByType() {
    toolWindows.erase(
        std::remove_if(
            toolWindows.begin(),
            toolWindows.end(),
            [](BaseWindow* window) {
                if(dynamic_cast<T*>(window) != nullptr) {
					delete window;
					return true;
				}
				return false;
            }
        ),
        toolWindows.end()
    );
}

template <typename T>
bool toolTypeIsOpen() {
    for (const auto& window : toolWindows) {
        if (dynamic_cast<T*>(window) != nullptr) {
            return true;
        }
    }
    return false;
}

void toggleProfilerWindow() {
	if(!toolTypeIsOpen<ProfilerWindow>()) {
		toolWindows.push_back(new ProfilerWindow(profiler));
	} else {
		closeToolByType<ProfilerWindow>();
	}
}

void toggleMemBrowserWindow() {
	if(!toolTypeIsOpen<MemBrowserWindow>()) {
		toolWindows.push_back(new MemBrowserWindow(loadedMemoryMap, MemoryReadResolve, GetRAM, *gameconfig));
	} else {
		closeToolByType<MemBrowserWindow>();
	}
}

void toggleVRAMWindow() {
	if(!toolTypeIsOpen<VRAMWindow>()) {
		toolWindows.push_back(new VRAMWindow(vRAM_Surface, gRAM_Surface,
			&system_state, cpu_core, &cartridge_state));
	} else {
		closeToolByType<VRAMWindow>();
	}
}

void toggleSteppingWindow() {
	if(!toolTypeIsOpen<SteppingWindow>()) {
		toolWindows.push_back(new SteppingWindow(timekeeper, loadedMemoryMap, cpu_core, *gameconfig, cartridge_state));
	} else {
		closeToolByType<SteppingWindow>();
	}
}

void togglePatchingWindow() {
	if(!toolTypeIsOpen<PatchingWindow>()) {
		toolWindows.push_back(new PatchingWindow(loadedMemoryMap, gameconfig));
	} else {
		closeToolByType<PatchingWindow>();
	}
}

void doRamDump() {
	soundcard->dump_ram("audio_debug.dat");
	ofstream dumpfile ("ram_debug.dat", ios::out | ios::binary);
	dumpfile.write((char*) system_state.ram, RAMSIZE);
	dumpfile.close();
}

void toggleControllerOptionsWindow() {
	if(!toolTypeIsOpen<ControllerOptionsWindow>()) {
		toolWindows.push_back(new ControllerOptionsWindow(joysticks));
	} else {
		closeToolByType<ControllerOptionsWindow>();
	}
}

#endif

void toggleFullScreen() {
	if(isFullScreen) {
		SDL_SetWindowFullscreen(mainWindow, 0);
		isFullScreen = false;
	} else {
		SDL_SetWindowFullscreen(mainWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
		isFullScreen = true;
	}
	timekeeper.scaling_increment = INITIAL_SCALING_INCREMENT;
}

void toggleMute() {
	muteMask = muteMask ^ MUTE_SOURCE_MANUAL;
	AudioCoprocessor::singleton_acp_state->isMuted = (muteMask != 0);
}

void setMenuMute(bool muted) {
	muteMask &= ~MUTE_SOURCE_MENU;
	if(muted) {
		muteMask |= MUTE_SOURCE_MENU;
	}
	AudioCoprocessor::singleton_acp_state->isMuted = (muteMask != 0);
}

typedef struct HotkeyAssignment {
	void (*func)();
	SDL_Keycode  key;
} HotkeyAssignment;

HotkeyAssignment hotkeys[] = {
	{&toggleFullScreen, SDLK_F11},
	{&toggleMute, SDLK_m},
#if !defined(WASM_BUILD) && !defined(WRAPPER_MODE)
	{&doRamDump, SDLK_F6},
	{&toggleSteppingWindow, SDLK_F7},
	{&takeScreenShot, SDLK_F8},
	{&toggleMemBrowserWindow, SDLK_F9},
	{&toggleVRAMWindow, SDLK_F10},
	{&toggleProfilerWindow, SDLK_F12},
#endif
};

bool checkHotkey(SDL_Keycode  key) {
	for(HotkeyAssignment assignment : hotkeys) {
		if(assignment.key == key) {
			assignment.func();
			return true;
		}
	}
	return false;
}

#ifndef EM_BOOL
#define EM_BOOL int
#endif


void UpdateNTSCTexture() {
    const float SCALE_X = ntsc_res_scale; // Upscale factor (2x horizontal resolution)
    const int NTSC_WIDTH = (int)(GT_WIDTH * SCALE_X);
    const int NTSC_HEIGHT_TOTAL = GT_HEIGHT * 2; // Double buffered height stays intact

    static std::vector<uint32_t> ntsc_framebuffer;
    if (ntsc_framebuffer.size() != (size_t)(NTSC_WIDTH * NTSC_HEIGHT_TOTAL)) {
        ntsc_framebuffer.resize(NTSC_WIDTH * NTSC_HEIGHT_TOTAL, 0);
    }
    int current_y_offset = (system_state.dma_control & DMA_VID_OUT_PAGE_BIT) ? GT_HEIGHT : 0;
    uint32_t* src_pixels = (uint32_t*)vRAM_Surface->pixels;
    int pitch_pixels = vRAM_Surface->pitch / 4;

    static float frame_phase_offset = 0.0f;
    frame_phase_offset = fmod(frame_phase_offset + 4.0f, 12.0f); 
	
	std::vector<float> composite_signal(NTSC_WIDTH * 4); // 4 samples per pixel max

    for (int y = 0; y < GT_HEIGHT; ++y) {
        int actual_y = current_y_offset + y;
        float scanline_phase = fmod(frame_phase_offset + (y * 4.0f), 12.0f); 

        const int SAMPLES_PER_PIXEL = 4; // 256 * 4 keeps the internal subcarrier frequency consistent
        const int TOTAL_SAMPLES = NTSC_WIDTH * SAMPLES_PER_PIXEL;
        //static std::vector<float> composite_signal(TOTAL_SAMPLES);

		float carryover_r = 0.0f;
    	float carryover_g = 0.0f;
    	float carryover_b = 0.0f;

        // ENCODE WITH LINEAR INTERPOLATION
        for (int x = 0; x < NTSC_WIDTH; ++x) {
            float src_x = x / (float)SCALE_X;
            int x0 = (int)floor(src_x);
            int x1 = std::min(x0 + 1, GT_WIDTH - 1);
            float t = src_x - x0;

            uint32_t p0 = src_pixels[actual_y * pitch_pixels + x0];
            uint32_t p1 = src_pixels[actual_y * pitch_pixels + x1];

            float r = (((p0 >> 16) & 0xFF) * (1.0f - t) + ((p1 >> 16) & 0xFF) * t) / 255.0f;
            float g = (((p0 >> 8) & 0xFF) * (1.0f - t) + ((p1 >> 8) & 0xFF) * t) / 255.0f;
            float b = ((p0 & 0xFF) * (1.0f - t) + (p1 & 0xFF) * t) / 255.0f;

            float Y_val = 0.299f * r + 0.587f * g + 0.114f * b;
            float U_val = (-0.299f * r - 0.587f * g + 0.886f * b) * 0.492111f;
            float V_val = (0.701f * r - 0.587f * g - 0.114f * b) * 0.877283f;

            for (int p = 0; p < SAMPLES_PER_PIXEL; ++p) {
                int sample_idx = x * SAMPLES_PER_PIXEL + p;
                float angle = M_PI * (scanline_phase + sample_idx) / 6.0f; 
                composite_signal[sample_idx] = Y_val + U_val * sin(angle) + V_val * cos(angle);
            }
        }

        // FILTER 
        float v_prev = composite_signal[0];
        float dt = 1.0f / (236.25e6f / 11.0f * 2.0f); 
        float amount = 3.5f; 
        float composite_white = 1.200f;

        for (int i = 0; i < TOTAL_SAMPLES; ++i) {
            float voltage_ratio = composite_signal[i] / composite_white;
            float alpha = dt / (voltage_ratio * amount * 1e-8f + dt);
            v_prev = alpha * composite_signal[i] + (1.0f - alpha) * v_prev;
            composite_signal[i] = v_prev;
        }

        // DECODE
        for (int x = 0; x < NTSC_WIDTH; ++x) {
            int center = x * SAMPLES_PER_PIXEL;
            int begin = center - 6;
            if (begin < 0) begin = 0;
            int end = center + 6;
            if (end > TOTAL_SAMPLES) end = TOTAL_SAMPLES;

            float out_y = 0.0f, out_u = 0.0f, out_v = 0.0f;
            float sample_scale = 1.0f / (end - begin);

            for (int p = begin; p < end; ++p) {
                float level = composite_signal[p] * sample_scale;
                out_y += level;
                out_u += level * sin(M_PI * (scanline_phase + p) / 6.0f) * 2.0f;
                out_v += level * cos(M_PI * (scanline_phase + p) / 6.0f) * 2.0f;
            }

            out_u *= 1.4f; 
            out_v *= 1.4f;

            // float r_out = out_y + 1.139883f * out_v;
            // float g_out = out_y - 0.394642f * out_u - 0.580622f * out_v;
            // float b_out = out_y + 2.032062f * out_u;

            // int R_int = std::max(0, std::min(255, (int)(r_out * 255.0f)));
            // int G_int = std::max(0, std::min(255, (int)(g_out * 255.0f)));
            // int B_int = std::max(0, std::min(255, (int)(b_out * 255.0f)));

			// --- SURGICAL LAYER BLEND ---
            // Extract the raw sharp pixel first
            int sharp_x = x / SCALE_X;
            uint32_t sharp_pixel = src_pixels[actual_y * pitch_pixels + sharp_x];
            int sharp_r = (sharp_pixel >> 16) & 0xFF;
            int sharp_g = (sharp_pixel >> 8) & 0xFF;
            int sharp_b = sharp_pixel & 0xFF;

			// Isolate the pure chroma vectors (omitting out_y)
            float r_chroma = 1.139883f * out_v;
            float g_chroma = -0.394642f * out_u - 0.580622f * out_v;
            float b_chroma = 2.032062f * out_u;

            // Amplify or attenuate the color shifting
            // Change 1.5f to whatever intensity factor feels right
            //const float color_shift_intensity = 1.5f; 
            r_chroma *= ntsc_color_shift;//color_shift_intensity;
            g_chroma *= ntsc_color_shift;//color_shift_intensity;
            b_chroma *= ntsc_color_shift;//color_shift_intensity;
			int base_mix_r, base_mix_g, base_mix_b;

		if (ntsc_bloom_enabled){
			// Add the base sharp pixel, the chroma artifact, AND the carryover energy from the previous pixel
			int raw_r = sharp_r + (int)(r_chroma * 255.0f) + (int)carryover_r;
			int raw_g = sharp_g + (int)(g_chroma * 255.0f) + (int)carryover_g;
			int raw_b = sharp_b + (int)(b_chroma * 255.0f) + (int)carryover_b;

			// Calculate the surplus energy that exceeds the standard integer ceiling
			int surplus_r = std::max(0, raw_r - 255);
			int surplus_g = std::max(0, raw_g - 255);
			int surplus_b = std::max(0, raw_b - 255);

			// Decay the surplus to carry it into the next pixel (e.g., 75% persistence)
			//    Higher values create a wider, more severe horizontal smear behind bright elements
			//const float bloom_decay = 0.75f;
			carryover_r = surplus_r * ntsc_bloom_decay;//bloom_decay;
			carryover_g = surplus_g * ntsc_bloom_decay;//bloom_decay;
			carryover_b = surplus_b * ntsc_bloom_decay;//bloom_decay;

			// Apply the final hard clamp for the current pixel compilation
			base_mix_r = std::min(255, raw_r);
			base_mix_g = std::min(255, raw_g);
			base_mix_b = std::min(255, raw_b);

			// Ensure values don't dip below zero due to negative chroma modulation
			if (base_mix_r < 0) base_mix_r = 0;
			if (base_mix_g < 0) base_mix_g = 0;
			if (base_mix_b < 0) base_mix_b = 0;
		}
		else
		{
			// Inject the isolated color fringe straight onto the sharp baseline
            base_mix_r = sharp_r + (int)(r_chroma * 255.0f);
            base_mix_g = sharp_g + (int)(g_chroma * 255.0f);
            base_mix_b = sharp_b + (int)(b_chroma * 255.0f);

            // Clamp to safeguard against integer wrapping
            base_mix_r = std::max(0, std::min(255, base_mix_r));
            base_mix_g = std::max(0, std::min(255, base_mix_g));
            base_mix_b = std::max(0, std::min(255, base_mix_b));

		}
			
            uint32_t final_pixel;
            if (!phosphor_blending_enabled)
            {
                final_pixel = (0xFF000000) | (base_mix_r << 16) | (base_mix_g << 8) | base_mix_b;
            }
            else
            {
                // Simulate phosphor blending to reduce temporal shimmering on top of the base layer mix
                uint32_t old_pixel = ntsc_framebuffer[actual_y * NTSC_WIDTH + x];

                int old_r = (old_pixel >> 16) & 0xFF;
                int old_g = (old_pixel >> 8) & 0xFF;
                int old_b = old_pixel & 0xFF;

                int blended_r = ((base_mix_r * 3) + old_r) >> 2;
                int blended_g = ((base_mix_g * 3) + old_g) >> 2;
                int blended_b = ((base_mix_b * 3) + old_b) >> 2;

                final_pixel = (0xFF000000) | (blended_r << 16) | (blended_g << 8) | blended_b;
            }
            ntsc_framebuffer[actual_y * NTSC_WIDTH + x] = final_pixel;
        }
    }

    SDL_UpdateTexture(framebufferTexture, NULL, ntsc_framebuffer.data(), NTSC_WIDTH * 4);
}

void refreshScreen() {
    SDL_Rect src, dest;
    int scr_w, scr_h;
    SDL_GetWindowSize(mainWindow, &scr_w, &scr_h);

	// Determine target dimensions based on the NTSC toggle state
	int target_tex_w = ntsc_filter_enabled ? (int)(GT_WIDTH * ntsc_res_scale) : GT_WIDTH;
    int target_tex_h = GT_HEIGHT * 2;

    // Validate and adjust the texture allocation size dynamically
    int current_tex_w = 0, current_tex_h = 0;
    if (framebufferTexture) {
        SDL_QueryTexture(framebufferTexture, NULL, NULL, &current_tex_w, &current_tex_h);
    }
    if (current_tex_w != target_tex_w) {
        if (framebufferTexture) SDL_DestroyTexture(framebufferTexture);
        
        framebufferTexture = SDL_CreateTexture(mainRenderer, SDL_PIXELFORMAT_ARGB8888, 
                                              SDL_TEXTUREACCESS_STREAMING, target_tex_w, target_tex_h);
    }

#ifdef WRAPPER_MODE
    // number of native overscan rows to strip from the top and bottom
    const int BORDER_TOP = 8; 
    const int BORDER_BOTTOM = 8; 

    src.x = 0;
    src.y = ((system_state.dma_control & DMA_VID_OUT_PAGE_BIT) ? GT_HEIGHT : 0) + BORDER_TOP;
	if (ntsc_filter_enabled){
		src.w = (int)(GT_WIDTH * ntsc_res_scale);
	} else {
		src.w = GT_WIDTH;
	}
    src.h = GT_HEIGHT - (BORDER_TOP + BORDER_BOTTOM); 
    
    dest.h = scr_h; 
    // Fix: Calculate aspect ratio using structural GT_WIDTH (128) instead of src.w (256)
    dest.w = (int)(dest.h * ((double)GT_WIDTH / src.h));
#else
    src.x = 0;
    src.y = (system_state.dma_control & DMA_VID_OUT_PAGE_BIT) ? GT_HEIGHT : 0;
	if (ntsc_filter_enabled){
		src.w = (int)(GT_WIDTH * ntsc_res_scale);
	} else {
		src.w = GT_WIDTH;
	}
    src.h = GT_HEIGHT;

    dest.w = min(scr_w, scr_h);
    dest.h = dest.w;
#endif

    dest.x = (scr_w - dest.w) / 2; 
    dest.y = 0;

    int main_frame_w = dest.w;
    
	if (ntsc_filter_enabled){
    	UpdateNTSCTexture();
	} else {
		SDL_UpdateTexture(framebufferTexture, NULL, vRAM_Surface->pixels, vRAM_Surface->pitch);
	}

    SDL_RenderClear(mainRenderer);

    SDL_RenderCopy(mainRenderer, framebufferTexture, &src, &dest);

    // // Fix: Read from the edge of the new upscaled bounds (255 instead of 127)
    // src.x = (GT_WIDTH * ntsc_res_scale) - 1;
    // src.w = 1;
    // dest.w = (int)(main_frame_w * 86.0 / 512.0);
    
    // // left overscan bar
    // dest.x -= dest.w;
    // SDL_RenderCopy(mainRenderer, framebufferTexture, &src, &dest);

    // // right overscan bar
    // dest.x += dest.w + main_frame_w;
    // SDL_RenderCopy(mainRenderer, framebufferTexture, &src, &dest);

	// Set draw color to opaque black for the side bars
    SDL_SetRenderDrawColor(mainRenderer, 0, 0, 0, 255);

    // Calculate width of the overscan bar area
    dest.w = (int)(main_frame_w * 86.0 / 512.0);
    
    // Left overscan bar
    dest.x -= dest.w;
    SDL_RenderFillRect(mainRenderer, &dest);

    // Right overscan bar
    dest.x += dest.w + main_frame_w;
    SDL_RenderFillRect(mainRenderer, &dest);

#if !defined(WASM_BUILD)
	ImGui::SetCurrentContext(main_imgui_ctx);
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();
	if(showMenu) {
#ifndef WRAPPER_MODE
		if(ImGui::BeginMainMenuBar()) {
			if (ImGui::BeginMenu("File")) {
				if(ImGui::MenuItem("Open Rom")) {
					const char* rom_file_name = open_rom_dialog();
					if(rom_file_name) {
						LoadRomFile(rom_file_name);
					}	
				}
				if(ImGui::MenuItem("Exit")) {
					running = false;
				}
				ImGui::EndMenu();
			}
			
			if(ImGui::BeginMenu("Settings")) {
				if(ImGui::MenuItem("Controllers")) {
					toggleControllerOptionsWindow();
				}
				ImGui::MenuItem("Toggle Instant Blits", NULL, &(blitter->instant_mode));
				ImGui::SliderInt("Volume", &AudioCoprocessor::singleton_acp_state->volume, 0, 256);
				ImGui::Checkbox("Mute", &AudioCoprocessor::singleton_acp_state->isMuted);
				if (ImGui::Checkbox("Mouse Paddle", &paddle_emulation_enabled)) {
					SavePreferences();
					joysticks->SetHeldButtons(0);//clear bits on change just in case
				}
				if (ImGui::Checkbox("NTSC Filter", &ntsc_filter_enabled)) {
					SavePreferences();
				}
				if (ImGui::SliderFloat("NTSC Color Shift",&ntsc_color_shift,0.05f,2.0f, "%.2f")){
					SavePreferences();
				}
				if (ImGui::SliderFloat("NTSC Resolution Scale",&ntsc_res_scale,1.0f,4.0f, "%.1f")){
					ntsc_res_scale = (ntsc_res_scale <= 4) ? (ntsc_res_scale = (ntsc_res_scale > 0) ? ntsc_res_scale : 1.0f) : 4.0f;
					SavePreferences();
				}
				if (ImGui::Checkbox("NTSC Bloom", &ntsc_bloom_enabled)){
					SavePreferences();
				}
				if (ImGui::SliderFloat("NTSC Bloom Decay",&ntsc_bloom_decay,0.05f,0.95f, "%.2f")){
				SavePreferences();
				}
				if (ImGui::Checkbox("Enable Phosphor Blending", &phosphor_blending_enabled)) {
					SavePreferences();
				}
				// if (ImGui::Checkbox("Use Any Joystick As Paddle", &use_any_joystick_as_paddle)){
				// 	SavePreferences();
				// 	paddleDetected = false;
				// 	PaddleInit();
				// }
				
				// ImGui::SetNextItemWidth(60.0f);
				// if (ImGui::InputInt("Joystick Index", &paddle_device_index)){
				// 	if (paddle_device_index < 0) paddle_device_index = 0; // Prevent negative indices
				// 	SavePreferences();
				// 	paddleDetected = false;
				// 	if (joysticks != nullptr) joysticks->SetHeldButtons(0); // Prevent stuck inputs
				// 	PaddleInit();
				// }
				
				// ImGui::SetNextItemWidth(60.0f);
				// if (ImGui::InputInt("Joystick Axis", &paddle_axis_index)){
				// 	if (paddle_axis_index < 0) paddle_axis_index = 0; // Prevent negative indices
				// 	SavePreferences();
				// }

				if(ImGui::BeginMenu("Pallete")) {
					ImGui::RadioButton("Unscaled Capture", &palette_select, PALETTE_SELECT_CAPTURE);
					ImGui::RadioButton("Full Contrast", &palette_select, PALETTE_SELECT_SCALED);
					ImGui::RadioButton("Cheap HDMI converter", &palette_select, PALETTE_SELECT_HDMI);
					ImGui::RadioButton("Flawed Theory (Legacy)", &palette_select, PALETTE_SELECT_OLD);
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
			}
			if(ImGui::BeginMenu("Tools")) {
				if(ImGui::MenuItem("Profiler (F12)")) {
					toggleProfilerWindow();
				}
				if(ImGui::MenuItem("Memory Browser (F9)")) {
					toggleMemBrowserWindow();
				}
				if(ImGui::MenuItem("VRAM Viewer (F10)")) {
					toggleVRAMWindow();
				}
				if(ImGui::MenuItem("Code Stepper (F7)")) {
					toggleSteppingWindow();
				}
				if(ImGui::MenuItem("Patching Window")) {
					togglePatchingWindow();
				}
				if(ImGui::MenuItem("Update Patches")) {
					gameconfig->UpdateAllPatches(cartridge_state.rom);
				}
				if(ImGui::MenuItem("Dump RAM to file (F6)")) {
					doRamDump();
				}
				if(ImGui::MenuItem("Deep Profile Single Vsync")) {
					vsyncProfileArmed = true;
				}
				ImGui::EndMenu();
			}
			ImGui::EndMainMenuBar();
		}
#else
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0.5f)); // 50% transparent black
		ImGui::Begin("OverlayBackground", nullptr,
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoInputs | 
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoBringToFrontOnFocus);
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);

		// Now create the actual menu window in the top-left corner
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(0.9f);
		if(!ImGui::IsAnyItemFocused()) {
			ImGui::SetNextWindowFocus();
		}
		menuOpening = false;
		ImGui::Begin("MainMenu", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoTitleBar);

			ImGui::SetWindowFontScale(2.0f);

		if (ImGui::IsWindowAppearing())
    		ImGui::SetKeyboardFocusHere(-1);
		

		if (ImGui::BeginMenu("Options")) {
			// These are items inside the pop-out menu
			if (ImGui::MenuItem("Toggle Full Screen")) {
				toggleFullScreen();
			}
			ImGui::SliderInt("Volume", &AudioCoprocessor::singleton_acp_state->volume, 0, 256, "", ImGuiSliderFlags_NoInput);
			bool appMute = (muteMask & MUTE_SOURCE_MANUAL) != 0;
			ImGui::Checkbox("Mute Audio", &appMute);
			if(appMute) muteMask |= MUTE_SOURCE_MANUAL;
			else muteMask &= ~MUTE_SOURCE_MANUAL;
			AudioCoprocessor::singleton_acp_state->isMuted = (muteMask != 0);
			ImGui::Separator();
			if (ImGui::Checkbox("Mouse Paddle", &paddle_emulation_enabled)) {//hidden from wrapper mode
				SavePreferences();
				joysticks->SetHeldButtons(0);//clear bits on change just in case
			}
			if (ImGui::Checkbox("NTSC Filter", &ntsc_filter_enabled)) {
				SavePreferences();
			}
			if (ImGui::SliderFloat("NTSC Color Shift",&ntsc_color_shift,0.05f,2.0f, "%.2f")){
				SavePreferences();
			}
			if (ImGui::SliderFloat("NTSC Resolution Scale",&ntsc_res_scale,1.0f,4.0f, "%.1f")){
				ntsc_res_scale = (ntsc_res_scale <= 4) ? (ntsc_res_scale = (ntsc_res_scale > 0) ? ntsc_res_scale : 1.0f) : 4.0f;
				SavePreferences();
			}
			if (ImGui::Checkbox("NTSC Bloom", &ntsc_bloom_enabled)){
				SavePreferences();
			}
			if (ImGui::SliderFloat("NTSC Bloom Decay",&ntsc_bloom_decay,0.05f,0.95f, "%.2f")){
				SavePreferences();
			}
			if (ImGui::Checkbox("Enable Phosphor Blending", &phosphor_blending_enabled)) {
				SavePreferences();
			}

			// if (ImGui::Checkbox("Use Any Joystick As Paddle", &use_any_joystick_as_paddle)){
			// 	SavePreferences();
			// 	paddleDetected = false;
			// 	PaddleInit();
			// }
			// ImGui::SetNextItemWidth(60.0f);
			// if (ImGui::InputInt("Joystick Index", &paddle_device_index)){
			// 	if (paddle_device_index < 0) paddle_device_index = 0; // Prevent negative indices
			// 	SavePreferences();
			// 	paddleDetected = false;
			// 	if (joysticks != nullptr) joysticks->SetHeldButtons(0); // Prevent stuck inputs
			// 	PaddleInit();
			// }
			// ImGui::SetNextItemWidth(60.0f);
			// if (ImGui::InputInt("Joystick Axis", &paddle_axis_index)){
			// 	if (paddle_axis_index < 0) paddle_axis_index = 0; // Prevent negative indices
			// 	SavePreferences();
			// }

			ImGui::EndMenu();
		}

		if(ImGui::Selectable("Reset")) {
			resetQueued = 2;
			showMenu = false;
			setMenuMute(showMenu);
			joysticks->Reset();
		}

		if(ImGui::Selectable("Exit")) {
			running = false;
		}

		ImGui::End();
#endif
	}
	ImGui::Render();
	ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), mainRenderer);
#endif
	SDL_RenderPresent(mainRenderer);
}

char titlebuf[256];
int32_t intended_cycles = 0;

#ifdef WASM_BUILD
double target_frame_period_ms = 1000.0 / 60.0;
double last_raf_time = 0;
double frame_time_accumulator = 0;
#endif

EM_BOOL mainloop(double time, void* userdata) {
#ifdef WASM_BUILD
        double delta_time = time - last_raf_time;
        frame_time_accumulator += delta_time;
        last_raf_time = time;
        if(frame_time_accumulator < target_frame_period_ms) {
                return true;
        }
        frame_time_accumulator -= target_frame_period_ms;
#else
//UpdatePaddleStatus();//lazy dev checker //is a bad idea to run this periodically

if (romRequestedPaddle){ //master switch for paddle behavior
	//fallback to paddle emulation if no paddle is detected
	if (paddle_emulation_enabled || !paddleDetected) { //mouse paddle emulation, overrides joystick behavior
		if (paddle_touch_mode){ //touch / absolute
			// Fallback to mouse if hardware isn't plugged in
			int mx, my, winW, winH;
			SDL_GetMouseState(&mx, &my);
			SDL_GetWindowSize(mainWindow, &winW, &winH);
			joysticks->UpdatePaddleFromCursorPos(0, mx, winW);
		} else { //mouse mode delta
			if (showMenu) {
				if (SDL_GetRelativeMouseMode()) SDL_SetRelativeMouseMode(SDL_FALSE);
			} else {
				// Not in menu? Ensure the mouse is captured
				if (!SDL_GetRelativeMouseMode()) SDL_SetRelativeMouseMode(SDL_TRUE);
				
				int dx, dy;
				SDL_GetRelativeMouseState(&dx, &dy);
				joysticks->UpdatePaddleFromMouse(0, dx);
			}
		}
	}//paddle emulation
	else if (paddleDetected) {
		// We treat the full joystick range as our "Window Width"
		// Logical range of SDL Axis is 65535 units wide
		const int virtualWidth = 65535;
		
		// Offset the raw value (-32768 to 32767) to be 0 to 65535
		int normalizedX = currentPaddleRawValue + 32768;

		joysticks->UpdatePaddleFromCursorPos(0, normalizedX, virtualWidth);
	} 
	// else {
	// 	if(SDL_GetRelativeMouseMode()) SDL_SetRelativeMouseMode(SDL_FALSE);
	// }
}
#endif


#ifdef WRAPPER_MODE
	if(!paused && !showMenu) {
#else
	if(!paused) {
#endif
			timekeeper.actual_cycles = timekeeper.totalCyclesCount;
#ifndef WASM_BUILD
			switch(timekeeper.clock_mode) {
				case CLOCKMODE_NORMAL:
					cpu_core->freeze = false;
					intended_cycles = timekeeper.cycles_per_vsync;
					break;
				case CLOCKMODE_SINGLE:
					cpu_core->freeze = false;
					Disassembler::Decode(MemoryReadResolve, loadedMemoryMap, cpu_core->pc, 32);
					intended_cycles = 1;
					timekeeper.clock_mode = CLOCKMODE_STOPPED;
					break;
				case CLOCKMODE_STOPPED:
					intended_cycles = 0;
					break;
			}
			if(intended_cycles) {
				cpu_core->Run(intended_cycles, timekeeper.totalCyclesCount);
			}
#else
			intended_cycles = timekeeper.cycles_per_vsync;
			cpu_core->Run(intended_cycles, timekeeper.totalCyclesCount);
#endif
			timekeeper.actual_cycles = timekeeper.totalCyclesCount - timekeeper.actual_cycles;
			if(cpu_core->illegalOpcode) {
				printf("Hit illegal opcode %x\npc = %x\n", cpu_core->illegalOpcodeSrc, cpu_core->pc);
				PauseEmulation();
			} else if((timekeeper.clock_mode == CLOCKMODE_NORMAL) && (timekeeper.actual_cycles == 0)) {
				profiler.zeroConsec++;
				if(profiler.zeroConsec == 10) {
					printf("(Got stuck at 0x%x)\n", cpu_core->pc);
					PauseEmulation();
				}
				timekeeper.totalCyclesCount += intended_cycles;
			} else {
				profiler.zeroConsec = 0;
			}

#ifndef WASM_BUILD
			if(!gofast) {
				SDL_Delay(timekeeper.time_scaling * intended_cycles/timekeeper.system_clock);
			} else {
				timekeeper.lastTicks = 0;
			}
			timekeeper.currentTicks = SDL_GetTicks();

			if(timekeeper.clock_mode == CLOCKMODE_NORMAL) {
				if(timekeeper.lastTicks != 0) {
					int time_error = (timekeeper.currentTicks - timekeeper.lastTicks) - (1000 * intended_cycles/timekeeper.system_clock);
					if(timekeeper.frameCount == 100) {
#ifndef WRAPPER_MODE						
					  sprintf(titlebuf, "%s | %s | s: %.1f inc: %.1f err: %d\n", WINDOW_TITLE, currentRomFilePath.c_str(), timekeeper.time_scaling, timekeeper.scaling_increment, time_error);
						SDL_SetWindowTitle(mainWindow, titlebuf);
#endif
						profiler.fps = profiler.bufferFlipCount * 60 / 100;
						timekeeper.frameCount = 0;
						profiler.bufferFlipCount = 0;
					}
					bool overlong = time_error > 0;

					if(overlong == timekeeper.prev_overlong) {
						//scaling_increment = 1;
					} else if(timekeeper.scaling_increment > 1) {
						timekeeper.scaling_increment -= 1;
					}
					if((timekeeper.scaling_increment > 1) || (abs(time_error) > 2)) {
						if(overlong) {
							timekeeper.time_scaling -= timekeeper.scaling_increment;
						} else {
							timekeeper.time_scaling += timekeeper.scaling_increment;
						}
					}
					timekeeper.prev_overlong = overlong;

					if(timekeeper.time_scaling < 100) {
						timekeeper.time_scaling = 100;
					} else if(timekeeper.time_scaling > 2000) {
						timekeeper.time_scaling = 2000;
					}
				}
				timekeeper.lastTicks = timekeeper.currentTicks;
				timekeeper.frameCount++;
			}
#endif
			timekeeper.totalCyclesCount -= timekeeper.actual_cycles;
			timekeeper.totalCyclesCount += intended_cycles;
			timekeeper.cycles_since_vsync += intended_cycles;
			if(timekeeper.cycles_since_vsync >= timekeeper.cycles_per_vsync) {
				timekeeper.cycles_since_vsync -= timekeeper.cycles_per_vsync;
				if(system_state.dma_control & DMA_VSYNC_NMI_BIT) {
					cpu_core->NMI();
					if(vsyncProfileArmed) {
						profiler.DeepProfileStart();
						vsyncProfileArmed = false;
						vsyncProfileRunning = true;
					} else if(vsyncProfileRunning) {
						profiler.DeepProfileStop(loadedMemoryMap, SourceMap::singleton);
						vsyncProfileRunning = false;
					}
				}
				if(!profiler.measure_by_frameflip) {
					profiler.ResetTimers();
					profiler.last_blitter_activity = blitter->pixels_this_frame;
					blitter->pixels_this_frame = 0;
				}
			}
		} else {
				SDL_Delay(16);
		}
		blitter->CatchUp();
		

		if(EmulatorConfig::noSound) {
			AudioCoprocessor::fill_audio(AudioCoprocessor::singleton_acp_state, NULL, AudioCoprocessor::singleton_acp_state->samples_per_frame);
		}

		while( SDL_PollEvent( &e ) != 0 )
        {
#ifndef WASM_BUILD

#ifdef WRAPPER_MODE
			if(true){
#else
			if(SDL_GetMouseFocus() == mainWindow) {
#endif
				ImGui::SetCurrentContext(main_imgui_ctx);
				ImPlot::SetCurrentContext(main_implot_ctx);
				ImGui_ImplSDL2_ProcessEvent(&e);
			}
			for (auto toolWindow : toolWindows) {
				toolWindow->HandleEvent(e);
			}

			ImGui::SetCurrentContext(main_imgui_ctx);
			ImPlot::SetCurrentContext(main_implot_ctx);

#ifndef WRAPPER_MODE
			if(ImGui::GetIO().WantCaptureKeyboard && ((e.type == SDL_KEYDOWN) || (e.type == SDL_KEYUP))) {
				continue;
			}
#endif //WRAPPER_MODE
#endif //WASM_BUILD
            //User requests quit
            if( e.type == SDL_QUIT )
            {
               running = false;
            } else if(e.type == SDL_WINDOWEVENT)
			{
				if(e.window.event == SDL_WINDOWEVENT_CLOSE) {
					SDL_Window* closedWindow = SDL_GetWindowFromID(e.window.windowID);
					if(closedWindow == mainWindow) {
						running = false;
					}
				}
				else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
					SDL_SetRelativeMouseMode(SDL_FALSE);
				} 
				else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
					if (paddle_emulation_enabled && !paddle_touch_mode) {
						SDL_SetRelativeMouseMode(SDL_TRUE);
					}
				}

			} else if((e.type == SDL_KEYDOWN) || (e.type == SDL_KEYUP)) {
				if(e.key.repeat == 0) {
					if((e.type == SDL_KEYUP) || !checkHotkey(e.key.keysym.sym)) {
						switch(e.key.keysym.sym) {
							case SDLK_LSHIFT:
								lshift = (e.type == SDL_KEYDOWN);
								break;
							case SDLK_RSHIFT:
								rshift = (e.type == SDL_KEYDOWN);
								break;							
							case SDLK_ESCAPE:
	#if !defined(DISABLE_ESC)
								if(e.type == SDL_KEYDOWN) {
									showMenu = !showMenu;
									menuOpening = showMenu;
	#ifdef WRAPPER_MODE
									setMenuMute(showMenu);
	#endif
								}
								#endif
								break;
	#ifndef WRAPPER_MODE
							case SDLK_BACKQUOTE:
								gofast = (e.type == SDL_KEYDOWN);
								break;
							case SDLK_r:
								//TODO add menu item for reset
								if(e.type == SDL_KEYDOWN) {
									if(lshift || rshift) {
										resetQueued = 2;
									} else {
										resetQueued = 1;
									}
								}
								break;
							case SDLK_o:
								if(e.type == SDL_KEYDOWN) {
									const char* rom_file_name = open_rom_dialog();
									if(rom_file_name) {
										LoadRomFile(rom_file_name);
									} else {
	#ifdef TINYFILEDIALOGS_H
										tinyfd_notifyPopup("Alert",
										"No ROM was loaded",
										"warning");
	#endif
									}
								}
								break;
	#endif
							default:
								joysticks->update(&e, showMenu || resetQueued);
								break;
						}
					}
				}
            } else if (e.type == SDL_JOYAXISMOTION) {
				#ifdef WASM_BUILD
				if (e.jaxis.axis == paddle_axis_index) {
					if (use_any_joystick_as_paddle || e.jaxis.which == dksPaddle_instanceID) {
						currentPaddleRawValue = e.jaxis.value; 
					}
				}
				#else
				if (paddleDetected && e.jaxis.axis == paddle_axis_index && !paddle_emulation_enabled) {
					if (/*use_any_joystick_as_paddle || */ e.jaxis.which == dksPaddle_instanceID) {
						currentPaddleRawValue = e.jaxis.value;
					}               
				}
				#endif
            } else if (e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP) {
				//printf("Button Press: %d\n", e.jbutton.button);

                if (paddleDetected && e.jbutton.button == 0) {

					bool isDown = (e.type == SDL_JOYBUTTONDOWN);
					
					joysticks->SetPaddleAButtonDirect(isDown);

                }
			 } else if (e.type == SDL_JOYDEVICEREMOVED) {
				if (paddleDetected && e.jdevice.which == dksPaddle_instanceID) {
					paddleDetected = false;
					dksPaddle_instanceID = -1; // Reset it
					printf("Paddle/JoyStick Disconnected\n");
				}
				PaddleInit();
			} else if (e.type == SDL_JOYDEVICEADDED) {
				PaddleInit();
            } else {
				joysticks->update(&e, showMenu || resetQueued);
			}
        }

		if(joysticks->CheckSystemButtonPressed()) {
	#if !defined(DISABLE_ESC)
									showMenu = !showMenu;
									menuOpening = showMenu;
	#ifdef WRAPPER_MODE
									setMenuMute(showMenu);
	#endif
	#endif
		}

		refreshScreen();
		SDL_UpdateWindowSurface(mainWindow);

#ifndef WASM_BUILD
		for (auto& window : toolWindows) {
			if(window->IsOpen()) {
				window->Draw();
			}
		}

		auto const to_be_removed = std::partition(begin(toolWindows), end(toolWindows), [](auto w){ return w->IsOpen(); });
		std::for_each(to_be_removed, end(toolWindows), [](auto w) {
			delete w;
		});
		toolWindows.erase(to_be_removed, end(toolWindows));
#endif
		
	if(!running) {
#ifdef WASM_BUILD
		emscripten_cancel_main_loop();
#else
		for (auto& window : toolWindows) {
			delete window;
		}
		toolWindows.clear();

		ImGui::SetCurrentContext(main_imgui_ctx);
		ImPlot::SetCurrentContext(main_implot_ctx);
		ImPlot::DestroyContext(main_implot_ctx);
		ImGui_ImplSDLRenderer2_Shutdown();
    	ImGui_ImplSDL2_Shutdown();
    	ImGui::DestroyContext(main_imgui_ctx);
#endif
    	SDL_DestroyRenderer(mainRenderer);
		SDL_DestroyWindow(mainWindow);
	}

	if(resetQueued) {
		ResumeEmulation();
		if(lshift || rshift || (resetQueued == 2)) {
			randomize_memory();
			randomize_vram();
		}
		cpu_core->Reset();
		cartridge_state.write_mode = false;
		joysticks->Reset();
		resetQueued = 0;
		joysticks->SetHeldButtons(0);
		currentPaddleRawValue = 0;
#ifndef WASM_BUILD
		//paddle_emulation_enabled = false;//set by user as an override
		//paddleDetected = false; //reset shouldn't affect this
		//dksPaddle_instanceID = -1; // Reset it //reset shouldn't affect this
#endif
	}
	return running;
}

int main(int argC, char* argV[]) {
	srand(time(NULL));
	LoadPreferences();
	cartridge_state.rom = new uint8_t[1 << 21];

	const char* rom_file_name = NULL;

#ifdef EMBED_ROM_FILE
	rom_file_name = EMBED_ROM_FILE;
#else
	for(int argIdx = 1; argIdx < argC; ++argIdx) {
		if((argV[argIdx])[0] == '-') {
			EmulatorConfig::parseArg(argV[argIdx]);
		} else if(!rom_file_name) {
			rom_file_name = argV[argIdx];
		}
	}

	// Brick Game FALLBACK: If no ROM was specified via command line, use the default local path
    if (rom_file_name == NULL) {
        rom_file_name = (char*)"roms/brickgame.gtr";
    }

#endif

#ifdef DEFAULT_ROM_PATH
	if(argC == 1) {
		int execPathLength = wai_getExecutablePath(NULL, 0, NULL);
		if(execPathLength != -1) {
			char* path = (char*)malloc(execPathLength + 1);
			wai_getExecutablePath(path, execPathLength, NULL);
			path[execPathLength] = '\0';
			std::filesystem::path execPath(path);
			free(path);
			std::filesystem::path romPath = execPath.parent_path() / DEFAULT_ROM_PATH;
			std::string romPathStr = (execPath.parent_path() / DEFAULT_ROM_PATH).string();
			rom_file_name = strdup(romPathStr.c_str());
		}
	}
#endif

	//cartridge_state.rom = new uint8_t [cartridge_state.size];
		for(int i = 0; i < cartridge_state.size; i++) {
			cartridge_state.rom[i] = 0;
		}

	
	soundcard = new AudioCoprocessor();
	cpu_core = new mos6502(MemoryRead, MemoryWrite, CPUStopped, MemorySync);
	cpu_core->Reset();
	cartridge_state.write_mode = false;
	blitter = new Blitter(cpu_core, &timekeeper, &system_state, vRAM_Surface);
	randomize_memory();
	
	SDL_Init(SDL_INIT_VIDEO);
	atexit(SDL_Quit);

	bmpFont = SDL_CreateRGBSurfaceFrom(font_map, 128, 128, 32, 4 * 128, rmask, gmask, bmask, amask);

	vRAM_Surface = SDL_CreateRGBSurface(0, GT_WIDTH, GT_HEIGHT * 2, 32, rmask, gmask, bmask, amask);
	gRAM_Surface = SDL_CreateRGBSurface(0, GT_WIDTH, GT_HEIGHT * 32, 32, rmask, gmask, bmask, amask);

	SDL_SetColorKey(vRAM_Surface, SDL_FALSE, 0);
	SDL_SetColorKey(gRAM_Surface, SDL_FALSE, 0);

	mainWindow = SDL_CreateWindow(WINDOW_TITLE, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	mainRenderer = SDL_CreateRenderer(mainWindow, -1, EmulatorConfig::defaultRendererFlags);
	framebufferTexture = SDL_CreateTexture(mainRenderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, GT_WIDTH, GT_HEIGHT * 2);

#ifndef WASM_BUILD
	main_imgui_ctx = ImGui::CreateContext();
	main_implot_ctx = ImPlot::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigViewportsNoAutoMerge = true;
	io.IniFilename = NULL;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	io.Fonts->Flags |= ImFontAtlasFlags_NoBakedLines;
	ImGui::StyleColorsDark();
	ImGui_ImplSDL2_InitForSDLRenderer(mainWindow, mainRenderer);
	ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual);
	ImGui_ImplSDLRenderer2_Init(mainRenderer);
#endif

	//Init joystick handler AFTER init imgui
	joysticks = new JoystickAdapter();

	#if SDL_BYTEORDER == SDL_BIG_ENDIAN
	    rmask = 0xff000000;
	    gmask = 0x00ff0000;
	    bmask = 0x0000ff00;
	    amask = 0x000000ff;
	#else
	    rmask = 0x000000ff;
	    gmask = 0x0000ff00;
	    bmask = 0x00ff0000;
	    amask = 0xff000000;
	#endif

	randomize_vram();

	if(!rom_file_name || LoadRomFile(rom_file_name) == -1) {
		PauseEmulation();
#ifdef TINYFILEDIALOGS_H
		if(rom_file_name) {
			tinyfd_notifyPopup("Alert",
			"No ROM was loaded",
			"warning");
		}
#endif
		
	} else {
		ResumeEmulation();
	}

#ifdef WASM_BUILD

	emscripten_request_animation_frame_loop(mainloop, 0);
#else
	PaddleInit();
	SDL_RaiseWindow(mainWindow);
	while(running) {
		mainloop(0, NULL);
	}
	joysticks->SaveBindings();
#endif

#ifndef WASM_BUILD
	if(savingThread.joinable()) {
		savingThread.join();
	}
#endif
	return 0;
}
