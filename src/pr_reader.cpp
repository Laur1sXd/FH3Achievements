// SPDX-License-Identifier: GPL-3.0-only
#include "pr_reader.hpp"
#include "events.hpp"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>

namespace ach {
namespace {
const uint8_t* prGame = nullptr;
FH3Hook* prHook = nullptr;
using NativeScore = void (*)(void*,float,float);
void* originalScore = nullptr;
template<class T> std::optional<T> memory(uintptr_t address) {
    if (!address) return {};
    T result{}; SIZE_T bytes=0;
    if (!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&result,sizeof result,&bytes) || bytes!=sizeof result) return {};
    return result;
}
uintptr_t pointer(uintptr_t address) { return memory<uintptr_t>(address).value_or(0); }
bool method(uintptr_t object, int offset, uintptr_t rva) {
    return pointer(pointer(object)+offset)==reinterpret_cast<uintptr_t>(prGame)+rva;
}
std::optional<std::string> nativeString(uintptr_t address) {
    const auto size=memory<size_t>(address+16), capacity=memory<size_t>(address+24);
    if (!size || !capacity || !*size || *size>100 || *size>*capacity || *capacity>16384) return {};
    const auto data=*capacity>=16?pointer(address):address;
    std::string result(*size,'\0'); SIZE_T bytes=0;
    if (!data || !ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(data),result.data(),result.size(),&bytes) || bytes!=result.size()) return {};
    if (result.find('\0')!=result.npos) return {};
    return result;
}
struct CarContext { int world=0, car=0, carClass=-1; float kilograms=0; };
std::optional<CarContext> currentCar() {
    const auto base=reinterpret_cast<uintptr_t>(prGame);
    const auto world=memory<int>(pointer(base+0x4e6cc60)+8);
    const auto services=pointer(base+0x4d6fc00);
    if (!world || (*world!=552 && *world!=553 && *world!=554) || !method(services,0x48,0x3c17e0)) return {};
    const auto mode=memory<uint8_t>(services+0x34);
    const auto count=memory<size_t>(services+0x40);
    if (!mode || !count || *mode>1 || *count<(*mode?1u:2u) || *count>16) return {};
    auto entry=pointer(pointer(services+0x38));
    if (!*mode) entry=pointer(entry);
    const auto player=pointer(entry+0x10);
    if (!method(player,0xc0,0x3c2480) || !method(player,0xe8,0x3c2490) || !method(player,0x118,0x3c1be0)) return {};
    const auto state=pointer(player+0x9a0), data=pointer(state+0x110);
    const auto car=memory<int>(data);
    if (!state || !car || *car<=0 || *car>100000) return {};
    CarContext result{*world,*car,-1,0};
    const auto save=pointer(player+0x9c0);
    const auto info=save+0xb90;
    if (method(info,0x60,0x320b40)) {
        const auto value=memory<int>(info+0x84);
        if (value && *value>=0 && *value<=6) result.carClass=*value;
        else if (value && *value==-1) {
            const auto liveClass=memory<int>(save+0x28);
            if (liveClass && *liveClass>=0 && *liveClass<=6) result.carClass=*liveClass;
        }
    }
    const auto physics=pointer(state+0x71e8);
    if (physics>=0x4a8 && method(physics,0x3a0,0x1d41140)) {
        const auto mass=memory<float>(physics-0x4a8);
        if (mass && std::isfinite(*mass) && *mass>0 && *mass<10000) result.kilograms=*mass*100.f;
    }
    return result;
}
struct Captured { CarContext car; std::string name; float gold; };
std::optional<Captured> capture(void* component) {
    const auto base=reinterpret_cast<uintptr_t>(prGame);
    // The four native PR completion paths call AmbientGameplayRewards::OnScore.
    // Its MedalData reference points to the same entity shown in the game's HUD.
    const auto medal=pointer(reinterpret_cast<uintptr_t>(component)+0x1f0);
    if (pointer(medal)!=base+0x42fa440 || nativeString(medal+0x50)!="medal_data") return {};
    const auto owner=pointer(medal+0x40);
    if (pointer(owner)!=base+0x4314958) return {};
    const auto name=nativeString(owner+0x48);
    const auto gold=memory<float>(medal+0x118);
    const auto car=currentCar();
    if (!name || !gold || !car) return {};
    return Captured{*car,*name,*gold};
}
void onScore(void* component,float score,float previous) {
    std::optional<Captured> event;
    try { if (std::isfinite(score) && score>=0) event=capture(component); } catch (...) {}
    reinterpret_cast<NativeScore>(originalScore)(component,score,previous);
    if (event) try {
        const auto& car=event->car;
        recordPrResult(car.world,event->name,car.car,car.carClass,car.kilograms,score,event->gold);
    } catch (...) {}
}
}
bool installPrReader(const FH3ModApi* api,FH3Mod* mod,const uint8_t* game) {
    if (!api->hook_function || !api->enable_hooks) return false;
    prGame=game;
    if (!prHook) prHook=api->hook_function(mod,"Native PR stunt results",const_cast<uint8_t*>(game)+0x4949d0,
                                         reinterpret_cast<void*>(onScore),&originalScore);
    return prHook && originalScore && api->enable_hooks(mod,prHook);
}
void stopPrReader(const FH3ModApi* api,FH3Mod* mod) { if (prHook) api->disable_hooks(mod,prHook); }
}
