// SPDX-License-Identifier: GPL-3.0-only
#include "../src/events.cpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace {
using namespace ach;
constexpr uintptr_t Base = 0x140000000;
unsigned forwarded = 0;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
uint32_t nativeWrite(const void*,const void*,uint64_t,uint32_t,const Descriptor*) { ++forwarded; return 42; }
FH3Hook* hookPointer(FH3Mod*,const char*,void**,void*,void** next) {
    *next = reinterpret_cast<void*>(nativeWrite); return reinterpret_cast<FH3Hook*>(1);
}
int toggle(FH3Mod*,FH3Hook*) { return 1; }
struct Payload {
    std::array<int32_t,32> numbers{};
    std::array<Descriptor,32> fields{};
    GUID round{};
    float seconds = 100, multiplier = 1;
    std::wstring text;
    Payload() {
        static uint32_t sequence = 1;
        round.Data1 = sequence++;
        for (size_t i=0;i<fields.size();++i) fields[i]={reinterpret_cast<uint64_t>(&numbers[i]),4,0};
        fields[7]={reinterpret_cast<uint64_t>(&round),sizeof round,0};
        fields[10]={reinterpret_cast<uint64_t>(&seconds),sizeof seconds,0};
        numbers[8]=2751; numbers[9]=2; numbers[11]=1;
    }
    void string(int index, const wchar_t* name) {
        text=name; fields[index]={reinterpret_cast<uint64_t>(text.c_str()),static_cast<uint32_t>((text.size()+1)*2),0};
    }
    void emit(uintptr_t rva,uint32_t count) {
        require(eventWrite(reinterpret_cast<void*>(Base+rva),nullptr,0,count,fields.data())==42,"native forwarding changed");
    }
};
}
int main() {
    using namespace ach;
    try {
        FH3ModApi api{}; api.hook_pointer=hookPointer; api.enable_hooks=toggle; api.disable_hooks=toggle;
        auto* owner=reinterpret_cast<FH3Mod*>(1);
        require(installEventReader(&api,owner,reinterpret_cast<uint8_t*>(Base)),"event hook missing");
        EventBatch totals;
        Payload mountain; mountain.numbers[26]=553; mountain.numbers[19]=2022;
        mountain.emit(0x4461ef8,32); mountain.emit(0x4461ef8,32); mountain.emit(0x4463ac0,13);
        mergeEventBatch(totals,takeEventBatch());
        require(totals.counters[83]==1 && totals.counters[108]==0 && totals.counters[135]==1,
                "mountain map routing or race GUID de-duplication failed");
        Payload wrongMap; wrongMap.numbers[26]=552; wrongMap.numbers[19]=2022;
        wrongMap.emit(0x4461ef8,32);
        Payload wrongRoute; wrongRoute.numbers[26]=553; wrongRoute.numbers[19]=528;
        wrongRoute.emit(0x4461ef8,32);
        Payload island; island.numbers[26]=554; island.numbers[19]=528; island.emit(0x4461ef8,32);
        Payload coop; coop.numbers[26]=553; coop.numbers[19]=2022; coop.numbers[25]=1; coop.emit(0x4461ef8,32);
        for (int exit : {0,1,3,4,5}) {
            Payload quit; quit.numbers[26]=554; quit.numbers[19]=528; quit.numbers[9]=exit; quit.emit(0x4461ef8,32);
        }
        Payload rivals; rivals.numbers[26]=554; rivals.numbers[19]=528; rivals.numbers[12]=1; rivals.emit(0x4461ef8,32);
        mergeEventBatch(totals,takeEventBatch());
        require(totals.counters[83]==1 && totals.counters[84]==1 && totals.counters[108]==1 &&
                totals.counters[39]==1 && totals.counters[40]==1,"DLC/Co-op gates counted a wrong map, wrong route, Rivals or quit");
        for (int classId : {0,0,1,2,3,4,5,6,7}) {
            Payload drag; drag.numbers[26]=552; drag.numbers[19]=3011; drag.numbers[20]=classId;
            drag.emit(0x4461ef8,32);
        }
        for (int car : {2728,2728,2729,2730,2732,2733,2734,2736,2751}) {
            Payload championship; championship.numbers[9]=car;
            championship.fields[7]={reinterpret_cast<uint64_t>(&championship.numbers[7]),4,0};
            championship.emit(0x4462470,16);
        }
        mergeEventBatch(totals,takeEventBatch());
        auto progress=initialProgress(); addEventProgress(progress,totals);
        require(progress[45].current==7 && progress[45].complete() && progress[122].current==7 && progress[122].complete(),
                "unique classes/cars were duplicated or included unrelated cars/classes");
        require(!validEvidence(45,7) && !validEvidence(122,2751) && !validEvidence(-1,0),"invalid persisted evidence accepted");
        // Exact routes, cars, map IDs and strict time limits for the DLC challenges.
        for (auto [world,route,car,seconds] : {std::tuple{554,515,2474,120.f}, {553,515,2474,119.f},
                 {554,528,2474,119.f}, {554,515,1175,119.f}, {554,515,2474,119.99f},
                 {553,2022,2648,119.f}, {554,528,1175,389.99f}}) {
            Payload race; race.numbers[22]=world; race.numbers[12]=route; race.numbers[8]=car; race.seconds=seconds;
            race.emit(0x4461f68,26);
            auto batch=takeEventBatch();
            const bool zen=world==554 && route==515 && car==2474 && seconds<120;
            require((batch.counters[114]!=0)==zen,"High Speed Highway accepted a wrong route/car/map or a 2-minute result");
            mergeEventBatch(totals,batch);
        }
        require(totals.counters[85]==1 && totals.counters[114]==1 && totals.counters[116]==1,"DLC time challenge did not complete");
        for (auto [world,car,grade,name] : {std::tuple{552,2663,3,L"Air"}, {553,2663,2,L"Air"},
                 {553,2663,3,L"Air"}, {554,2576,4,L"Drift"}, {554,2742,3,L"LoopTheLoop"},
                 {553,2662,0,L"Snowman"}, {552,2732,3,L"Drift"}, {552,2734,0,L"FruitSalad"}}) {
            Payload skill; skill.string(3,name); skill.numbers[4]=grade; skill.numbers[6]=car; skill.numbers[8]=world;
            skill.emit(0x4463ee8,10);
        }
        mergeEventBatch(totals,takeEventBatch());
        require(totals.counters[89]==1 && totals.counters[112]==1 && totals.counters[113]==1 &&
                totals.counters[88]==1 && totals.counters[127]==1 && totals.counters[124]==1,
                "skill type/grade/car/map validation failed");
        // Blueprint difficulty and tyre conditions must be accepted together, not individually.
        for (int difficulty : {5,6,7}) {
            Payload race; race.numbers[4]=1; race.numbers[5]=1; race.numbers[6]=25;
            race.numbers[7]=difficulty; race.fields[7]={reinterpret_cast<uint64_t>(&race.numbers[7]),4,0};
            race.numbers[8]=1; race.emit(0x44629b0,15);
            auto batch=takeEventBatch(); require((batch.counters[51]!=0)==(difficulty==6),"Endurance Hero accepted the wrong difficulty");
        }
        for (int tyres : {3,7,-1}) {
            Payload race; race.numbers[3]=1; race.numbers[14]=553; race.numbers[16]=tyres;
            race.emit(0x4463510,19);
            require((takeEventBatch().counters[92]!=0)==(tyres==3),"snow/unknown tyres were accepted for Gliding It Home");
        }
        Payload stars; stars.numbers[16]=552; stars.numbers[20]=3; stars.emit(0x44634d8,29);
        require(!takeEventBatch().counters[67],"mountain stars counted on Australia");
        stars.numbers[16]=553; stars.emit(0x44634d8,29);
        require(takeEventBatch().counters[67]==1,"verified mountain three-star finish missing");
        stars.numbers[16]=554; stars.numbers[9]=2022; stars.emit(0x44634d8,29);
        require(!takeEventBatch().counters[96],"Hot Wheels medals accepted a mountain route");
        stars.numbers[9]=528; stars.emit(0x44634d8,29);
        require(takeEventBatch().counters[96]==1,"three Hot Wheels race medals missing");
        for (int mode : {4,5,26,10}) {
            Payload infected; infected.numbers[8]=2728; infected.numbers[15]=mode; infected.numbers[18]=1;
            infected.emit(0x4461ef8,32);
            auto batch=takeEventBatch(); require((batch.counters[125]!=0)==(mode==5||mode==26),"another Playground mode counted as Infected");
        }
        // Malformed strings and inaccessible payloads must be ignored and forwarded.
        Payload malformed; malformed.string(3,L"Air"); malformed.fields[3].size=511; malformed.numbers[6]=2663; malformed.numbers[8]=553;
        malformed.emit(0x4463ee8,10); require(takeEventBatch().empty(),"malformed skill payload counted");
        stopEventReader(&api,owner);
        const auto before=forwarded;
        island.emit(0x4461ef8,32);
        require(takeEventBatch().empty() && forwarded==before+1,"Off collected progress or swallowed a game event");
        std::cout<<"PASS: DLC map + route isolation, duplicate rounds, quits/Rivals, Co-op, unique classes/Hoonigan cars, exact timed routes, skill car/grade/map guards, malformed payloads and Off forwarding.\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
